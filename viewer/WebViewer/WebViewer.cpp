// weweb standalone GLFW + Vulkan + CEF (OSR) viewer.

import vvk;
import rstd.argparse;
import rstd.cppstd;
import owe.audio_response;
import wescene.cli;
import weweb;
import wavsen.audio;
import viewer.common;
import viewer.audio;
import viewer.web;

#include "GlfwVulkan.hpp"

namespace
{

using namespace rstd::prelude;
using namespace rstd::argparse;
using namespace rstd::literals;

struct WebViewerArgs {
    std::string workshop;
    std::string presenter;
    i32         width;
    i32         height;
    i32         remote_debugging_port;
};

std::string ToStdString(const String& value) { return rstd::cppstd::to_string(value.as_str()); }

template<typename T>
const T& Value(const Matches& matches, const ArgKey<T>& key) {
    auto value = matches.get_one(key);
    if (value.is_err() || value->is_none()) rstd::unreachable();
    return ***value;
}

auto ParseWebViewerArgs(int argc, char** argv) -> Result<WebViewerArgs, owe::cli::ParseExit> {
    auto command  = Command::make("webviewer"_str);
    auto workshop = command.add_arg(
        Arg<String>::value("workshop"_str, string_parser())
            .value_name("WORKSHOP"_str)
            .help("path to a workshop/<id>/ directory containing project.json + index.html"_str)
            .required());
    auto width  = command.add_arg(Arg<i32>::value("width"_str, from_str_parser<i32>())
                                      .long_name("width"_str)
                                      .help("initial window width in pixels"_str)
                                      .default_value("1280"_str));
    auto height = command.add_arg(Arg<i32>::value("height"_str, from_str_parser<i32>())
                                      .long_name("height"_str)
                                      .help("initial window height in pixels"_str)
                                      .default_value("720"_str));
    auto remote_debugging_port =
        command.add_arg(Arg<i32>::value("remote-debugging-port"_str, from_str_parser<i32>())
                            .long_name("remote-debugging-port"_str)
                            .help("if non-zero, expose chrome devtools on this localhost port"_str)
                            .default_value("0"_str));
    auto presenter =
        command.add_arg(Arg<String>::value("presenter"_str, string_parser())
                            .long_name("presenter"_str)
                            .help("present backend: vulkan (macOS) or egl/vulkan (Linux)"_str)
#if __is_target_os(macos)
                            .default_value("vulkan"_str));
#else
                            .default_value("egl"_str));
#endif

    auto parsed = owe::cli::ParseArgs(rstd::move(command), argc, argv);
    if (parsed.is_err()) return Err(parsed.unwrap_err());
    auto matches = rstd::move(parsed).unwrap();
    return Ok(WebViewerArgs {
        .workshop              = ToStdString(Value(matches, workshop)),
        .presenter             = ToStdString(Value(matches, presenter)),
        .width                 = Value(matches, width),
        .height                = Value(matches, height),
        .remote_debugging_port = Value(matches, remote_debugging_port),
    });
}

struct ViewerCtx {
    weweb::BrowserHost* host { nullptr };
    weweb::Presenter*   presenter { nullptr };
    bool                need_swapchain_recreate { false };
};

// CEF mouse button codes match cef_mouse_button_type_t: 0=L, 1=M, 2=R.
int CefButtonFromGlfw(int glfw_button) {
    switch (glfw_button) {
    case GLFW_MOUSE_BUTTON_LEFT: return 0;
    case GLFW_MOUSE_BUTTON_MIDDLE: return 1;
    case GLFW_MOUSE_BUTTON_RIGHT: return 2;
    default: return -1;
    }
}

void OnFramebufferSize(GLFWwindow* w, int /*fb_w*/, int /*fb_h*/) {
    auto* ctx = static_cast<ViewerCtx*>(glfwGetWindowUserPointer(w));
    if (ctx) ctx->need_swapchain_recreate = true;
}

