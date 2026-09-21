module;

#include <algorithm>
#include <cstring>

#if __is_target_os(macos)
#    include <mach-o/dyld.h>
#endif

module weweb;

import rstd;
import rstd.cppstd;
import :browser_host;
import :cef;
import :cef_internal;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::cppstd::to_string;
using rstd::ffi::CStr;
using rstd::ffi::OsStr;
using rstd::io::eprintln;
using rstd::os::unix::ffi::OsStrExt;
using rstd::path::Path;
using rstd::path::PathBuf;
using rstd::sync::atomic::Atomic;
using rstd::sync::atomic::Ordering;

namespace weweb
{

namespace
{
std::string CefPath(ref<Path> path) {
    auto os    = path.as_os_str();
    auto bytes = os.as_encoded_bytes();
    return { reinterpret_cast<const char*>(bytes.as_raw_ptr()), bytes.len().to_primitive() };
}
} // namespace

#if __is_target_os(macos)
namespace
{

PathBuf CurrentExecutablePath() {
    rstd::uint32_t size = 0;
    if (_NSGetExecutablePath(nullptr, &size) != -1 || size == 0) return {};

    Vec<char> buffer;
    buffer.resize(usize(size), '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) return {};
    return PathBuf::from(ref<Path>(OsStrExt::from_bytes(CStr::from_ptr(buffer.data()).to_bytes())));
}

void AddFrameworkCandidate(Vec<PathBuf>& candidates, PathBuf path) {
    auto name = path.as_path().file_name();
    if (name && *name == ref<OsStr>("Chromium Embedded Framework.framework"_str)) {
        path.push("Chromium Embedded Framework"_str);
        name = path.as_path().file_name();
    }
    if (! name || *name != ref<OsStr>("Chromium Embedded Framework"_str)) return;

    auto metadata = rstd::fs::metadata(path.as_path());
    if (metadata.is_err() || ! metadata->is_file()) return;
    if (! candidates.iter().any([&](auto candidate) {
            return candidate->as_path() == path.as_path();
        })) {
        candidates.push(rstd::move(path));
    }
}

bool LoadCefFramework(bool helper, PathBuf* loaded_binary) {
    Vec<PathBuf> candidates;
    if (auto override_path = rstd::env::var_os("OWE_CEF_FRAMEWORK_PATH"_str);
        override_path && ! override_path->is_empty()) {
        AddFrameworkCandidate(candidates, PathBuf::from(rstd::move(*override_path)));
    }

    const auto executable = CurrentExecutablePath();
    if (! executable.is_empty()) {
        auto       parent         = executable.as_path().parent();
        const auto executable_dir = parent ? PathBuf::from(*parent) : PathBuf {};
        const auto main_root      = executable_dir.join("../Frameworks"_str);
        const auto helper_root    = executable_dir.join("../../.."_str);
        if (helper) {
            AddFrameworkCandidate(
                candidates,
                helper_root.join(
                    "Chromium Embedded Framework.framework/Chromium Embedded Framework"_str));
        }
        // A single executable can also be used as the browser subprocess
        // entry point. In that layout the helper process still loads from the
        // main app's Contents/Frameworks directory.
        AddFrameworkCandidate(
            candidates,
            main_root.join(
                "Chromium Embedded Framework.framework/Chromium Embedded Framework"_str));

        // Development builds may not be wrapped in an application bundle yet.
        AddFrameworkCandidate(
            candidates,
            executable_dir.join(
                "Chromium Embedded Framework.framework/Chromium Embedded Framework"_str));
        AddFrameworkCandidate(
            candidates,
            executable_dir.join(
                "../Chromium Embedded Framework.framework/Chromium Embedded Framework"_str));
        AddFrameworkCandidate(
            candidates,
            executable_dir.join(
                "../Frameworks/Chromium Embedded Framework.framework/Chromium Embedded Framework"_str));
    }

    for (const auto& candidate : candidates) {
        if (cef_load_library(CefPath(candidate.as_path()).c_str())) {
            if (loaded_binary != nullptr) *loaded_binary = candidate.clone();
            if (! helper) {
                eprintln("weweb: loaded CEF framework from {}",
                         candidate.as_path().as_os_str().display());
            }
            return true;
        }
    }

    eprintln("weweb: unable to load Chromium Embedded Framework; "
             "set OWE_CEF_FRAMEWORK_PATH to the framework binary");
    return false;
}

bool IsCefHelperProcess(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (argv[i] != nullptr &&
            CStr::from_ptr(argv[i]).to_bytes().starts_with("--type="_str.as_bytes()))
            return true;
    }
    return false;
}

} // namespace
#endif

struct BrowserHost::Impl {
    CefRefPtr<AppHandler>            app;
    CefRefPtr<OsrRenderHandler>      osr;
    CefRefPtr<ClientHandler>         client;
    Option<AcceleratedPaintCallback> accel_cb;
    Option<CpuPaintCallback>         cpu_cb;
    Option<AudioDemandCallback>      audio_demand_cb;
    Atomic<bool>                     should_exit { false };
    bool                             initialised { false };
    // Stash the original argv from RunOrExitIfHelper; CefInitialize needs
    // the real argv to derive the per-child --type=… / --icu-data-file=…
    // switches it forwards to subprocesses.
    int    saved_argc { 0 };
    char** saved_argv { nullptr };
#if __is_target_os(macos)
    bool    cef_loaded { false };
    PathBuf cef_framework_binary;
#endif
};

BrowserHost::BrowserHost(): impl_(Box<Impl>::make()) { impl_->app = new AppHandler(); }

BrowserHost::~BrowserHost() { Shutdown(); }

int BrowserHost::RunOrExitIfHelper(int argc, char** argv) {
    impl_->saved_argc = argc;
    impl_->saved_argv = argv;
#if __is_target_os(macos)
    if (! impl_->cef_loaded) {
        if (! LoadCefFramework(IsCefHelperProcess(argc, argv), &impl_->cef_framework_binary)) {
            return 1;
        }
        impl_->cef_loaded = true;
    }
#endif
    CefMainArgs main_args(argc, argv);
    const int   result = CefExecuteProcess(main_args, impl_->app.get(), nullptr);
#if __is_target_os(macos)
    if (result >= 0) {
        cef_unload_library();
        impl_->cef_loaded = false;
    }
#endif
    return result;
}

bool BrowserHost::Init(const InitOptions& opts) {
    if (impl_->initialised) {
        eprintln("weweb: BrowserHost::Init called twice");
        return false;
    }

#if __is_target_os(macos)
    if (! impl_->cef_loaded) {
        if (! LoadCefFramework(false, &impl_->cef_framework_binary)) return false;
        impl_->cef_loaded = true;
    }
#endif

    CefMainArgs main_args(impl_->saved_argc, impl_->saved_argv);

    CefSettings settings;
    settings.no_sandbox                   = true;
    settings.windowless_rendering_enabled = true; // OSR mode
    settings.multi_threaded_message_loop  = false;
    settings.log_severity                 = LOGSEVERITY_WARNING;
    // WW_CEF_DEBUG=1 ⇒ flip CEF's own log threshold so the chromium VLOG
    // stream actually fires (--enable-logging=stderr alone is gated by
    // settings.log_severity). WW_CEF_LOG_FILE redirects the file sink.
    if (auto dbg = rstd::env::var_os("WW_CEF_DEBUG"_str);
        dbg && ! dbg->is_empty() && dbg->as_os_str().as_encoded_bytes()[usize()] != u8('0')) {
        settings.log_severity = LOGSEVERITY_VERBOSE;
    }
    if (auto lf = rstd::env::var_os("WW_CEF_LOG_FILE"_str); lf && ! lf->is_empty()) {
        CefString(&settings.log_file) = CefPath(ref<Path>(lf->as_os_str()));
    }

    auto set_cef_path = [](cef_string_t* dest, ref<Path> p) {
        if (p.is_empty()) return;
        CefString cef_str { dest };
        cef_str = CefPath(p);
    };
#if __is_target_os(macos)
    // A single executable is used for the browser and CEF subprocesses in
    // development builds. The same entry point calls CefExecuteProcess
    // before entering the browser loop, so no helper app is required.
    set_cef_path(&settings.browser_subprocess_path, CurrentExecutablePath());
    if (auto override_path = rstd::env::var_os("OWE_CEF_FRAMEWORK_PATH"_str);
        override_path && ! override_path->is_empty()) {
        auto framework_path = PathBuf::from(rstd::move(*override_path));
        auto name           = framework_path.as_path().file_name();
        if (name && *name == ref<OsStr>("Chromium Embedded Framework"_str)) {
            auto parent    = framework_path.as_path().parent();
            framework_path = parent ? PathBuf::from(*parent) : PathBuf {};
        }
        set_cef_path(&settings.framework_dir_path, framework_path);
    }
    if (settings.framework_dir_path.length == 0 && ! impl_->cef_framework_binary.is_empty()) {
        if (auto parent = impl_->cef_framework_binary.as_path().parent())
            set_cef_path(&settings.framework_dir_path, *parent);
    }
#endif
    set_cef_path(&settings.resources_dir_path, opts.resources_dir);
    set_cef_path(&settings.locales_dir_path, opts.locales_dir);
    set_cef_path(&settings.root_cache_path, opts.cache_dir);

    if (opts.enable_remote_debugging && opts.remote_debugging_port > 0) {
        settings.remote_debugging_port = opts.remote_debugging_port;
    }

    // Stash before CefInitialize; AppHandler::OnBeforeCommandLineProcessing
    // runs synchronously inside it and reads the flag.
    impl_->app->SetMuteAudio(! opts.enable_audio);
    impl_->app->SetSharedTextureEnabled(opts.shared_texture_enabled);
    impl_->app->SetRenderNodeOverride(opts.render_node_override.as_str());

    if (! CefInitialize(main_args, settings, impl_->app.get(), nullptr)) {
        eprintln("weweb: CefInitialize failed");
#if __is_target_os(macos)
        cef_unload_library();
        impl_->cef_loaded = false;
#endif
        return false;
    }
    impl_->initialised = true;
    return true;
}

bool BrowserHost::OpenWallpaper(const WebManifest& manifest, ref<Path> workshop_dir, int width,
                                int height) {
    return OpenWallpaper(manifest, workshop_dir, width, height, OpenOptions {});
}

bool BrowserHost::OpenWallpaper(const WebManifest& manifest, ref<Path> workshop_dir, int width,
                                int height, OpenOptions opts) {
    if (! impl_->initialised) {
        eprintln("weweb: OpenWallpaper before Init");
        return false;
    }

    impl_->osr = new OsrRenderHandler();
    impl_->osr->SetViewSize(width, height);
    if (impl_->accel_cb) {
        impl_->osr->SetAcceleratedPaintCallback(impl_->accel_cb.clone());
    }
    if (impl_->cpu_cb) {
        impl_->osr->SetCpuPaintCallback(impl_->cpu_cb.clone());
    }
    impl_->osr->SetDeviceScaleFactor(opts.device_scale_factor);

    impl_->client =
        new ClientHandler(manifest.user_props.clone(), impl_->osr, opts.initially_muted);
    impl_->client->SetAudioDemandCallback(impl_->audio_demand_cb.clone());
    impl_->client->SetCloseCallback(Box<dyn<Fn<void()>>>::make([this] {
        impl_->should_exit.store(true, Ordering::SeqCst);
    }));

    auto        entry = PathBuf::from(workshop_dir).join(manifest.entry_html.as_str());
    std::string url   = "file://" + CefPath(entry.as_path());

    CefWindowInfo info;
    info.SetAsWindowless(0); // no parent window — pure OSR
    info.shared_texture_enabled = opts.shared_texture_enabled ? 1 : 0;

    CefBrowserSettings browser_settings;
    browser_settings.windowless_frame_rate = opts.frame_rate > 0 ? opts.frame_rate : 60;

    const bool created = CefBrowserHost::CreateBrowser(
        info, impl_->client.get(), url, browser_settings, nullptr, nullptr);
    if (! created) {
        eprintln("weweb: CefBrowserHost::CreateBrowser failed for {}",
                 rstd::cppstd::as_str(url).unwrap());
    }
    return created;
}

void BrowserHost::SetAcceleratedPaintCallback(Option<AcceleratedPaintCallback> cb) {
    impl_->accel_cb = rstd::move(cb);
    if (impl_->osr) impl_->osr->SetAcceleratedPaintCallback(impl_->accel_cb.clone());
}

void BrowserHost::SetCpuPaintCallback(Option<CpuPaintCallback> cb) {
    impl_->cpu_cb = rstd::move(cb);
    if (impl_->osr) impl_->osr->SetCpuPaintCallback(impl_->cpu_cb.clone());
}

void BrowserHost::SetAudioResponseDemandCallback(Option<AudioDemandCallback> cb) {
    impl_->audio_demand_cb = rstd::move(cb);
    if (impl_->client) impl_->client->SetAudioDemandCallback(impl_->audio_demand_cb.clone());
}

void BrowserHost::Invalidate() {
    if (! impl_->client) return;
    auto b = impl_->client->GetBrowser();
    if (b && b->GetHost()) b->GetHost()->Invalidate(PET_VIEW);
}

void BrowserHost::OnResize(int width, int height) { OnResize(width, height, 1.0f); }

void BrowserHost::OnResize(int width, int height, float device_scale_factor) {
    if (width <= 0 || height <= 0) return;
    if (! impl_->osr) return;
    impl_->osr->SetViewSize(width, height);
    impl_->osr->SetDeviceScaleFactor(device_scale_factor);
    if (! impl_->client) return;
    if (auto b = impl_->client->GetBrowser(); b && b->GetHost()) {
        b->GetHost()->WasResized();
    }
}

void BrowserHost::OnMouseMove(int x, int y, bool left_down) {
    if (! impl_->client) return;
    auto b = impl_->client->GetBrowser();
    if (! b || ! b->GetHost()) return;
    CefMouseEvent ev;
    ev.x         = x;
    ev.y         = y;
    ev.modifiers = left_down ? EVENTFLAG_LEFT_MOUSE_BUTTON : 0;
    b->GetHost()->SendMouseMoveEvent(ev, /*mouseLeave=*/false);
}

void BrowserHost::OnMouseButton(int x, int y, int cef_button, bool down, int click_count) {
    if (! impl_->client) return;
    auto b = impl_->client->GetBrowser();
    if (! b || ! b->GetHost()) return;
    CefMouseEvent ev;
    ev.x = x;
    ev.y = y;
    b->GetHost()->SendMouseClickEvent(ev,
                                      static_cast<cef_mouse_button_type_t>(cef_button),
                                      /*mouseUp=*/! down,
                                      click_count);
}

void BrowserHost::OnMouseWheel(int x, int y, int delta_x, int delta_y) {
    if (! impl_->client) return;
    auto b = impl_->client->GetBrowser();
    if (! b || ! b->GetHost()) return;
    CefMouseEvent ev;
    ev.x = x;
    ev.y = y;
    b->GetHost()->SendMouseWheelEvent(ev, delta_x, delta_y);
}

void BrowserHost::OnKey(int cef_key_event_type, int native_key_code, int windows_key_code,
                        int modifiers, unsigned int unicode_char) {
    if (! impl_->client) return;
    auto b = impl_->client->GetBrowser();
    if (! b || ! b->GetHost()) return;
    CefKeyEvent ev;
    ev.type                 = static_cast<cef_key_event_type_t>(cef_key_event_type);
    ev.native_key_code      = native_key_code;
    ev.windows_key_code     = windows_key_code;
    ev.modifiers            = modifiers;
    ev.character            = static_cast<char16_t>(unicode_char);
    ev.unmodified_character = ev.character;
    b->GetHost()->SendKeyEvent(ev);
}

void BrowserHost::OnFocus(bool gained) {
    if (! impl_->client) return;
    auto b = impl_->client->GetBrowser();
    if (! b || ! b->GetHost()) return;
    b->GetHost()->SetFocus(gained);
}

void BrowserHost::Pump() {
    if (impl_->initialised) CefDoMessageLoopWork();
}

void BrowserHost::ApplyVolume(float volume) {
    if (impl_->client) impl_->client->SetAudioMuted(volume <= 0.0f);
    auto object = rstd::json::Map::make();
    object.insert("value"_Str, rstd::into<owe::Json>(f32(volume)));
    auto v = owe::Json::Object(rstd::move(object));
    ApplyUserProperty("audio"_str, v);
}

void BrowserHost::SetFrameRate(int fps) {
    if (fps <= 0 || ! impl_->client) return;
    auto b = impl_->client->GetBrowser();
    if (b && b->GetHost()) b->GetHost()->SetWindowlessFrameRate(fps);
}

void BrowserHost::SetPaused(bool paused) {
    if (! impl_->client) return;
    auto b = impl_->client->GetBrowser();
    if (b && b->GetHost()) b->GetHost()->WasHidden(paused);
}

void BrowserHost::ApplyUserProperty(ref<str> key, const owe::Json& value) {
    if (! impl_->client) return;
    auto browser = impl_->client->GetBrowser();
    if (! browser) return;
    auto frame = browser->GetMainFrame();
    if (! frame) return;
    auto snippet = BuildPropertyPatchSnippet(key, value);
    frame->ExecuteJavaScript(
        to_string(snippet.as_str()), "weweb://internal/apply_user_property.js", 0);
}

void BrowserHost::PushAudioData(slice<float> data) {
    if (! impl_->client || data.is_empty()) return;
    auto browser = impl_->client->GetBrowser();
    if (! browser) return;
    auto frame = browser->GetMainFrame();
    if (! frame) return;
    auto snippet = BuildAudioResponseSnippet(data);
    frame->ExecuteJavaScript(to_string(snippet.as_str()), "weweb://internal/push_audio.js", 0);
}

bool BrowserHost::ShouldExit() const { return impl_->should_exit.load(Ordering::SeqCst); }

void BrowserHost::RequestClose() { impl_->should_exit.store(true, Ordering::SeqCst); }

void BrowserHost::Shutdown() {
    if (impl_->initialised) {
        CefShutdown();
        impl_->initialised = false;
    }
#if __is_target_os(macos)
    if (impl_->cef_loaded) {
        cef_unload_library();
        impl_->cef_loaded = false;
    }
#endif
}

} // namespace weweb
