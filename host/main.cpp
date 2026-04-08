// waywallen-renderer — offscreen renderer host process.
//
// This binary is the permanent C++-side counterpart to the Rust waywallen
// daemon. The daemon spawns one instance per active wallpaper with a
// preconnected Unix-domain socket passed via `--ipc`. The host:
//
//   1. Connects to the socket and greets the daemon.
//   2. Constructs a SceneWallpaper in offscreen mode so every frame goes
//      into the ExSwapchain's triple-buffered DMA-BUF images.
//   3. On the first redraw callback, snapshots all three ExHandles from
//      the swapchain, serializes their metadata into a `BindBuffers`
//      event, and sends the message + all three DMA-BUF FDs via
//      SCM_RIGHTS. After that point the FDs never need to be retransmitted
//      because they live for the lifetime of the swapchain.
//   4. On every subsequent redraw callback sends a cheap `FrameReady`
//      event referencing the image index (no FDs on the hot path).
//   5. Runs a dedicated IPC reader thread that deserializes control
//      messages from the daemon and forwards them onto SceneWallpaper.
//   6. prctl(PR_SET_PDEATHSIG) so we die with the daemon.

#include "proto.hpp"
#include "uds.hpp"

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
        } else if (a == "--help" || a == "-h") {
            std::printf(
                "usage: waywallen-renderer --ipc PATH [--width W] [--height H]\n"
                "                          [--scene PKG] [--assets DIR] [--fps N]\n"
                "\n"
                "This binary is the renderer host subprocess spawned by the\n"
                "waywallen daemon. It renders a Wallpaper Engine scene into\n"
                "an offscreen DMA-BUF ExSwapchain and streams frame events\n"
                "over the --ipc Unix socket.\n");
            std::exit(0);
        } else {
            die("unknown argument: " + a);
        }
    }
    if (o.ipc_path.empty()) die("--ipc is required");
    return o;
}

} // namespace

// ---------------------------------------------------------------------------
// Host state — shared between the redraw callback and the IPC reader thread.
// ---------------------------------------------------------------------------

struct HostState {
    int                      sock { -1 };
    wallpaper::SceneWallpaper* wp { nullptr };

    std::mutex      send_mu; // serializes writes to `sock`
    std::atomic<bool> bound { false };
    std::atomic<uint64_t> seq { 0 };
    std::atomic<bool> shutdown { false };
};

static HostState* g_state = nullptr; // needed because the redraw callback has
                                      // no user-data pointer on its signature.

static uint64_t now_ns() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

static void send_bind_buffers_locked(HostState& s, wallpaper::ExSwapchain* ex) {
    auto all = ex->snapshot_all_slots();
    if (all[0] == nullptr || all[1] == nullptr || all[2] == nullptr) {
        // Swapchain not fully populated yet; retry next callback.
        return;
    }

    // Sort by id() so that image_index N maps to the handle whose id() == N.
    // That matches how VulkanExSwapchain constructs its slots.
    wallpaper::ExHandle* by_id[3] { nullptr, nullptr, nullptr };
    for (auto* h : all) {
        const int32_t id = h->id();
        if (id < 0 || id >= 3) return; // defensive; should not happen
        by_id[id] = h;
    }
    if (!by_id[0] || !by_id[1] || !by_id[2]) return;

    waywallen::ipc::BindBuffers bb;
    bb.count        = 3;
    bb.fourcc       = by_id[0]->drm_fourcc;
    bb.width        = static_cast<uint32_t>(by_id[0]->width);
    bb.height       = static_cast<uint32_t>(by_id[0]->height);
    bb.stride       = by_id[0]->plane0_stride;
    bb.modifier     = by_id[0]->drm_modifier;
    bb.plane_offset = by_id[0]->plane0_offset;
    bb.sizes        = {
        static_cast<uint64_t>(by_id[0]->size),
        static_cast<uint64_t>(by_id[1]->size),
        static_cast<uint64_t>(by_id[2]->size),
    };

    const int fds[3] = { by_id[0]->fd, by_id[1]->fd, by_id[2]->fd };
    waywallen::ipc::EventMsg ev { std::in_place_type<waywallen::ipc::BindBuffers>, bb };
    waywallen::ipc::send_typed(s.sock, ev, std::span<const int> { fds, 3 });
    s.bound.store(true, std::memory_order_release);
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

    try {
        if (!s.bound.load(std::memory_order_acquire)) {
            send_bind_buffers_locked(s, ex);
        }
        if (!s.bound.load(std::memory_order_acquire)) {
            // Couldn't bind yet (partial slots). Skip this frame event —
            // the renderer will keep producing and we'll catch up.
            return;
        }

        waywallen::ipc::FrameReady fr {};
        fr.image_index = static_cast<uint32_t>(frame->id());
        fr.seq         = s.seq.fetch_add(1, std::memory_order_relaxed);
        fr.ts_ns       = now_ns();
        waywallen::ipc::EventMsg ev { std::in_place_type<waywallen::ipc::FrameReady>, fr };
        waywallen::ipc::send_typed(s.sock, ev);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "waywallen-renderer: send failed: %s\n", e.what());
        s.shutdown.store(true, std::memory_order_release);
    }
}

