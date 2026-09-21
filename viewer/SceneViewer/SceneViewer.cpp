#include <cerrno>
#if __is_target_os(macos)
#    include <condition_variable>
#endif
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#if __is_target_os(macos)
#    include <mutex>
#    include <optional>
#endif
#include <thread>
#include <unistd.h>

import vvk;
import rstd.cppstd;
import rstd.log;
import wavsen.audio;
import wescene.core;
import wescene.json;
import wescene.scene_wallpaper;
import wescene.utils;
import viewer.common;
import viewer.audio;

#include "GlfwVulkan.hpp"

#if __is_target_os(macos)
extern "C" VkResult oweCreateGlfwCocoaSurface(GLFWwindow*, VkInstance, VkSurfaceKHR*, int, int);
extern "C" void     oweConfigureGlfwCocoaLayer(GLFWwindow*, int, int);
#endif

using namespace std;
using namespace rstd::prelude;
using namespace rstd::literals;

#if __is_target_os(macos)
// CoreAudio tap/aggregate-device creation may synchronously wait for the
// system-audio permission sheet or the HAL server. Keep that work away from
// the GLFW/Cocoa thread. The worker publishes only the newest complete PCM
// window; the SceneWallpaper API is still consumed on the viewer thread just
// as it is on Linux.
class AudioCaptureWorker {
public:
    AudioCaptureWorker()
        : m_thread([this] {
              run();
          }) {}

    ~AudioCaptureWorker() {
        {
            std::lock_guard lock(m_mutex);
            m_stop    = true;
            m_enabled = false;
        }
        m_condition.notify_one();
        if (m_thread.joinable()) m_thread.join();
    }

    void set_enabled(bool enabled) {
        {
            std::lock_guard lock(m_mutex);
            if (m_enabled == enabled) return;
            m_enabled = enabled;
        }
        m_condition.notify_one();
    }

    bool snapshot(wavsen::audio::AudioPcmWindow& out) {
        std::lock_guard lock(m_mutex);
        if (! m_latest ||
            (m_latest->generation == m_last_generation && m_latest->sequence == m_last_sequence))
            return false;

        out               = *m_latest;
        m_last_generation = m_latest->generation;
        m_last_sequence   = m_latest->sequence;
        return true;
    }

private:
    void run() {
        wavsen::audio::AudioCapture capture;
        bool                        capture_ready = false;
        auto                        retry_at      = std::chrono::steady_clock::now();

        for (;;) {
            {
                std::unique_lock lock(m_mutex);
                m_condition.wait(lock, [this] {
                    return m_stop || m_enabled;
                });
                if (m_stop) break;
            }

            if (! capture_ready) {
                std::unique_lock lock(m_mutex);
                if (m_condition.wait_until(lock, retry_at, [this] {
                        return m_stop || ! m_enabled;
                    })) {
                    if (m_stop) break;
                    continue;
                }
                lock.unlock();

                if (! capture.init()) {
                    retry_at = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                    continue;
                }
                capture_ready = true;
            }

            wavsen::audio::AudioPcmWindow window {};
            if (capture.snapshot(window)) {
                std::lock_guard lock(m_mutex);
                if (! m_stop && m_enabled) m_latest = window;
            }

            std::unique_lock lock(m_mutex);
            if (! m_condition.wait_for(lock, std::chrono::milliseconds(5), [this] {
                    return m_stop || ! m_enabled;
                }))
                continue;

            if (m_stop) break;
            lock.unlock();
            capture.uninit();
            capture_ready = false;
            retry_at      = std::chrono::steady_clock::now();
            {
                std::lock_guard latest_lock(m_mutex);
                m_latest.reset();
            }
        }

        if (capture_ready) capture.uninit();
    }

    std::mutex                                   m_mutex;
    std::condition_variable                      m_condition;
    std::thread                                  m_thread;
    bool                                         m_stop    = false;
    bool                                         m_enabled = false;
    std::optional<wavsen::audio::AudioPcmWindow> m_latest;
    std::uint64_t                                m_last_generation = 0;
    std::uint64_t                                m_last_sequence   = 0;
};
#endif

class StdinJsonControl {
public:
    explicit StdinJsonControl(bool enabled): m_enabled(enabled) {
        if (! m_enabled) return;
        m_original_flags = ::fcntl(STDIN_FILENO, F_GETFL, 0);
        if (m_original_flags < 0 ||
            ::fcntl(STDIN_FILENO, F_SETFL, m_original_flags | O_NONBLOCK) < 0) {
            std::cerr << "--stdin-json: cannot make stdin non-blocking\n";
            m_enabled = false;
        }
    }

