// waywallen-renderer — wescene (Wallpaper Engine scene) host subprocess.
//
// Spawned by the Rust waywallen daemon with a preconnected Unix-domain
// socket passed via --ipc. On startup:
//
//   1. Connect to the socket via waywallen-bridge.
//   2. Construct a SceneWallpaper in offscreen mode so every frame
//      lands in the ExSwapchain's triple-buffered DMA-BUF images.
//   3. On the first redraw callback, snapshot all three ExHandles,
//      fill a ww_evt_bind_buffers_t, and send it with the three
//      DMA-BUF fds attached.
//   4. On every subsequent redraw callback, send ww_evt_frame_ready_t
//      with a single acquire sync_fd (currently a signaled eventfd
//      placeholder — TODO: export a real VkSemaphore sync_file).
//   5. A dedicated reader thread decodes control requests from the
//      daemon and forwards them to SceneWallpaper.
//   6. prctl(PR_SET_PDEATHSIG) so we die with the daemon.
//
// All IPC scaffolding lives in waywallen-bridge; this file only holds
// the wescene-specific Vulkan/scene wiring.

#include <waywallen-bridge/bridge.h>

#include "SceneWallpaper.hpp"
#include "SceneWallpaperSurface.hpp"
#include "Swapchain/ExSwapchain.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <sys/eventfd.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vulkan/vulkan.h>

namespace {

struct Options {
    std::string ipc_path;
    uint32_t    width { 1280 };
    uint32_t    height { 720 };
    std::string initial_scene;
    std::string initial_assets;
    uint32_t    initial_fps { 30 };
    // Test-pattern mode bypasses scene loading: sends bind_buffers as
    // soon as the ExSwapchain is up, then pumps the ring on a host-owned
    // timer, emitting frame_ready per tick. Pixels are whatever the
    // driver left in the allocation — not meaningful, but it exercises
    // the full wire without needing a Wallpaper Engine assets dir.
    bool test_pattern { false };
};

void die(const std::string& msg) {
    std::fprintf(stderr, "waywallen-renderer: %s\n", msg.c_str());
    std::exit(1);
}

Options parse_args(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto        next = [&]() -> std::string {
            if (i + 1 >= argc) die("missing value for " + a);
            return argv[++i];
        };
        if (a == "--ipc") {
            o.ipc_path = next();
        } else if (a == "--width") {
            o.width = static_cast<uint32_t>(std::stoul(next()));
        } else if (a == "--height") {
            o.height = static_cast<uint32_t>(std::stoul(next()));
        } else if (a == "--scene") {
            o.initial_scene = next();
        } else if (a == "--assets") {
            o.initial_assets = next();
        } else if (a == "--fps") {
            o.initial_fps = static_cast<uint32_t>(std::stoul(next()));
        } else if (a == "--test-pattern") {
            o.test_pattern = true;
        } else if (a == "--help" || a == "-h") {
            std::printf(
                "usage: waywallen-renderer --ipc PATH [--width W] [--height H]\n"
                "                          [--scene PKG] [--assets DIR] [--fps N]\n"
                "                          [--test-pattern]\n");
            std::exit(0);
        } else {
            die("unknown argument: " + a);
        }
    }
    if (o.ipc_path.empty()) die("--ipc is required");
    return o;
}

uint64_t now_ns() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

// Create a signaled eventfd to use as a placeholder acquire sync_fd on
// frame_ready. TODO: export a real dma_fence sync_file from the
// SceneWallpaper render queue via VK_KHR_external_semaphore_fd. For
// now a pre-signaled eventfd satisfies the wire contract (one fd per
// frame_ready) and display clients just no-op the wait.
int make_signaled_eventfd() {
    int fd = eventfd(0, EFD_CLOEXEC);
    if (fd < 0) return -1;
    const uint64_t one = 1;
    if (write(fd, &one, sizeof(one)) != sizeof(one)) {
        close(fd);
        return -1;
    }
    return fd;
}

} // namespace


// ---------------------------------------------------------------------------
// Host state — shared between the redraw callback and the reader thread.
// ---------------------------------------------------------------------------

struct HostState {
    int                         sock { -1 };
    wallpaper::SceneWallpaper*  wp { nullptr };

    std::mutex            send_mu; // serializes writes to `sock`
    std::atomic<bool>     bound { false };
    std::atomic<uint64_t> seq { 0 };
    std::atomic<bool>     shutdown { false };
};

static HostState* g_state = nullptr; // redraw_callback has no user-data.