// ---------------------------------------------------------------------------
// Control-plane reader: one thread, blocking recv_frame loop.
// ---------------------------------------------------------------------------

static void apply_control(HostState& s, const waywallen::ipc::ControlMsg& msg) {
    using namespace waywallen::ipc;
    using namespace wallpaper;

    std::visit(
        [&](auto const& m) {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, HelloCtrl>) {
                // ignore; handshake already occurred
            } else if constexpr (std::is_same_v<T, LoadScene>) {
                if (!s.wp) return;
                s.wp->setPropertyString(PROPERTY_ASSETS, m.assets);
                s.wp->setPropertyString(PROPERTY_SOURCE, m.pkg);
                s.wp->setPropertyInt32(PROPERTY_FPS, static_cast<int32_t>(m.fps));
            } else if constexpr (std::is_same_v<T, Play>) {
                if (s.wp) s.wp->play();
            } else if constexpr (std::is_same_v<T, Pause>) {
                if (s.wp) s.wp->pause();
            } else if constexpr (std::is_same_v<T, Mouse>) {
                if (s.wp) s.wp->mouseInput(m.x, m.y);
            } else if constexpr (std::is_same_v<T, SetFps>) {
                if (s.wp) s.wp->setPropertyInt32(PROPERTY_FPS, static_cast<int32_t>(m.fps));
            } else if constexpr (std::is_same_v<T, Shutdown>) {
                s.shutdown.store(true, std::memory_order_release);
            }
        },
        msg);
}

static void ipc_reader_loop(HostState& s) {
    using namespace waywallen::ipc;
    while (!s.shutdown.load(std::memory_order_acquire)) {
        try {
            auto result = recv_frame(s.sock);
            auto maybe  = control_from_json(result.body);
            // Close any unexpected fds; control messages carry none.
            for (int fd : result.fds) ::close(fd);
            if (!maybe.has_value()) {
                std::fprintf(stderr,
                             "waywallen-renderer: unrecognized control: %s\n",
                             result.body.dump().c_str());
                continue;
            }
            apply_control(s, *maybe);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "waywallen-renderer: recv failed: %s\n", e.what());
            s.shutdown.store(true, std::memory_order_release);
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    Options opts = parse_args(argc, argv);

    // Die with the daemon.
    ::prctl(PR_SET_PDEATHSIG, SIGTERM);

    HostState state;
    g_state = &state;

    state.sock = waywallen::ipc::connect_uds(opts.ipc_path);
    // Note: we do NOT send a ControlMsg::Hello upstream. host→daemon is
    // always EventMsg; sending ControlMsg would force the daemon's reader
    // to dispatch across two disjoint variant types over one pipe. The
    // first upstream message is `Ready`, emitted after initVulkan succeeds.

    wallpaper::SceneWallpaper wp;
    state.wp = &wp;

    if (!wp.init()) die("SceneWallpaper::init failed");

    wallpaper::RenderInitInfo info {};
    info.offscreen        = true;
    info.offscreen_tiling = wallpaper::TexTiling::LINEAR; // iter 1a LINEAR-only
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
    {
        waywallen::ipc::EventMsg ready_ev {
            std::in_place_type<waywallen::ipc::Ready>, waywallen::ipc::Ready {}
        };
        try {
            waywallen::ipc::send_typed(state.sock, ready_ev);
        } catch (const std::exception& e) {
            die(std::string { "ready event failed: " } + e.what());
        }
    }

    // Reader thread handles daemon→host traffic.
    std::thread reader([&]() { ipc_reader_loop(state); });

    // Main thread idles. All the real work happens in the SceneWallpaper's
    // internal looper threads, which fire redraw_callback when frames are
    // ready. We just wait for a shutdown signal.
    while (!state.shutdown.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (reader.joinable()) {
        ::shutdown(state.sock, SHUT_RD); // wakes blocking recvmsg
        reader.join();
    }
    ::close(state.sock);

    return 0;
}