    ~StdinJsonControl() {
        if (m_enabled && m_original_flags >= 0) {
            (void)::fcntl(STDIN_FILENO, F_SETFL, m_original_flags);
        }
    }

    void poll(owe::SceneWallpaper& wallpaper) {
        if (! m_enabled || m_eof) return;

        char buffer[4096];
        for (;;) {
            const auto count = ::read(STDIN_FILENO, buffer, sizeof(buffer));
            if (count > 0) {
                m_pending.append(buffer, static_cast<std::size_t>(count));
                consumeLines(wallpaper, false);
                continue;
            }
            if (count == 0) {
                m_eof = true;
                consumeLines(wallpaper, true);
                return;
            }
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                std::cerr << "--stdin-json: stdin read failed: " << std::strerror(errno) << '\n';
                m_eof = true;
            }
            return;
        }
    }

private:
    void consumeLines(owe::SceneWallpaper& wallpaper, bool flush) {
        for (;;) {
            const auto newline = m_pending.find('\n');
            if (newline == std::string::npos) break;
            auto line = m_pending.substr(0, newline);
            m_pending.erase(0, newline + 1);
            consumeLine(wallpaper, std::move(line));
        }
        if (flush && ! m_pending.empty()) {
            consumeLine(wallpaper, std::move(m_pending));
            m_pending.clear();
        }
    }

    static void consumeLine(owe::SceneWallpaper& wallpaper, std::string line) {
        if (! line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) return;

        auto parsed_result = owe::ParseJson(line);
        if (parsed_result.is_err()) {
            const auto error = parsed_result.unwrap_err();
            std::cerr << "--stdin-json: invalid JSON at line " << error.line().to_primitive()
                      << " column " << error.column().to_primitive() << '\n';
            return;
        }
        auto command = parsed_result.unwrap();
        if (! command.is_object()) {
            std::cerr << "--stdin-json: command must be a JSON object\n";
            return;
        }

        auto command_name = command.get("command"_str);
        if (command_name.is_none() || ! (**command_name).is_string()) {
            std::cerr << "--stdin-json: command requires a string name\n";
            return;
        }

        auto name = rstd::cppstd::as_string_view(*(**command_name).as_str());
        if (name == "set_user_property") {
            setUserProperty(wallpaper, command);
        } else if (name == "set_mpris") {
            setMpris(wallpaper, command);
        } else {
            std::cerr << "--stdin-json: unsupported command\n";
        }
    }

    static void setUserProperty(owe::SceneWallpaper& wallpaper, const owe::Json& command) {
        auto key   = command.get("key"_str);
        auto value = command.get("value"_str);
        if (key.is_none() || ! (**key).is_string() || value.is_none()) {
            std::cerr
                << "--stdin-json: set_user_property requires a string key and a value field\n";
            return;
        }

        auto property = rstd::cppstd::to_string(*(**key).as_str());
        if (property.empty()) {
            std::cerr << "--stdin-json: set_user_property requires a non-empty key\n";
            return;
        }
        wallpaper.setUserPropertyJson(property, (**value).clone());
        std::cout << "scene-viewer: queued user property '" << property << "'\n" << std::flush;
    }

    static bool readString(const owe::Json& command, rstd::ref<rstd::str> key, std::string& value) {
        auto field = command.get(key);
        if (field.is_none()) return true;
        auto text = (**field).as_str();
        if (text.is_none()) return false;
        value = rstd::cppstd::to_string(*text);
        return true;
    }

    static void setMpris(owe::SceneWallpaper& wallpaper, const owe::Json& command) {
        auto state = command.get("state"_str);
        if (state.is_none()) {
            std::cerr << "--stdin-json: set_mpris requires state 0, 1, or 2\n";
            return;
        }
        auto state_value = (**state).as_u64();
        if (state_value.is_none() || *state_value > rstd::u64(2)) {
            std::cerr << "--stdin-json: set_mpris requires state 0, 1, or 2\n";
            return;
        }

        owe::MediaStatus status;
        status.state = static_cast<uint32_t>(state_value->to_primitive());
        if (! readString(command, "title"_str, status.title) ||
            ! readString(command, "artist"_str, status.artist) ||
            ! readString(command, "album"_str, status.album) ||
            ! readString(command, "album_artist"_str, status.album_artist) ||
            ! readString(command, "art_url"_str, status.art_url) ||
            ! readString(command, "previous_art_url"_str, status.previous_art_url)) {
            std::cerr << "--stdin-json: set_mpris metadata fields must be strings\n";
            return;
        }

        auto title   = status.title;
        auto art_url = status.art_url;
        wallpaper.setMediaStatus(std::move(status));
        std::cout << "scene-viewer: queued MPRIS title '" << title << "' art '" << art_url << "'\n"
                  << std::flush;
    }

    bool        m_enabled { false };
    bool        m_eof { false };
    int         m_original_flags { -1 };
    std::string m_pending;
};

