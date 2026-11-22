module;

#include <cstring>

export module owe.cli.web;

import rstd.argparse;
import rstd.cppstd;
import owe.audio_response;
import wescene.cli;
import weweb;
import wavsen.audio;
import owe.cli;
import owe.cli.web.vulkan_blitter;
import vui.host;

#if defined(LITO_FEAT_WAYWALLEN) && __is_target_os(linux)
import owe.cli.web.waywallen;
#endif

using rstd::time::Duration;

namespace
{

using namespace rstd::prelude;
using namespace rstd::argparse;
using namespace rstd::literals;
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
    auto command  = Command::make("owe-web"_str);
    auto workshop = command.add_arg(
        Arg<String>::value("workshop"_str, string_parser())
            .value_name("WORKSHOP"_str)
            .help("path to a workshop/<id>/ directory containing project.json + index.html"_str)
            .required());
    auto width  = command.add_arg(Arg<i32>::value("width"_str, from_str_parser<i32>())
                                      .long_name("width"_str)
                                      .help("initial window width in logical pixels"_str)
                                      .default_value("1280"_str));
    auto height = command.add_arg(Arg<i32>::value("height"_str, from_str_parser<i32>())
                                      .long_name("height"_str)
                                      .help("initial window height in logical pixels"_str)
                                      .default_value("720"_str));
    auto remote_debugging_port =
        command.add_arg(Arg<i32>::value("remote-debugging-port"_str, from_str_parser<i32>())
                            .long_name("remote-debugging-port"_str)
                            .help("if non-zero, expose chrome devtools on this localhost port"_str)
                            .default_value("0"_str));
    auto presenter = command.add_arg(Arg<String>::value("presenter"_str, string_parser())
                                         .long_name("presenter"_str)
                                         .help("present backend: vulkan"_str)
                                         .default_value("vulkan"_str));

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

auto WindowBackend() -> vui::host::WindowBackend {
    using vui::host::WindowBackend;
#if __is_target_os(linux)
    if (auto backend = rstd::env::var("OWE_WINDOW_BACKEND"_str); backend.is_ok()) {
        if (backend->as_str() == "x11"_str) return WindowBackend::X11;
        if (backend->as_str() == "wayland"_str) return WindowBackend::Wayland;
    }
    if (viewer::EnvEnabled("WP_GLFW_X11"_str)) return WindowBackend::X11;
#endif
    return WindowBackend::Automatic;
}

int CefButton(vui::PointerButton button) {
    switch (button) {
    case vui::PointerButton::Primary: return 0;
    case vui::PointerButton::Middle: return 1;
    case vui::PointerButton::Secondary: return 2;
    default: return -1;
    }
}

struct WindowMetrics {
    int   logical_width { 0 };
    int   logical_height { 0 };
    int   framebuffer_width { 0 };
    int   framebuffer_height { 0 };
    float scale { 1.0f };

