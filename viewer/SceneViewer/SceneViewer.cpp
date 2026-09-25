#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

import vvk;
import rstd;
import rstd.log;
import wavsen.audio;
import wescene.core;
import wescene.json;
import wescene.scene_wallpaper;
import wescene.utils;
import viewer.common;
import viewer.audio;
import viewer.stdin_control;

import viewer.glfw_vulkan;

using namespace viewer::glfw;

#if __is_target_os(macos)
extern "C" VkResult oweCreateGlfwCocoaSurface(GLFWwindow*, VkInstance, VkSurfaceKHR*, int, int);
extern "C" void     oweConfigureGlfwCocoaLayer(GLFWwindow*, int, int);
#endif

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::io::eprintln;
using rstd::io::println;
using rstd::sync::atomic::Atomic;
using rstd::sync::atomic::Ordering;
using rstd::time::Duration;
using rstd::time::Instant;

struct UserData {
    owe::SceneWallpaper* psw { nullptr };
    bool                 mouse_position_locked { false };
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
    if (action == viewer::glfw::Press) data->psw->mouseButton(button, true);
    if (action == viewer::glfw::Release) data->psw->mouseButton(button, false);
}

void cursor_position_callback(GLFWwindow* win, double xpos, double ypos) {
    UserData* data = static_cast<UserData*>(glfwGetWindowUserPointer(win));
    if (! data || ! data->psw || data->mouse_position_locked) return;
    int width  = 0;
    int height = 0;
    glfwGetWindowSize(win, &width, &height);
    if (width <= 0 || height <= 0) return;
    data->psw->mouseInput(xpos / static_cast<double>(width), ypos / static_cast<double>(height));
}

void cursor_enter_callback(GLFWwindow* win, int entered) {
    UserData* data = static_cast<UserData*>(glfwGetWindowUserPointer(win));
    if (! data || ! data->psw || data->mouse_position_locked) return;
    data->psw->mouseEnter(entered != 0);
}
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
        eprintln("Failed to load Vulkan");
        return -1;
    }
    auto vulkan_loader = loader_result.unwrap_unchecked();
    viewer::glfw::glfwInitVulkanLoader(vulkan_loader.global().vkGetInstanceProcAddr);
    if (! glfwInit()) {
        eprintln("Failed to initialize GLFW");
        return -1;
    }
#else
    glfwInit();
#endif
    glfwWindowHint(viewer::glfw::ClientApi, viewer::glfw::NoApi);
#if __is_target_os(macos)
    // Keep the backing framebuffer in physical pixels. The window remains
    // sized in Cocoa points, while the CAMetalLayer/Vulkan swapchain use the
    // 2x (or display-specific) backing dimensions.
    glfwWindowHint(viewer::glfw::CocoaRetinaFramebuffer, viewer::glfw::TrueValue);
#endif
    // Bulk-scan path: WP_HEADLESS=1 hides the window so a scan loop over
    // hundreds of pkgs doesn't spam the desktop. Compile/render still
    // runs against the offscreen surface — stderr captures shader errors.
    if (auto hl = rstd::env::var_os("WP_HEADLESS"_str);
        hl && ! hl->is_empty() && hl->as_os_str().as_encoded_bytes()[usize()] == u8('1')) {
        glfwWindowHint(viewer::glfw::Visible, viewer::glfw::FalseValue);
    }
    GLFWwindow* window = glfwCreateWindow(w_width, w_height, "WP", nullptr, nullptr);