struct UserData {
    owe::SceneWallpaper* psw { nullptr };
    bool                 mouse_position_locked { false };

    uint16_t width;
    uint16_t height;
};

extern "C" {
#if __is_target_os(macos)
void framebuffer_size_callback(GLFWwindow* window, int width, int height) {
    // GLFW reports backing pixels here when the Cocoa Retina framebuffer hint
    // is enabled. Keep the CAMetalLayer drawable size synchronized on the
    // Cocoa main thread after a display/scale or window-size change.
    oweConfigureGlfwCocoaLayer(window, width, height);
}
#else
void framebuffer_size_callback(GLFWwindow*, int, int) {}
#endif

void mouse_button_callback(GLFWwindow* win, int button, int action, int /*mods*/) {
    UserData* data = static_cast<UserData*>(glfwGetWindowUserPointer(win));
    if (! data || ! data->psw) return;
    // GLFW button numbering (0=left, 1=right, 2=middle) matches WE.
    if (action == GLFW_PRESS) data->psw->mouseButton(button, true);
    if (action == GLFW_RELEASE) data->psw->mouseButton(button, false);
}

void cursor_position_callback(GLFWwindow* win, double xpos, double ypos) {
    UserData* data = static_cast<UserData*>(glfwGetWindowUserPointer(win));
    if (! data || ! data->psw || data->mouse_position_locked) return;
#if __is_target_os(macos)
    int width  = 0;
    int height = 0;
    glfwGetWindowSize(win, &width, &height);
    if (width <= 0 || height <= 0) return;
    data->psw->mouseInput(xpos / static_cast<double>(width), ypos / static_cast<double>(height));
#else
    data->psw->mouseInput(xpos / data->width, ypos / data->height);
#endif
}

void cursor_enter_callback(GLFWwindow* win, int entered) {
    UserData* data = static_cast<UserData*>(glfwGetWindowUserPointer(win));
    if (! data || ! data->psw || data->mouse_position_locked) return;
    data->psw->mouseEnter(entered != 0);
}
}

Option<std::array<double, 2>> parseMousePosition(const std::string& value) {
    if (value.empty()) return None();
    const auto comma = value.find(',');
    if (comma == std::string::npos) return None();
    double x  = 0.0;
    double y  = 0.0;
    auto   xs = value.substr(0, comma);
    auto   ys = value.substr(comma + 1);
#if __is_target_os(macos)
    // Floating-point std::from_chars is unavailable before macOS 26 in this
    // libc++; parse with strtod instead (target API level is macOS 15).
    char* xs_end = nullptr;
    char* ys_end = nullptr;
    x            = std::strtod(xs.c_str(), &xs_end);
    y            = std::strtod(ys.c_str(), &ys_end);
    if (xs_end != xs.c_str() + xs.size() || ys_end != ys.c_str() + ys.size()) return None();
#else
    auto xr = std::from_chars(xs.data(), xs.data() + xs.size(), x);
    auto yr = std::from_chars(ys.data(), ys.data() + ys.size(), y);
    if (xr.ec != std::errc {} || yr.ec != std::errc {}) return None();
#endif
    return Some(std::array { std::clamp(x, 0.0, 1.0), std::clamp(y, 0.0, 1.0) });
}

int main(int argc, char** argv) {
    static rstd::log::EnvLogger _logger;
    rstd::log::set_logger(_logger);
    rstd::log::set_max_level(_logger.filter());

    auto args                = viewer::ParseSceneViewerArgs(argc, argv);
    auto [w_width, w_height] = args.resolution;

    viewer::InitGlfwPlatformHint(/*force_x11=*/false);
#if __is_target_os(macos)
    auto loader_result = vvk::VulkanLoader::Open();
    if (loader_result.is_err()) {
        std::cerr << "Failed to load Vulkan\n";
        return -1;
    }
    auto vulkan_loader = loader_result.unwrap_unchecked();
    glfwInitVulkanLoader(vulkan_loader.global().vkGetInstanceProcAddr);
    if (! glfwInit()) {
        std::cerr << "Failed to initialize GLFW\n";
        return -1;
    }
#else
    glfwInit();
#endif
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
#if __is_target_os(macos)
    // Keep the backing framebuffer in physical pixels. The window remains
    // sized in Cocoa points, while the CAMetalLayer/Vulkan swapchain use the
    // 2x (or display-specific) backing dimensions.
    glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_TRUE);