void OnCursorPos(GLFWwindow* w, double x, double y) {
    auto* ctx = static_cast<ViewerCtx*>(glfwGetWindowUserPointer(w));
    if (! ctx || ! ctx->host) return;
    bool left = glfwGetMouseButton(w, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    ctx->host->OnMouseMove(static_cast<int>(x), static_cast<int>(y), left);
}

void OnMouseButton(GLFWwindow* w, int button, int action, int /*mods*/) {
    auto* ctx = static_cast<ViewerCtx*>(glfwGetWindowUserPointer(w));
    if (! ctx || ! ctx->host) return;
    int cef_btn = CefButtonFromGlfw(button);
    if (cef_btn < 0) return;
    double x = 0, y = 0;
    glfwGetCursorPos(w, &x, &y);
    ctx->host->OnMouseButton(
        static_cast<int>(x), static_cast<int>(y), cef_btn, action == GLFW_PRESS, /*click_count=*/1);
}

void OnScroll(GLFWwindow* w, double dx, double dy) {
    auto* ctx = static_cast<ViewerCtx*>(glfwGetWindowUserPointer(w));
    if (! ctx || ! ctx->host) return;
    double x = 0, y = 0;
    glfwGetCursorPos(w, &x, &y);
    // GLFW scroll units are "wheel notches"; CEF expects pixel-ish deltas.
    ctx->host->OnMouseWheel(static_cast<int>(x),
                            static_cast<int>(y),
                            static_cast<int>(dx * 40),
                            static_cast<int>(dy * 40));
}

void OnFocus(GLFWwindow* w, int focused) {
    auto* ctx = static_cast<ViewerCtx*>(glfwGetWindowUserPointer(w));
    if (ctx && ctx->host) ctx->host->OnFocus(focused == GLFW_TRUE);
}

struct WindowMetrics {
    int   logical_width { 0 };
    int   logical_height { 0 };
    int   framebuffer_width { 0 };
    int   framebuffer_height { 0 };
    float scale { 1.0f };
};

WindowMetrics GetWindowMetrics(GLFWwindow* window) {
    WindowMetrics metrics;
    glfwGetWindowSize(window, &metrics.logical_width, &metrics.logical_height);
    glfwGetFramebufferSize(window, &metrics.framebuffer_width, &metrics.framebuffer_height);

    float scale_x = 1.0f;
    float scale_y = 1.0f;
#if __is_target_os(macos)
    glfwGetWindowContentScale(window, &scale_x, &scale_y);
#endif
    if (scale_x <= 0.0f || scale_y <= 0.0f) {
        scale_x = metrics.logical_width > 0
                      ? static_cast<float>(metrics.framebuffer_width) / metrics.logical_width
                      : 1.0f;
        scale_y = metrics.logical_height > 0
                      ? static_cast<float>(metrics.framebuffer_height) / metrics.logical_height
                      : 1.0f;
    }
    metrics.scale = scale_x > 0.0f ? scale_x : scale_y;
    return metrics;
}

#if __is_target_os(macos)
std::filesystem::path CefFrameworkRoot(const std::filesystem::path& exe_dir) {
    std::vector<std::filesystem::path> candidates;
    if (const char* override_path = std::getenv("OWE_CEF_FRAMEWORK_PATH");
        override_path != nullptr && override_path[0] != '\0') {
        std::filesystem::path path(override_path);
        if (path.filename() == "Chromium Embedded Framework") path = path.parent_path();
        candidates.push_back(std::move(path));
    }
    candidates.push_back(exe_dir / "../Frameworks/Chromium Embedded Framework.framework");
    candidates.push_back(exe_dir / "Chromium Embedded Framework.framework");
    candidates.push_back(exe_dir / "../Chromium Embedded Framework.framework");
    for (const auto& candidate : candidates) {
        std::error_code error;
        if (std::filesystem::is_directory(candidate / "Resources", error)) return candidate;
    }
    return {};
}
#endif

} // namespace