#if __is_target_os(macos)
    if (window == nullptr) {
        eprintln("Failed to create GLFW window");
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

    owe::RenderInitInfo info;
    info.enable_valid_layer = args.enable_valid_layer;
    info.width              = static_cast<rstd::uint16_t>(render_width);
    info.height             = static_cast<rstd::uint16_t>(render_height);
    info.msaa_samples       = args.msaa_samples.to_primitive();

    auto& sf_info = info.surface_info;
    {
        uint32_t glfwExtCount = 0;
        auto     exts         = glfwGetRequiredInstanceExtensions(&glfwExtCount);
#if __is_target_os(macos)
        if (exts == nullptr || glfwExtCount == 0) {
            eprintln("GLFW did not provide Vulkan instance extensions");
            glfwDestroyWindow(window);
            glfwTerminate();
            return -1;
        }
#endif
        for (uint32_t i = 0; i < glfwExtCount; i++) {
            sf_info.instanceExts.push(
                String::make(rstd::ffi::CStr::from_ptr(exts[i]).to_str().unwrap()));
        }
#if __is_target_os(macos)
        sf_info.createSurfaceOp = Some(owe::CreateSurfaceCallback::make(
            [window, render_width, render_height](VkInstance inst, VkSurfaceKHR* surface) {
                return oweCreateGlfwCocoaSurface(
                    window, inst, surface, render_width, render_height);
            }));
#else
        sf_info.createSurfaceOp =
            Some(owe::CreateSurfaceCallback::make([window](VkInstance inst, VkSurfaceKHR* surface) {
                return viewer::glfw::glfwCreateWindowSurface(inst, window, nullptr, surface);
            }));
#endif
    }

#if ! __is_target_os(macos)
    if (window == nullptr) {
        println("Failed to create GLFW window");
        glfwTerminate();
        return -1;
    }
#endif

    auto* psw = new owe::SceneWallpaper();
    data.psw  = psw;

    Atomic<bool> audio_response_demand { false };
    psw->setAudioResponseDemandCallback([&audio_response_demand](bool active) {
        audio_response_demand.store(active, Ordering::Release);
    });

    psw->init();

    owe::SceneWallpaperConfig config;
    config.assets_dir      = rstd::move(args.assets_dir);
    config.source_pkg_path = rstd::move(args.scene_path);
    config.graphviz        = args.graphviz;
    config.load_bench      = owe::CreateSceneLoadBench(args.load_bench_output.as_str());
    config.fps             = static_cast<uint32_t>(args.fps.to_primitive());
    if (args.random_seed.is_some()) {
        config.random_seed = Some(*args.random_seed);
    }

    config.cache_dir = args.cache_path.is_empty() ? viewer::DefaultCacheDir("wescene-renderer"_str)
                                                  : rstd::move(args.cache_path);

    // Apply --user-properties FILE before the scene loads so the first
    // frame already reflects the user's edits. Mirrors the daemon path
    // (Init.user_properties): JSON object whose values can be strings,
    // numbers, or booleans.
    if (const auto& up_path = args.user_properties_path; ! up_path.is_empty()) {
        auto source = rstd::fs::read_to_string(up_path);
        if (source.is_err()) {
            eprintln("--user-properties: cannot open '{}'",
                     up_path.as_path().as_os_str().display());
            return 1;
        }
        auto parsed_result = owe::ParseJson(source->as_str(), { .allow_comments = true });
        if (parsed_result.is_err()) {
            auto error = parsed_result.unwrap_err();
            eprintln("--user-properties: '{}' is invalid JSON at line {} column {}",
                     up_path.as_path().as_os_str().display(),
                     error.line(),
                     error.column());
            return 1;
        }
        auto parsed = parsed_result.unwrap();
        if (! parsed.is_object()) {
            eprintln("--user-properties: '{}' is not a JSON object",
                     up_path.as_path().as_os_str().display());
            return 1;
        }
        auto object = parsed.as_object();
        (*object)->iter().for_each([&](auto entry) {
            auto [entry_key, entry_value] = entry;
            config.user_properties.insert(entry_key->clone(), entry_value->clone());
        });
    }

    psw->configure(rstd::move(config));
    psw->initVulkan(rstd::move(info));

    glfwSetWindowUserPointer(window, &data);

    glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);
    glfwSetMouseButtonCallback(window, mouse_button_callback);
    glfwSetCursorPosCallback(window, cursor_position_callback);
    glfwSetCursorEnterCallback(window, cursor_enter_callback);

    auto locked_mouse          = viewer::ParseMousePosition(args.mouse_position.as_str());
    data.mouse_position_locked = locked_mouse.is_some();
    auto apply_locked_mouse    = [&]() {
        if (! locked_mouse) return;
        psw->mouseEnter(true);
        psw->mouseInput((*locked_mouse)[usize()], (*locked_mouse)[usize(1)]);
        int window_width  = 0;
        int window_height = 0;
        glfwGetWindowSize(window, &window_width, &window_height);
        if (window_width > 0 && window_height > 0) {
            glfwSetCursorPos(window,
                             (*locked_mouse)[usize()] * window_width,
                             (*locked_mouse)[usize(1)] * window_height);
        }
    };
    apply_locked_mouse();

    viewer::StdinJsonControl stdin_control(args.stdin_json);
#if __is_target_os(macos)
    viewer::AudioCaptureWorker audio_capture;
#else
    wavsen::audio::AudioCapture audio_capture;
    auto                        next_audio_update = Instant::now();
#endif
    bool audio_ended  = true;
    auto update_audio = [&] {
#if __is_target_os(macos)
        const bool demanded = audio_response_demand.load(Ordering::Acquire);
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
        if (! audio_response_demand.load(Ordering::Acquire)) {
            if (audio_capture.is_inited()) audio_capture.uninit();
            if (! audio_ended) {
                psw->endAudioResponse();
                audio_ended = true;
            }
            return;
        }
        if (! audio_capture.is_inited() && ! audio_capture.init()) return;
        audio_ended    = false;
        const auto now = Instant::now();
        if (now < next_audio_update) return;
        next_audio_update = now + Duration::from_millis(u64(33));
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
    if (auto co = rstd::env::var_os("WP_COMPILE_ONLY"_str); co && ! co->is_empty()) {
        auto text    = co->as_os_str().to_str();
        auto seconds = text ? rstd::from_str<i32>(*text).unwrap_or(i32(2)) : i32(2);
        if (seconds <= i32()) seconds = i32(2);
#if __is_target_os(macos)
        const auto deadline = Instant::now() + Duration::from_secs(u64(seconds.to_primitive()));
        // Keep the Cocoa run loop alive while the render thread initializes
        // the GLFW surface on the main thread. A blocking sleep would leave
        // dispatch_sync_f waiting forever in headless compile-only runs.
        while (Instant::now() < deadline) {
            glfwWaitEventsTimeout(0.01);
        }
#else
        rstd::thread::sleep(Duration::from_secs(u64(seconds.to_primitive())));
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