#endif
    // Bulk-scan path: WP_HEADLESS=1 hides the window so a scan loop over
    // hundreds of pkgs doesn't spam the desktop. Compile/render still
    // runs against the offscreen surface — stderr captures shader errors.
    if (const char* hl = std::getenv("WP_HEADLESS"); hl && hl[0] == '1') {
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    }
    GLFWwindow* window = glfwCreateWindow(w_width, w_height, "WP", nullptr, nullptr);
#if __is_target_os(macos)
    if (window == nullptr) {
        std::cerr << "Failed to create GLFW window\n";
        glfwTerminate();
        return -1;
    }
#endif
    int render_width  = w_width;
    int render_height = w_height;
#if __is_target_os(macos)
    glfwGetFramebufferSize(window, &render_width, &render_height);
    if (render_width <= 0) render_width = w_width;
    if (render_height <= 0) render_height = w_height;
#endif
    UserData data;
    data.width  = w_width;
    data.height = w_height;

    owe::RenderInitInfo info;
    info.enable_valid_layer = args.enable_valid_layer;
    info.width              = static_cast<std::uint16_t>(render_width);
    info.height             = static_cast<std::uint16_t>(render_height);
    info.msaa_samples       = args.msaa_samples.to_primitive();

    auto& sf_info = info.surface_info;
    {
        uint32_t glfwExtCount = 0;
        auto     exts         = glfwGetRequiredInstanceExtensions(&glfwExtCount);
#if __is_target_os(macos)
        if (exts == nullptr || glfwExtCount == 0) {
            std::cerr << "GLFW did not provide Vulkan instance extensions\n";
            glfwDestroyWindow(window);
            glfwTerminate();
            return -1;
        }
#endif
        for (uint32_t i = 0; i < glfwExtCount; i++) {
            sf_info.instanceExts.emplace_back(exts[i]);
        }
#if __is_target_os(macos)
        sf_info.createSurfaceOp = [window, render_width, render_height](VkInstance    inst,
                                                                        VkSurfaceKHR* surface) {
            return oweCreateGlfwCocoaSurface(window, inst, surface, render_width, render_height);
        };
#else
        sf_info.createSurfaceOp = [window](VkInstance inst, VkSurfaceKHR* surface) {
            return glfwCreateWindowSurface(inst, window, nullptr, surface);
        };
#endif
    }

#if ! __is_target_os(macos)
    if (window == nullptr) {
        std::cout << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        return -1;
    }
#endif

    auto* psw = new owe::SceneWallpaper();
    data.psw  = psw;

    std::atomic<bool> audio_response_demand { false };
    psw->setAudioResponseDemandCallback([&audio_response_demand](bool active) {
        audio_response_demand.store(active, std::memory_order_release);
    });

    psw->init();

    owe::SceneWallpaperConfig config;
    config.assets_dir      = std::move(args.assets_dir);
    config.source_pkg_path = std::move(args.scene_path);
    config.graphviz        = args.graphviz;
    config.load_bench      = owe::CreateSceneLoadBench(args.load_bench_output.as_str());
    config.fps             = static_cast<uint32_t>(args.fps.to_primitive());
    if (args.random_seed.is_some()) {
        config.random_seed = Some(*args.random_seed);
    }

    std::string cache_path = std::move(args.cache_path);
    if (cache_path.empty()) cache_path = viewer::DefaultCacheDir("wescene-renderer").string();
    config.cache_dir = std::move(cache_path);

    // Apply --user-properties FILE before the scene loads so the first
    // frame already reflects the user's edits. Mirrors the daemon path
    // (Init.user_properties): JSON object whose values can be strings,
    // numbers, or booleans.
    if (const auto& up_path = args.user_properties_path; ! up_path.empty()) {
        std::ifstream is(up_path);
        if (! is) {
            std::cerr << "--user-properties: cannot open '" << up_path << "'\n";
            return 1;
        }
        std::stringstream ss;
        ss << is.rdbuf();
        auto parsed_result = owe::ParseJson(ss.str(), { .allow_comments = true });
        if (parsed_result.is_err()) {
            auto error = parsed_result.unwrap_err();
            std::cerr << "--user-properties: '" << up_path << "' is invalid JSON at line "
                      << error.line().to_primitive() << " column " << error.column().to_primitive()
                      << '\n';
            return 1;
        }
        auto parsed = parsed_result.unwrap();
        if (! parsed.is_object()) {
            std::cerr << "--user-properties: '" << up_path << "' is not a JSON object\n";
            return 1;
        }
        auto object = parsed.as_object();
        (*object)->iter().for_each([&](auto entry) {
            auto [entry_key, entry_value] = entry;
            config.user_properties.insert(entry_key->clone(), entry_value->clone());
        });
    }

    psw->configure(std::move(config));
    psw->initVulkan(std::move(info));

    glfwSetWindowUserPointer(window, &data);

    glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);
    glfwSetMouseButtonCallback(window, mouse_button_callback);
    glfwSetCursorPosCallback(window, cursor_position_callback);
    glfwSetCursorEnterCallback(window, cursor_enter_callback);

    auto locked_mouse          = parseMousePosition(args.mouse_position);
    data.mouse_position_locked = locked_mouse.is_some();
    auto apply_locked_mouse    = [&]() {
        if (! locked_mouse) return;
        psw->mouseEnter(true);
        psw->mouseInput((*locked_mouse)[0], (*locked_mouse)[1]);
#if __is_target_os(macos)
        int window_width  = 0;
        int window_height = 0;
        glfwGetWindowSize(window, &window_width, &window_height);
        if (window_width > 0 && window_height > 0) {
            glfwSetCursorPos(
                window, (*locked_mouse)[0] * window_width, (*locked_mouse)[1] * window_height);
        }
#else
        glfwSetCursorPos(window, (*locked_mouse)[0] * w_width, (*locked_mouse)[1] * w_height);
#endif
    };
    apply_locked_mouse();

    StdinJsonControl stdin_control(args.stdin_json);
