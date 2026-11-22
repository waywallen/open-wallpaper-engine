module;

#include <cstring>

export module owe.cli.scene;

import vvk;
import vui.host;
import rstd;
import rstd.bench;
import rstd.log;
import wavsen.audio;
import wescene.core;
import wescene.json;
import wescene.render_cli;
import wescene.scene_wallpaper;
import wescene.utils;
import owe.cli;
import :arg;
import :stdin_control;

#if defined(LITO_FEAT_WAYWALLEN) && __is_target_os(linux)
import owe.cli.scene.waywallen;
#endif

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::io::eprintln;
using rstd::io::println;
using rstd::sync::atomic::Atomic;
using rstd::sync::atomic::Ordering;
using rstd::time::Duration;
using rstd::time::Instant;

namespace
{

auto SceneWindowBackend() -> vui::host::WindowBackend {
    using vui::host::WindowBackend;
#if __is_target_os(linux)
    auto backend = rstd::env::var("OWE_WINDOW_BACKEND"_str);
    if (backend.is_ok()) {
        if (backend->as_str() == "x11"_str) return WindowBackend::X11;
        if (backend->as_str() == "wayland"_str) return WindowBackend::Wayland;
    }
    if (viewer::EnvEnabled("WP_GLFW_X11"_str)) return WindowBackend::X11;
#endif
    return WindowBackend::Automatic;
}

void ApplyWindowInput(owe::SceneWallpaper& scene, const vui::host::WindowEvents& events,
                      bool mouse_position_locked) {
    using vui::EventType;
    using vui::PointerButton;
    const auto size = events.surface.logical_size;
    for (const auto& event : events.input) {
        switch (event.type) {
        case EventType::WindowPointerEnter:
            if (! mouse_position_locked) scene.mouseEnter(true);
            break;
        case EventType::WindowPointerLeave:
            if (! mouse_position_locked) scene.mouseEnter(false);
            continue;
        case EventType::PointerDown:
        case EventType::PointerUp:
        case EventType::PointerMove: break;
        case EventType::InputReset:
            if (! mouse_position_locked) scene.mouseEnter(false);
            [[fallthrough]];
        case EventType::PointerCancel:
            for (int button = 0; button < 3; ++button) scene.mouseButton(button, false);
            continue;
        default: continue;
        }
        if (! mouse_position_locked && size.width > 0 && size.height > 0) {
            scene.mouseInput(event.position.x / size.width, event.position.y / size.height);
        }
        if (event.type == EventType::PointerDown || event.type == EventType::PointerUp) {
            const bool down = event.type == EventType::PointerDown;
            switch (event.button) {
            case PointerButton::Primary: scene.mouseButton(0, down); break;
            case PointerButton::Secondary: scene.mouseButton(1, down); break;
            case PointerButton::Middle: scene.mouseButton(2, down); break;
            default: break;
            }
        }
    }
}

int RunPreview(int argc, char** argv) {
    static rstd::log::EnvLogger _logger;
    rstd::log::set_logger(_logger);
    rstd::log::set_max_level(_logger.filter());

    auto args                = viewer::ParseSceneViewerArgs(argc, argv);
    auto [w_width, w_height] = args.resolution;
    const bool screenshot    = args.screenshot_path.is_some();
    const auto started       = Instant::now();
    auto       compile_only  = rstd::env::var_os("WP_COMPILE_ONLY"_str);
    const bool compiling     = compile_only && ! compile_only->is_empty();
    const bool offscreen     = screenshot || compiling || viewer::EnvEnabled("WP_HEADLESS"_str);
    if (screenshot) {
        if (compiling) {
            eprintln("--screenshot cannot be used with WP_COMPILE_ONLY");
            return 2;
        }
        if (args.fps <= i32() || args.fps > i32(65535)) {
            eprintln("--screenshot requires --fps between 1 and 65535");
            return 2;
        }
        auto output = owe::cli::ValidateScreenshotOutput(args.screenshot_path->as_path());
        if (output.is_err()) {
            eprintln("--screenshot: {}", output.unwrap_err());
            return 2;
        }
    }

    auto load_bench = owe::CreateSceneLoadBench(args.load_bench_output.as_str());
    Option<rstd::bench::probe::ProbeRecorder> startup_recorder;
    if (load_bench) startup_recorder = Some((**load_bench).session().recorder());
    auto startup_span = owe::SceneLoadSpan({ startup_recorder ? &*startup_recorder : nullptr,
                                             load_bench ? &(**load_bench).ids() : nullptr },
                                           &owe::SceneLoadProbeIds::viewer_startup);

    Option<Box<vui::host::Window>> window;
    owe::RenderInitInfo            info;
    info.enable_valid_layer = args.enable_valid_layer;
    info.offscreen          = offscreen;
    info.offscreen_export   = false;
    if (args.async_pipelines)
        info.pipeline_preparation = vrento::vulkan::PipelinePreparationMode::Deferred;
    info.width        = static_cast<rstd::uint16_t>(w_width);
    info.height       = static_cast<rstd::uint16_t>(w_height);
    info.msaa_samples = args.msaa_samples.to_primitive();

    if (! offscreen) {
        window       = Some(Box<vui::host::Window>::make());
        auto& host   = **window;
        auto  opened = host.open("OWE Scene", w_width, w_height, SceneWindowBackend());
        if (opened.is_err() || host.synchronize().is_err() || ! host.configured()) {
            eprintln("Failed to open the Scene preview window");
            return 1;
        }
        const auto extent = host.pixel_extent();
        if (! extent.width || ! extent.height || extent.width > 65535 || extent.height > 65535) {
            eprintln("Unsupported preview window extent: {}x{}", extent.width, extent.height);
            return 1;
        }
        info.width  = static_cast<rstd::uint16_t>(extent.width);
        info.height = static_cast<rstd::uint16_t>(extent.height);

        auto exts = host.instance_extensions();
        if (exts.is_err()) {
            eprintln("The preview window did not provide Vulkan instance extensions");
            return 1;
        }
        for (const auto& ext : *exts) info.surface_info.instanceExts.push(String::make(ext.name));
        info.surface_info.createSurfaceOp = Some(owe::CreateSurfaceCallback::make(
            [&host](const vvk::Instance& instance) -> owe::CreateSurfaceResult {
                auto surface = host.create_surface(instance);
                if (surface.is_ok()) return Ok(rstd::move(surface).unwrap());
                auto result = surface.as_ref().unwrap_err().result;
                return Err(vrento::vulkan::DeviceSessionError {
                    vrento::vulkan::DeviceSessionStage::Surface,
                    result == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : result });
            }));
    }

    auto         texture_memory = args.texture_system ? owe::TextureMemorySession::System()
                                                      : owe::TextureMemorySession::VirtualMemory();
    Atomic<bool> audio_response_demand { false };
    auto         wallpaper = Some(Box<owe::SceneWallpaper>::make(texture_memory.options()));
    auto*        psw       = &**wallpaper;

    psw->setAudioResponseDemandCallback([&audio_response_demand](bool active) {
        audio_response_demand.store(active, Ordering::Release);
    });

    if (! psw->init()) {
        eprintln("Failed to initialize scene renderer");
        return 1;
    }

    owe::SceneWallpaperConfig config;
    config.assets_dir      = rstd::move(args.assets_dir);
    config.source_pkg_path = rstd::move(args.scene_path);
    config.graphviz        = args.graphviz;
    config.load_bench      = load_bench.clone();
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

    (void)startup_span.finish();
    if (startup_recorder) {
        auto batch = startup_recorder->drain();
        if (batch.is_ok())
            (**load_bench).add_preload_batch(rstd::move(batch).unwrap_unchecked());
        else
            eprintln("cannot drain viewer startup probes");
        startup_recorder = None();
    }
    load_bench              = None();
    auto locked_mouse       = viewer::ParseMousePosition(args.mouse_position.as_str());
    auto apply_locked_mouse = [&]() {
        if (! locked_mouse) return;
        psw->mouseEnter(true);
        psw->mouseInput((*locked_mouse)[usize()], (*locked_mouse)[usize(1)]);
    };
    apply_locked_mouse();

    Option<rstd::sync::mpmc::Receiver<owe::FrameCaptureResult>> capture_receiver;
    if (screenshot) {
        auto [sender, receiver] = rstd::sync::mpmc::sync_channel<owe::FrameCaptureResult>(usize(1));
        capture_receiver        = Some(rstd::move(receiver));
        psw->requestFrameCapture(owe::FrameCaptureCallback::make(
            [sender = rstd::move(sender)](owe::FrameCaptureResult result) mutable {
                (void)sender.try_send(rstd::move(result));
            }));
    }
    psw->configure(rstd::move(config));
    psw->initVulkan(rstd::move(info));

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

    int exit_code = 0;
    if (screenshot) {
        const auto timeout = Duration::from_secs_f64(args.screenshot_timeout.to_primitive());
        exit_code          = 1;
        while (true) {
            if (started.elapsed() >= timeout) {
                eprintln("--screenshot: timed out after {} seconds", args.screenshot_timeout);
                break;
            }
            auto captured = capture_receiver->try_recv();
            if (captured.is_ok()) {
                auto result = rstd::move(captured).unwrap();
                if (result.is_err()) {
                    eprintln("--screenshot: {}", result.as_ref().unwrap_err().message.as_str());
                    break;
                }
                auto frame   = rstd::move(result).unwrap();
                auto written = owe::WriteFrameCapturePng(frame, args.screenshot_path->as_path());
                if (written.is_err()) {
                    eprintln("--screenshot: {}", written.as_ref().unwrap_err().message.as_str());
                } else if (started.elapsed() >= timeout) {
                    eprintln("--screenshot: timed out after {} seconds", args.screenshot_timeout);
                } else {
                    exit_code = 0;
                }
                break;
            }
            if (captured.unwrap_err() == rstd::sync::mpmc::TryRecvError::Disconnected) {
                eprintln("--screenshot: capture callback disconnected without a result");
                break;
            }
            stdin_control.poll(*psw);
            apply_locked_mouse();
            update_audio();
            rstd::thread::sleep(Duration::from_millis(u64(5)));
        }
    } else if (compiling) {
        auto text    = compile_only->as_os_str().to_str();
        auto seconds = text ? rstd::from_str<i32>(*text).unwrap_or(i32(2)) : i32(2);
        if (seconds <= i32()) seconds = i32(2);
        rstd::thread::sleep(Duration::from_secs(u64(seconds.to_primitive())));
    } else {
#if __is_target_os(macos)
        constexpr auto event_wait = 8u;
#else
        constexpr auto event_wait = 33u;
#endif
        while (true) {
            if (window) {
                auto events = (**window).wait(event_wait);
                if (events.is_err()) {
                    eprintln("Scene preview window event processing failed");
                    exit_code = 1;
                    break;
                }
                if (events->close_requested) break;
                ApplyWindowInput(*psw, *events, locked_mouse.is_some());
            } else {
                rstd::thread::sleep(Duration::from_millis(u64(event_wait)));
            }
            stdin_control.poll(*psw);
            apply_locked_mouse();
            update_audio();
        }
    }
    wallpaper = None();
    if (! args.texture_memory_output.is_empty()) {
        auto            stats = texture_memory.statistics();
        rstd::json::Map fields;
        auto            add = [&fields](ref<str> key, u64 value) {
            fields.insert(String::make(key), rstd::into<owe::Json>(value));
        };
        fields.insert("backend"_Str,
                      rstd::into<owe::Json>(String::make(stats.virtual_memory ? "virtual-memory"_str
                                                                              : "system"_str)));
        fields.insert("overflow"_Str, rstd::into<owe::Json>(stats.logical.overflow));
        add("live_bytes"_str, stats.logical.live_bytes);
        add("peak_live_bytes"_str, stats.logical.peak_live_bytes);
        add("allocations"_str, stats.logical.allocations);
        add("deallocations"_str, stats.logical.deallocations);
        add("failures"_str, stats.logical.failures);
        add("mapped_bytes"_str,
            stats.virtual_memory ? rstd::as_cast<u64>(stats.virtual_memory->mapped_bytes) : u64());
        add("mapped_bytes_peak"_str,
            stats.virtual_memory ? rstd::as_cast<u64>(stats.virtual_memory->mapped_bytes_peak)
                                 : u64());
        add("mappings"_str,
            stats.virtual_memory ? rstd::as_cast<u64>(stats.virtual_memory->mappings) : u64());
        add("mappings_peak"_str,
            stats.virtual_memory ? rstd::as_cast<u64>(stats.virtual_memory->mappings_peak) : u64());
        auto report = rstd::json::to_string(owe::Json::Object(rstd::move(fields)));
        auto path   = rstd::path::PathBuf::from(args.texture_memory_output.as_str());
        if (rstd::fs::write(path.as_path(), report.as_str().as_bytes()).is_err())
            eprintln("failed to write texture memory statistics");
    }
    return exit_code;
}

} // namespace

extern "C++" int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--waywallen") == 0) {
#if defined(LITO_FEAT_WAYWALLEN) && __is_target_os(linux)
        argv[1] = argv[0];
        return waywallen::run(argc - 1, argv + 1);
#else
        eprintln("--waywallen requires a Linux build with the waywallen feature");
        return 2;
#endif
    }
    return RunPreview(argc, argv);
}