static void send_bind_buffers_locked(HostState& s, wallpaper::ExSwapchain* ex) {
    auto all = ex->snapshot_all_slots();
    if (all[0] == nullptr || all[1] == nullptr || all[2] == nullptr) {
        // Swapchain not fully populated yet; retry next callback.
        return;
    }

    // Sort by id() so image_index N maps to the handle whose id()==N.
    wallpaper::ExHandle* by_id[3] { nullptr, nullptr, nullptr };
    for (auto* h : all) {
        const int32_t id = h->id();
        if (id < 0 || id >= 3) return;
        by_id[id] = h;
    }
    if (!by_id[0] || !by_id[1] || !by_id[2]) return;

    uint64_t sizes[3] = {
        static_cast<uint64_t>(by_id[0]->size),
        static_cast<uint64_t>(by_id[1]->size),
        static_cast<uint64_t>(by_id[2]->size),
    };
    ww_evt_bind_buffers_t bb {};
    bb.count        = 3;
    bb.fourcc       = by_id[0]->drm_fourcc;
    bb.width        = static_cast<uint32_t>(by_id[0]->width);
    bb.height       = static_cast<uint32_t>(by_id[0]->height);
    bb.stride       = by_id[0]->plane0_stride;
    bb.modifier     = by_id[0]->drm_modifier;
    bb.plane_offset = by_id[0]->plane0_offset;
    bb.sizes.count  = 3;
    bb.sizes.data   = sizes;

    int fds[3] = { by_id[0]->fd, by_id[1]->fd, by_id[2]->fd };
    int rc = ww_bridge_send_bind_buffers(s.sock, &bb, fds);
    if (rc != 0) {
        std::fprintf(stderr, "waywallen-renderer: send bind_buffers failed: %d\n", rc);
        s.shutdown.store(true, std::memory_order_release);
        return;
    }
    s.bound.store(true, std::memory_order_release);
}

static void send_frame_ready_locked(HostState& s, wallpaper::ExHandle* frame) {
    int sync_fd = make_signaled_eventfd();
    if (sync_fd < 0) {
        std::fprintf(stderr, "waywallen-renderer: eventfd failed: %s\n", std::strerror(errno));
        s.shutdown.store(true, std::memory_order_release);
        return;
    }

    ww_evt_frame_ready_t fr {};
    fr.image_index = static_cast<uint32_t>(frame->id());
    fr.seq         = s.seq.fetch_add(1, std::memory_order_relaxed);
    fr.ts_ns       = now_ns();

    int rc = ww_bridge_send_frame_ready(s.sock, &fr, sync_fd);
    // sendmsg dup'd the fd into the kernel's message on success; on
    // failure our fd is still ours to close. Either way, close it.
    close(sync_fd);
    if (rc != 0) {
        std::fprintf(stderr, "waywallen-renderer: send frame_ready failed: %d\n", rc);
        s.shutdown.store(true, std::memory_order_release);
    }
}

static void redraw_callback() {
    HostState& s = *g_state;
    if (s.shutdown.load(std::memory_order_acquire)) return;
    if (!s.wp || !s.wp->inited()) return;

    auto* ex = s.wp->exSwapchain();
    if (ex == nullptr) return;

    wallpaper::ExHandle* frame = ex->eatFrame();
    if (frame == nullptr) return;

    std::lock_guard<std::mutex> lock(s.send_mu);

    if (!s.bound.load(std::memory_order_acquire)) {
        send_bind_buffers_locked(s, ex);
    }
    if (!s.bound.load(std::memory_order_acquire)) {
        return;
    }
    send_frame_ready_locked(s, frame);
}


// ---------------------------------------------------------------------------
// Control-plane reader: one thread, blocking ww_bridge_recv_control loop.
// ---------------------------------------------------------------------------

static void apply_control(HostState& s, const ww_bridge_control_t& msg) {
    using namespace wallpaper;

    switch (msg.op) {
    case WW_REQ_HELLO:
        // handshake already implicit on connect
        break;
    case WW_REQ_LOAD_SCENE:
        if (s.wp) {
            s.wp->setPropertyString(PROPERTY_ASSETS, msg.u.load_scene.assets);
            s.wp->setPropertyString(PROPERTY_SOURCE, msg.u.load_scene.pkg);
            s.wp->setPropertyInt32(PROPERTY_FPS,
                                   static_cast<int32_t>(msg.u.load_scene.fps));
        }
        break;
    case WW_REQ_PLAY:
        if (s.wp) s.wp->play();
        break;
    case WW_REQ_PAUSE:
        if (s.wp) s.wp->pause();
        break;
    case WW_REQ_MOUSE:
        if (s.wp) s.wp->mouseInput(msg.u.mouse.x, msg.u.mouse.y);
        break;
    case WW_REQ_SET_FPS:
        if (s.wp) s.wp->setPropertyInt32(
            PROPERTY_FPS, static_cast<int32_t>(msg.u.set_fps.fps));
        break;
    case WW_REQ_SHUTDOWN:
        s.shutdown.store(true, std::memory_order_release);
        break;
    default:
        std::fprintf(stderr, "waywallen-renderer: unknown control op %d\n", msg.op);
        break;
    }
}