#if __is_target_os(macos)
    AudioCaptureWorker audio_capture;
#else
    wavsen::audio::AudioCapture audio_capture;
    auto                        next_audio_update = std::chrono::steady_clock::now();
#endif
    bool audio_ended  = true;
    auto update_audio = [&] {
#if __is_target_os(macos)
        const bool demanded = audio_response_demand.load(std::memory_order_acquire);
        audio_capture.set_enabled(demanded);
        if (! demanded) {
            if (! audio_ended) {
                psw->endAudioResponse();
                audio_ended = true;
            }
            return;
        }
        wavsen::audio::AudioPcmWindow window {};
        if (audio_capture.snapshot(window)) {
            audio_ended = false;
            psw->setAudioPcmWindow(viewer::ConvertAudioWindow(window));
        }
#else
        if (! audio_response_demand.load(std::memory_order_acquire)) {
            if (audio_capture.is_inited()) audio_capture.uninit();
            if (! audio_ended) {
                psw->endAudioResponse();
                audio_ended = true;
            }
            return;
        }
        if (! audio_capture.is_inited() && ! audio_capture.init()) return;
        audio_ended    = false;
        const auto now = std::chrono::steady_clock::now();
        if (now < next_audio_update) return;
        next_audio_update = now + std::chrono::milliseconds(33);
        wavsen::audio::AudioPcmWindow window {};
        if (audio_capture.snapshot(window)) {
            psw->setAudioPcmWindow(viewer::ConvertAudioWindow(window));
        }
#endif
    };

    // Bulk-scan path: WP_COMPILE_ONLY=N waits N seconds after scene load
    // to let the async shader-compile pass drain, then exits. Skips the
    // render loop so no swapchain present is required (which would deadlock
    // with a hidden window). Use together with WP_HEADLESS=1.
    if (const char* co = std::getenv("WP_COMPILE_ONLY"); co && co[0] != '\0') {
        int seconds = std::atoi(co);
        if (seconds <= 0) seconds = 2;
#if __is_target_os(macos)
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        // Keep the Cocoa run loop alive while the render thread initializes
        // the GLFW surface on the main thread. A blocking sleep would leave
        // dispatch_sync_f waiting forever in headless compile-only runs.
        while (std::chrono::steady_clock::now() < deadline) {
            glfwWaitEventsTimeout(0.01);
        }
#else
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
#endif
    } else {
        while (! glfwWindowShouldClose(window)) {
#if __is_target_os(macos)
            // Keep the Cocoa run loop responsive while MoltenVK drives the
            // CAMetalLayer. A 30 Hz event wait can leave display-link/layer
            // updates pending across multiple presents and lower the
            // effective FIFO cadence. Rendering remains on the render thread;
            // this only services GLFW/Cocoa events more frequently.
            glfwWaitEventsTimeout(1.0 / 120.0);
#else
            glfwWaitEventsTimeout(1.0 / 30.0);
#endif
            stdin_control.poll(*psw);
            apply_locked_mouse();
            update_audio();
        }
    }
    delete psw;
    // wgl.Clear();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
