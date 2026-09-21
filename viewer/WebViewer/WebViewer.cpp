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

import viewer.glfw_vulkan;

using rstd::time::Duration;

using namespace viewer::glfw;

namespace
{

using namespace rstd::prelude;
using namespace rstd::argparse;
using namespace rstd::literals;
using rstd::ffi::CStr;
using rstd::ffi::OsStr;
using rstd::io::eprintln;
using rstd::os::unix::ffi::OsStrExt;
using rstd::path::Path;
using rstd::path::PathBuf;

struct WebViewerArgs {
    String workshop;
    String presenter;
    i32    width;
    i32    height;
    i32    remote_debugging_port;
};

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
        .workshop              = Value(matches, workshop).clone(),
        .presenter             = Value(matches, presenter).clone(),
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
    case viewer::glfw::MouseButtonLeft: return 0;
    case viewer::glfw::MouseButtonMiddle: return 1;
    case viewer::glfw::MouseButtonRight: return 2;
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
    bool left = glfwGetMouseButton(w, viewer::glfw::MouseButtonLeft) == viewer::glfw::Press;
    ctx->host->OnMouseMove(static_cast<int>(x), static_cast<int>(y), left);
}

void OnMouseButton(GLFWwindow* w, int button, int action, int /*mods*/) {
    auto* ctx = static_cast<ViewerCtx*>(glfwGetWindowUserPointer(w));
    if (! ctx || ! ctx->host) return;
    int cef_btn = CefButtonFromGlfw(button);
    if (cef_btn < 0) return;
    double x = 0, y = 0;
    glfwGetCursorPos(w, &x, &y);
    ctx->host->OnMouseButton(static_cast<int>(x),
                             static_cast<int>(y),
                             cef_btn,
                             action == viewer::glfw::Press,
                             /*click_count=*/1);
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
    if (ctx && ctx->host) ctx->host->OnFocus(focused == viewer::glfw::TrueValue);
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
PathBuf CefFrameworkRoot(const PathBuf& exe_dir) {
    Vec<PathBuf> candidates;
    if (auto override_path = rstd::env::var_os("OWE_CEF_FRAMEWORK_PATH"_str);
        override_path && ! override_path->is_empty()) {
        auto path = PathBuf::from(rstd::move(*override_path));
        auto name = path.as_path().file_name();
        if (name && *name == ref<OsStr>("Chromium Embedded Framework"_str)) {
            auto parent = path.as_path().parent();
            path        = parent ? PathBuf::from(*parent) : PathBuf {};
        }
        candidates.push(rstd::move(path));
    }
    candidates.push(exe_dir.join("../Frameworks/Chromium Embedded Framework.framework"_str));
    candidates.push(exe_dir.join("Chromium Embedded Framework.framework"_str));
    candidates.push(exe_dir.join("../Chromium Embedded Framework.framework"_str));
    for (const auto& candidate : candidates) {
        auto metadata = rstd::fs::metadata(candidate.join("Resources"_str).as_path());
        if (metadata.is_ok() && metadata->is_dir()) return candidate.clone();
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

    auto workshop_dir = PathBuf::from(args.workshop.as_str());
    auto metadata     = rstd::fs::metadata(workshop_dir.as_path());
    if (metadata.is_err() || ! metadata->is_dir()) {
        eprintln("webviewer: not a directory: {}", workshop_dir.as_path().as_os_str().display());
        return 2;
    }

    auto presenter_name = args.presenter.as_str();
#if defined(__linux__)
    if (presenter_name != "vulkan"_str && presenter_name != "egl"_str) {
        eprintln("webviewer: --presenter must be 'vulkan' or 'egl', got '{}'", presenter_name);
        return 2;
    }
#else
    if (presenter_name != "vulkan"_str) {
        eprintln("webviewer: macOS requires --presenter vulkan, got '{}'", presenter_name);
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
    if (presenter_name == "vulkan"_str) {
        auto loaded = vvk::VulkanLoader::Open();
        if (loaded.is_err()) {
            eprintln("webviewer: Vulkan loader open failed");
            return 1;
        }
        vulkan_loader = Some(loaded.unwrap_unchecked());
        viewer::glfw::glfwInitVulkanLoader(
            vulkan_loader.as_ref().unwrap_unchecked().global().vkGetInstanceProcAddr);
    }
    if (! glfwInit()) {
        eprintln("webviewer: glfwInit failed");
        return 1;
    }
    if (presenter_name == "vulkan"_str && ! glfwVulkanSupported()) {
        eprintln("webviewer: glfw says Vulkan is not supported");
        glfwTerminate();
        return 1;
    }
    // The presenter owns the graphics context/device.
    glfwWindowHint(viewer::glfw::ClientApi, viewer::glfw::NoApi);
#if __is_target_os(macos)
    glfwWindowHint(viewer::glfw::CocoaRetinaFramebuffer, viewer::glfw::TrueValue);
#endif

    int         w_width  = args.width.to_primitive();
    int         w_height = args.height.to_primitive();
    GLFWwindow* window = glfwCreateWindow(w_width,
                                          w_height,
                                          rstd::cppstd::to_string(manifest.title.as_str()).c_str(),
                                          nullptr,
                                          nullptr);
    if (! window) {
        eprintln("webviewer: glfwCreateWindow failed");
        glfwTerminate();
        return 1;
    }

    Option<Box<dyn<weweb::PresenterObject>>> presenter_owner;
    if (presenter_name == "vulkan"_str) {
        presenter_owner = Some(weweb::Presenter::Make<weweb::VulkanBlitter>());
#if defined(__linux__)
    } else {
        presenter_owner = Some(weweb::Presenter::Make<weweb::EglPresenter>());
#endif
    }
    auto* presenter = &(*presenter_owner)->AsPresenter();
    if (! presenter->Init(window)) {
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    auto exe_dir = viewer::ExecutableDir(argv[0]);

    weweb::BrowserHost::InitOptions opts;
    opts.resources_dir = exe_dir.clone();
    opts.locales_dir   = exe_dir.join("locales"_str);
#if __is_target_os(macos)
    if (auto framework_root = CefFrameworkRoot(exe_dir); ! framework_root.is_empty()) {
        opts.resources_dir = framework_root.join("Resources"_str);
        opts.locales_dir   = opts.resources_dir.join("locales"_str);
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
    weweb::Presenter* presenter_ptr = presenter;
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
    ctx.presenter = presenter;
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
                eprintln("webviewer: audio capture init failed");
            }
        }
        if (audio_capture.is_inited() && (audio_tick++ & 1u) == 0) {
            wavsen::audio::AudioPcmWindow captured {};
            owe::audio::ResponseFrame     response {};
            auto                          window = owe::audio::PcmWindow {};
            if (audio_capture.snapshot(captured)) window = viewer::ConvertAudioWindow(captured);
            if (window.frames != 0 && audio_response.analyze(window, response)) {
                array<float, 128> arr {};
                for (usize i {}; i < usize(64); ++i) {
                    arr[i]             = response.left[i];
                    arr[usize(64) + i] = response.right[i];
                }
                host.PushAudioData(arr.as_slice());
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
                    eprintln("webviewer: presenter Resize failed");
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
                rstd::thread::sleep(Duration::from_millis(u64(16)));
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