int main(int argc, char** argv) {
    weweb::BrowserHost host;

    // CRITICAL: must run before any of our own arg parsing — CEF re-execs
    // this binary with `--type=zygote/--type=renderer/...` switches; we
    // must short-circuit those helper invocations immediately.
    if (int helper_exit = host.RunOrExitIfHelper(argc, argv); helper_exit >= 0) {
        return helper_exit;
    }

    auto parsed_args = ParseWebViewerArgs(argc, argv);
    if (parsed_args.is_err()) return parsed_args.unwrap_err().code;
    auto args = rstd::move(parsed_args).unwrap();

    auto workshop_dir = std::filesystem::path(args.workshop);
    if (! std::filesystem::is_directory(workshop_dir)) {
        std::cerr << "webviewer: not a directory: " << workshop_dir.string() << "\n";
        return 2;
    }

    auto presenter_name = args.presenter;
#if defined(__linux__)
    if (presenter_name != "vulkan" && presenter_name != "egl") {
        std::cerr << "webviewer: --presenter must be 'vulkan' or 'egl', got '" << presenter_name
                  << "'\n";
        return 2;
    }
#else
    if (presenter_name != "vulkan") {
        std::cerr << "webviewer: macOS requires --presenter vulkan, got '" << presenter_name
                  << "'\n";
        return 2;
    }
#endif

    auto manifest_opt = weweb::LoadWebManifest(workshop_dir);
    if (! manifest_opt) return 2;
    auto& manifest = *manifest_opt;

    // Keep Linux on its existing X11 path; macOS lets GLFW select Cocoa.
#if __is_target_os(macos)
    viewer::InitGlfwPlatformHint(/*force_x11=*/false);
#else
    viewer::InitGlfwPlatformHint(/*force_x11=*/true);
#endif
    Option<vvk::VulkanLoader> vulkan_loader;
    if (presenter_name == "vulkan") {
        auto loaded = vvk::VulkanLoader::Open();
        if (loaded.is_err()) {
            std::cerr << "webviewer: Vulkan loader open failed\n";
            return 1;
        }
        vulkan_loader = Some(loaded.unwrap_unchecked());
        glfwInitVulkanLoader(
            vulkan_loader.as_ref().unwrap_unchecked().global().vkGetInstanceProcAddr);
    }
    if (! glfwInit()) {
        std::cerr << "webviewer: glfwInit failed\n";
        return 1;
    }
    if (presenter_name == "vulkan" && ! glfwVulkanSupported()) {
        std::cerr << "webviewer: glfw says Vulkan is not supported\n";
        glfwTerminate();
        return 1;
    }
    // The presenter owns the graphics context/device.
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
#if __is_target_os(macos)
    glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_TRUE);