static void ipc_reader_loop(HostState& s) {
    while (!s.shutdown.load(std::memory_order_acquire)) {
        ww_bridge_control_t msg {};
        int                 rc = ww_bridge_recv_control(s.sock, &msg);
        if (rc != 0) {
            if (!s.shutdown.load(std::memory_order_acquire)) {
                std::fprintf(stderr,
                             "waywallen-renderer: recv_control failed: %d\n", rc);
            }
            s.shutdown.store(true, std::memory_order_release);
            return;
        }
        apply_control(s, msg);
        ww_bridge_control_free(&msg);
    }
}


// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    Options opts = parse_args(argc, argv);

    ::prctl(PR_SET_PDEATHSIG, SIGTERM);

    HostState state;
    g_state = &state;

    state.sock = ww_bridge_connect(opts.ipc_path.c_str());
    if (state.sock < 0) {
        die("ww_bridge_connect failed: " + std::string { std::strerror(-state.sock) });
    }

    wallpaper::SceneWallpaper wp;
    state.wp = &wp;

    if (!wp.init()) die("SceneWallpaper::init failed");

    wallpaper::RenderInitInfo info {};
    info.offscreen        = true;
    info.offscreen_tiling = wallpaper::TexTiling::LINEAR;
    info.width            = static_cast<uint16_t>(opts.width);
    info.height           = static_cast<uint16_t>(opts.height);
    info.surface_info.createSurfaceOp =
        [](VkInstance, VkSurfaceKHR*) -> VkResult { return VK_SUCCESS; };
    info.redraw_callback = &redraw_callback;

    wp.initVulkan(info);

    if (!opts.initial_assets.empty())
        wp.setPropertyString(wallpaper::PROPERTY_ASSETS, opts.initial_assets);
    if (!opts.initial_scene.empty())
        wp.setPropertyString(wallpaper::PROPERTY_SOURCE, opts.initial_scene);
    if (opts.initial_fps)
        wp.setPropertyInt32(wallpaper::PROPERTY_FPS,
                            static_cast<int32_t>(opts.initial_fps));

    // Tell the daemon we finished initVulkan and are ready to render.
    if (int rc = ww_bridge_send_ready(state.sock); rc != 0) {
        die("send ready failed: " + std::to_string(rc));
    }

    // Reader thread handles daemon → host control messages.
    std::thread reader([&]() { ipc_reader_loop(state); });

    // --test-pattern mode: drive the ExSwapchain ring on a host-owned
    // timer, bypassing SceneWallpaper's own looper. Emits bind_buffers
    // once + frame_ready per tick with no pixels drawn.
    std::thread test_pattern_thread;
    if (opts.test_pattern) {
        wallpaper::ExSwapchain* ex = nullptr;
        const auto              deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            ex = wp.exSwapchain();
            if (ex != nullptr) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (ex == nullptr)
            die("--test-pattern: exSwapchain() still null after 5s");

        {
            std::lock_guard<std::mutex> lock(state.send_mu);
            send_bind_buffers_locked(state, ex);
        }
        if (!state.bound.load(std::memory_order_acquire))
            die("--test-pattern: bind_buffers failed");

        const uint32_t fps = opts.initial_fps ? opts.initial_fps : 30;
        const auto     tick_period =
            std::chrono::nanoseconds(1'000'000'000ULL / fps);
        test_pattern_thread = std::thread([&, tick_period]() {
            auto next = std::chrono::steady_clock::now();
            while (!state.shutdown.load(std::memory_order_acquire)) {
                next += tick_period;
                ex->renderFrame();
                wallpaper::ExHandle* frame = ex->eatFrame();
                if (frame != nullptr) {
                    std::lock_guard<std::mutex> lock(state.send_mu);
                    send_frame_ready_locked(state, frame);
                }
                std::this_thread::sleep_until(next);
            }
        });
    }

    // Main thread idles; all the real work is in the scene looper or
    // the test-pattern thread.
    while (!state.shutdown.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (test_pattern_thread.joinable()) test_pattern_thread.join();

    if (reader.joinable()) {
        ::shutdown(state.sock, SHUT_RD); // wakes blocking recv
        reader.join();
    }
    ww_bridge_close(state.sock);

    return 0;
}