    bool operator==(const WindowMetrics&) const = default;
    bool Valid() const {
        return logical_width > 0 && logical_height > 0 && framebuffer_width > 0 &&
               framebuffer_height > 0;
    }
};

WindowMetrics GetWindowMetrics(const vui::host::Window& window) {
    WindowMetrics metrics;
    if (auto size = window.logical_size()) {
        metrics.logical_width  = static_cast<int>(size->width);
        metrics.logical_height = static_cast<int>(size->height);
    }
    auto extent                = window.pixel_extent();
    metrics.framebuffer_width  = static_cast<int>(extent.width);
    metrics.framebuffer_height = static_cast<int>(extent.height);
    if (metrics.Valid())
        metrics.scale = static_cast<float>(metrics.framebuffer_width) / metrics.logical_width;
    return metrics;
}

struct BrowserView {
    int   width;
    int   height;
    float scale;
};

BrowserView GetBrowserView(const WindowMetrics& metrics, const weweb::VulkanBlitter& presenter) {
#if __is_target_os(macos)
    (void)presenter;
    return { metrics.logical_width, metrics.logical_height, metrics.scale };
#else
    (void)metrics;
    return { static_cast<int>(presenter.Width()), static_cast<int>(presenter.Height()), 1.0f };
#endif
}

void ForwardInput(weweb::BrowserHost& host, const vui::InputEvent& event,
                  const WindowMetrics& metrics, BrowserView view, bool& left_down) {
    using vui::EventType;
    const auto x = static_cast<int>(event.position.x * view.width / metrics.logical_width);
    const auto y = static_cast<int>(event.position.y * view.height / metrics.logical_height);
    switch (event.type) {
    case EventType::WindowPointerEnter:
    case EventType::PointerMove: host.OnMouseMove(x, y, left_down); break;
    case EventType::PointerDown:
    case EventType::PointerUp:
        if (int button = CefButton(event.button); button >= 0) {
            const bool down = event.type == EventType::PointerDown;
            if (button == 0) left_down = down;
            host.OnMouseButton(x, y, button, down, 1);
        }
        break;
    case EventType::Wheel: {
        const auto unit = event.scroll_unit == vui::ScrollUnit::Lines ? 40.0f : 1.0f;
        // vui expresses scrolling in content coordinates; CEF uses wheel direction.
        host.OnMouseWheel(x,
                          y,
                          static_cast<int>(-event.scroll_delta.x * unit),
                          static_cast<int>(-event.scroll_delta.y * unit));
        break;
    }
    case EventType::PointerCancel:
        if (left_down) host.OnMouseButton(x, y, 0, false, 1);
        left_down = false;
        break;
    case EventType::WindowFocusGained: host.OnFocus(true); break;
    case EventType::WindowFocusLost:
    case EventType::InputReset:
        if (left_down) host.OnMouseButton(x, y, 0, false, 1);
        left_down = false;
        host.OnFocus(event.type == EventType::InputReset && event.window_focused &&
                     *event.window_focused);
        break;
    default: break;
    }
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

int RunPreview(int argc, char** argv) {
    weweb::BrowserHost host;
    // CEF re-executes this binary with its own arguments.
    if (int helper_exit = host.RunOrExitIfHelper(argc, argv); helper_exit >= 0) return helper_exit;

    if (argc > 1 && std::strcmp(argv[1], "--waywallen") == 0) {
        eprintln("owe-web: --waywallen requires a Linux build with the waywallen feature");
        return 2;
    }

    auto parsed_args = ParseWebViewerArgs(argc, argv);
    if (parsed_args.is_err()) return parsed_args.unwrap_err().code;
    auto args = rstd::move(parsed_args).unwrap();

    auto presenter_name = args.presenter.as_str();
    if (presenter_name == "egl"_str) {
        eprintln("owe-web: the EGL presenter was removed; use --presenter vulkan");
        return 2;
    }
    if (presenter_name != "vulkan"_str) {
        eprintln("owe-web: --presenter must be 'vulkan', got '{}'", presenter_name);
        return 2;
    }
    const auto width  = args.width.to_primitive();
    const auto height = args.height.to_primitive();
    if (width <= 0 || height <= 0 || width > 32768 || height > 32768) {
        eprintln("owe-web: window dimensions must be between 1 and 32768");
        return 2;
    }

    auto workshop_dir = PathBuf::from(args.workshop.as_str());
    auto metadata     = rstd::fs::metadata(workshop_dir.as_path());
    if (metadata.is_err() || ! metadata->is_dir()) {
        eprintln("owe-web: not a directory: {}", workshop_dir.as_path().as_os_str().display());
        return 2;
    }

    auto manifest_opt = weweb::LoadWebManifest(workshop_dir);
    if (! manifest_opt) return 2;

    vui::host::Window window;
    const auto        title = rstd::cppstd::to_string(manifest_opt->title.as_str());
    if (window.open(title.c_str(), width, height, WindowBackend()).is_err() ||
        window.synchronize().is_err() || ! window.configured()) {
        eprintln("owe-web: failed to open preview window");
        return 1;
    }

    weweb::VulkanBlitter presenter;
    auto                 metrics = GetWindowMetrics(window);
    if (! metrics.Valid() || ! presenter.Init(window)) return 1;
    auto view = GetBrowserView(metrics, presenter);

    auto                            exe_dir = viewer::ExecutableDir(argv[0]);
    weweb::BrowserHost::InitOptions opts;
    opts.resources_dir = exe_dir.clone();
    opts.locales_dir   = exe_dir.join("locales"_str);
#if __is_target_os(linux)
    if (window.backend() == vui::host::WindowBackend::X11)
        opts.ozone_platform = weweb::BrowserHost::OzonePlatform::X11;
#endif
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

    if (! host.Init(opts)) return 1;

    // CEF reclaims the borrowed paint resource when the callback returns.
#if __is_target_os(linux)
    host.SetAcceleratedPaintCallback([&presenter](const weweb::DmaBufFrame& frame) {
        presenter.AcceptDmaBuf(frame);
    });
#else
    host.SetCpuPaintCallback([&presenter](const weweb::CpuPaintFrame& frame) {
        presenter.AcceptCpuPaint(frame);
    });
#endif

    weweb::BrowserHost::OpenOptions open_opts;
    open_opts.device_scale_factor = view.scale;
#if __is_target_os(macos)
    open_opts.shared_texture_enabled = false;
#endif
    if (! host.OpenWallpaper(*manifest_opt, workshop_dir, view.width, view.height, open_opts)) {
        host.Shutdown();
        return 1;
    }

    rstd::sync::atomic::Atomic<bool> audio_response_demand { false };
    host.SetAudioResponseDemandCallback([&audio_response_demand](bool active) {
        audio_response_demand.store(active, rstd::sync::atomic::Ordering::Release);
    });

    wavsen::audio::AudioCapture audio_capture;
    owe::audio::ResponseEngine  audio_response;
    unsigned                    audio_tick     = 0;
    bool                        left_down      = false;
    bool                        resize_pending = false;
    bool                        suspended      = false;
    int                         exit_code      = 0;
    while (! host.ShouldExit()) {
        host.Pump();
        if (host.IsClosing()) {
            rstd::thread::sleep(Duration::from_millis(u64(10)));
            continue;
        }

        auto events = window.wait(suspended ? 16 : 0);
        if (events.is_err()) {
            eprintln("owe-web: window event processing failed");
            exit_code = 1;
            host.RequestClose();
            continue;
        }
        if (events->close_requested) {
            host.RequestClose();
            continue;
        }

        if (! audio_response_demand.load(rstd::sync::atomic::Ordering::Acquire)) {
            if (audio_capture.is_inited()) audio_capture.uninit();
            audio_response.end();
        } else if (! audio_capture.is_inited() && ! audio_capture.init()) {
            eprintln("owe-web: audio capture init failed");
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

        const auto next_metrics = GetWindowMetrics(window);
        suspended = events->minimized.unwrap_or(false) || events->suspended.unwrap_or(false) ||
                    ! next_metrics.Valid();
        if (suspended) continue;

        if (resize_pending || next_metrics != metrics) {
            if (! presenter.Resize()) {
                eprintln("owe-web: presenter resize failed");
                exit_code = 1;
                host.RequestClose();
                continue;
            }
            metrics = next_metrics;
            view    = GetBrowserView(metrics, presenter);
            host.OnResize(view.width, view.height, view.scale);
        }
        for (const auto& event : events->input) ForwardInput(host, event, metrics, view, left_down);

        // Keep CEF's OSR paint loop active for animated pages.
        host.Invalidate();
        resize_pending = ! presenter.RenderFrame();
    }

    host.Shutdown();
    return exit_code;
}

} // namespace

extern "C++" int main(int argc, char** argv) {
#if defined(LITO_FEAT_WAYWALLEN) && __is_target_os(linux)
    if (argc > 1 && std::strcmp(argv[1], "--waywallen") == 0) {
        // The bridge entry handles CEF helpers before parsing the remaining arguments.
        argv[1] = argv[0];
        return waywallen::run(argc - 1, argv + 1);
    }
#endif
    return RunPreview(argc, argv);
}