#endif

    int         w_width  = args.width.to_primitive();
    int         w_height = args.height.to_primitive();
    GLFWwindow* window =
        glfwCreateWindow(w_width, w_height, manifest.title.c_str(), nullptr, nullptr);
    if (! window) {
        std::cerr << "webviewer: glfwCreateWindow failed\n";
        glfwTerminate();
        return 1;
    }

    std::unique_ptr<weweb::Presenter> presenter;
    if (presenter_name == "vulkan") {
        presenter = std::make_unique<weweb::VulkanBlitter>();
#if defined(__linux__)
    } else {
        presenter = std::make_unique<weweb::EglPresenter>();
#endif
    }
    if (! presenter->Init(window)) {
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    auto exe_dir = viewer::ExecutableDir(argv[0]);

    weweb::BrowserHost::InitOptions opts;
    opts.resources_dir = exe_dir;
    opts.locales_dir   = exe_dir / "locales";
#if __is_target_os(macos)
    if (auto framework_root = CefFrameworkRoot(exe_dir); ! framework_root.empty()) {
        opts.resources_dir = framework_root / "Resources";
        opts.locales_dir   = opts.resources_dir / "locales";
    }
#endif
    if (int port = args.remote_debugging_port.to_primitive(); port > 0) {
        opts.enable_remote_debugging = true;
        opts.remote_debugging_port   = port;
    }

    if (! host.Init(opts)) {
        presenter->Shutdown();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    // Hook platform-specific CEF frame handles into the presenter's import
    // path. Each callback is synchronous: CEF reclaims its frame resource
    // immediately after the callback returns.
    weweb::Presenter* presenter_ptr = presenter.get();
#if defined(__linux__)
    host.SetAcceleratedPaintCallback([presenter_ptr](const weweb::DmaBufFrame& frame) {
        presenter_ptr->AcceptDmaBuf(frame);
    });
#else
    host.SetCpuPaintCallback([presenter_ptr](const weweb::CpuPaintFrame& frame) {
        presenter_ptr->AcceptCpuPaint(frame);
    });
#endif

#if __is_target_os(macos)
    // CEF view sizes are in logical pixels. The presenter remains in physical
    // framebuffer pixels, so CEF applies the device scale factor internally.
    auto initial_metrics = GetWindowMetrics(window);
    if (initial_metrics.logical_width <= 0 || initial_metrics.logical_height <= 0) {
        initial_metrics.logical_width  = w_width;
        initial_metrics.logical_height = w_height;
    }
    weweb::BrowserHost::OpenOptions open_opts;
    open_opts.device_scale_factor = initial_metrics.scale;
    // CEF currently supports windowless shared textures only on Windows.
    // macOS must use OnPaint; CEF still performs page compositing on the GPU.
    open_opts.shared_texture_enabled = false;
    if (! host.OpenWallpaper(manifest,
                             workshop_dir,
                             initial_metrics.logical_width,
                             initial_metrics.logical_height,
                             open_opts)) {
#else
    // Preserve the Linux contract: CEF receives the physical presenter extent
    // and uses its existing unit-scale screen information.
    if (! host.OpenWallpaper(manifest,
                             workshop_dir,
                             static_cast<int>(presenter->Width()),
                             static_cast<int>(presenter->Height()))) {
#endif
        host.Shutdown();
        presenter->Shutdown();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    ViewerCtx ctx;
    ctx.host      = &host;
    ctx.presenter = presenter.get();
    glfwSetWindowUserPointer(window, &ctx);
    glfwSetFramebufferSizeCallback(window, OnFramebufferSize);
    glfwSetCursorPosCallback(window, OnCursorPos);
    glfwSetMouseButtonCallback(window, OnMouseButton);
    glfwSetScrollCallback(window, OnScroll);
    glfwSetWindowFocusCallback(window, OnFocus);

    rstd::sync::atomic::Atomic<bool> audio_response_demand { false };
    host.SetAudioResponseDemandCallback([&audio_response_demand](bool active) {
        audio_response_demand.store(active, rstd::sync::atomic::Ordering::Release);
    });

    wavsen::audio::AudioCapture audio_capture;
    owe::audio::ResponseEngine  audio_response;
    unsigned                    audio_tick = 0;

    // Main loop. ~60Hz nominal. CefDoMessageLoopWork is cheap; the
    // Vulkan FIFO present pacing also throttles us.
    while (! glfwWindowShouldClose(window) && ! host.ShouldExit()) {
        glfwPollEvents();
        host.Pump();

        if (! audio_response_demand.load(rstd::sync::atomic::Ordering::Acquire)) {
            if (audio_capture.is_inited()) audio_capture.uninit();
            audio_response.end();
        } else {
            if (! audio_capture.is_inited() && ! audio_capture.init()) {
                std::cerr << "webviewer: audio capture init failed\n";
            }
        }
        if (audio_capture.is_inited() && (audio_tick++ & 1u) == 0) {
            wavsen::audio::AudioPcmWindow captured {};
            owe::audio::ResponseFrame     response {};
            auto                          window = owe::audio::PcmWindow {};
            if (audio_capture.snapshot(captured)) window = viewer::ConvertAudioWindow(captured);
            if (window.frames != 0 && audio_response.analyze(window, response)) {
                std::array<float, 128> arr {};
                for (std::size_t i = 0; i < 64; ++i) {
                    arr[i]      = response.left[rstd::usize(i)];
                    arr[64 + i] = response.right[rstd::usize(i)];
                }
                host.PushAudioData(arr.data(), arr.size());
            }
        }

        // CEF's OSR pacing can go quiet after the first paint until something
        // on the page is dirty. Pages with rAF-driven animation expect a
        // presenter to ask for frames continuously. CEF dedupes internally
        // to its windowless_frame_rate, so an unconditional Invalidate per
        // loop iteration is the right pattern.
        host.Invalidate();

        if (ctx.need_swapchain_recreate) {
            auto metrics = GetWindowMetrics(window);
            if (metrics.framebuffer_width > 0 && metrics.framebuffer_height > 0 &&
                metrics.logical_width > 0 && metrics.logical_height > 0) {
                if (! presenter->Resize()) {
                    std::cerr << "webviewer: presenter Resize failed\n";
                    break;
                }
#if __is_target_os(macos)
                host.OnResize(metrics.logical_width, metrics.logical_height, metrics.scale);
#else
                host.OnResize(static_cast<int>(presenter->Width()),
                              static_cast<int>(presenter->Height()));
#endif
                ctx.need_swapchain_recreate = false;
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
                continue;
            }
        }

        // CEF copies OSR pixels into the presenter's bounded latest-frame
        // buffer; the render thread uploads that buffer before presenting.
        if (! presenter->RenderFrame()) {
            ctx.need_swapchain_recreate = true;
        }
    }

    host.Shutdown();
    presenter->Shutdown();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
