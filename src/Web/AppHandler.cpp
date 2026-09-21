module;

module weweb;

import rstd;
import rstd.cppstd;

import :cef;
import :cef_internal;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::cppstd::as_str;
using rstd::cppstd::to_string;

namespace weweb
{

namespace
{

constexpr const char* kAudioDemandMessage = "weweb.audio-demand";

void SendAudioDemand(CefRefPtr<CefFrame> frame, int generation, bool active) {
    if (! frame) return;
    auto message = CefProcessMessage::Create(kAudioDemandMessage);
    auto args    = message->GetArgumentList();
    args->SetInt(0, generation);
    args->SetBool(1, active);
    frame->SendProcessMessage(PID_BROWSER, message);
}

class AudioDemandHandler final : public CefV8Handler {
public:
    AudioDemandHandler(CefRefPtr<CefFrame> frame, int generation)
        : m_frame(rstd::move(frame)), m_generation(generation) {}

    bool Execute(const CefString&, CefRefPtr<CefV8Value>, const CefV8ValueList& arguments,
                 CefRefPtr<CefV8Value>&, CefString&) override {
        if (arguments.size() != 1 || ! arguments[0]->IsBool()) return false;
        SendAudioDemand(m_frame, m_generation, arguments[0]->GetBoolValue());
        return true;
    }

    void AddRef() const override { m_ref_count.AddRef(); }
    bool Release() const override {
        if (m_ref_count.Release()) {
            delete this;
            return true;
        }
        return false;
    }
    bool HasOneRef() const override { return m_ref_count.HasOneRef(); }
    bool HasAtLeastOneRef() const override { return m_ref_count.HasAtLeastOneRef(); }

private:
    CefRefPtr<CefFrame> m_frame;
    int                 m_generation;
    CefRefCount         m_ref_count;
};

} // namespace

AppHandler::AppHandler() = default;

/*
arg: --no-sandbox
arg: --lang=en-US
arg: --log-severity=warning
arg: --resources-dir-path=...
arg: --locales-dir-path=.../locales
arg: --disable-features=GlicActorUi,AutofillActorMode,LensOverlay
*/

void AppHandler::OnBeforeCommandLineProcessing(const CefString&          process_type,
                                               CefRefPtr<CefCommandLine> cmd) {
    // Only tweak the browser process command line. Renderer / utility
    // helpers inherit the relevant switches from the browser anyway.
    if (! process_type.empty()) return;

    // WE web wallpapers are loose directory trees loaded as `file://` URLs.
    cmd->AppendSwitch("allow-file-access-from-files");
    cmd->AppendSwitch("disable-web-security");
    cmd->AppendSwitch("allow-chrome-scheme-url");

    // The `minimal` CEF Linux distribution does not ship the SUID sandbox
    // helper. Match `CefSettings.no_sandbox = true` at the cmdline level
    // so the switch propagates to all child procs.
    cmd->AppendSwitch("no-sandbox");
    // cmd->AppendSwitch("disable-gpu-sandbox");

    std::string features;
    std::string dis_features;
    if (cmd->HasSwitch("disable-features")) {
        dis_features = cmd->GetSwitchValue("disable-features").ToString();
        if (! dis_features.empty()) dis_features += ",";
        dis_features +=
            "Crashpad,AutofillServerCommunication,HardwareMediaKeyHandling,WebBluetooth,WebUSB";
    }

#if defined(__linux__)
    auto dis_vulkan = [&dis_features, &cmd] {
        if (! dis_features.empty()) dis_features += ",";
        dis_features += "Vulkan,VulkanFromANGLE,DefaultAngleVulkan,SkiaGraphite";
        cmd->AppendSwitch("disable-vulkan-surface");
    };
#endif

#if defined(__linux__)
    auto enable_shared = [this, &cmd] {
        if (! m_shared_texture_enabled) return;
        cmd->AppendSwitch("shared-texture-enabled");
        cmd->AppendSwitch("enable-zero-copy");
    };
#endif

#if defined(__linux__)
    features = "AcceleratedVideoDecodeLinuxZeroCopyGL,AcceleratedVideoDecodeLinuxGL,"
               "VaapiIgnoreDriverChecks,VaapiOnNvidiaGPUs,VaapiVideoDecodeLinuxGL";
    if (0) {
        // Vulkan
        cmd->AppendSwitch("vulkan");
        cmd->AppendSwitchWithValue("use-vulkan", "native");
        cmd->AppendSwitchWithValue("use-angle", "vulkan");
        cmd->AppendSwitchWithValue("use-gl", "angle");
        cmd->AppendSwitch("enable-raw-draw");
        cmd->AppendSwitch("enable-unsafe-webgpu");
        enable_shared();

        features.append(",DefaultAngleVulkan,VulkanFromANGLE,Vulkan,SkiaGraphite");
        cmd->AppendSwitchWithValue("ozone-platform", "x11");
    } else {
        // Gl-Egl
        cmd->AppendSwitchWithValue("use-gl", "angle");
        cmd->AppendSwitchWithValue("use-angle", "gl-egl");

        enable_shared();
        dis_vulkan();

        cmd->AppendSwitchWithValue("ozone-platform", "wayland");
        // cmd->AppendSwitchWithValue("ozone-platform-hint", "wayland");
    }
#else
    // macOS uses Chromium's native Metal and VideoToolbox paths. Forcing the
    // Linux ANGLE/Ozone configuration here disables those platform backends.
    // CEF's windowless shared-texture switch is currently supported only on
    // Windows/D3D11. macOS delivers OSR frames through OnPaint instead.
    // Wallpapers do not persist credentials and should not prompt for
    // access to the user's login keychain.
    cmd->AppendSwitch("use-mock-keychain");
#endif
    if (! m_render_node_override.is_empty()) {
        cmd->AppendSwitchWithValue("render-node-override",
                                   to_string(m_render_node_override.as_str()));
    }

    cmd->AppendSwitch("enable-gpu");
    cmd->AppendSwitch("ignore-gpu-blocklist");
    cmd->AppendSwitch("enable-gpu-rasterization");
    cmd->AppendSwitch("enable-gpu-compositing");
    cmd->AppendSwitch("disable-software-rasterizer");
    cmd->AppendSwitch("off-screen-rendering-enabled");

    // Hardware decode and native GPU memory buffer switches below are Linux
    // backend controls. Chromium selects VideoToolbox on macOS by default.
#if defined(__linux__)
    cmd->AppendSwitch("enable-accelerated-video-decode");
    cmd->AppendSwitch("enable-native-gpu-memory-buffers");
#endif

    // cmd->AppendSwitch("disable-gpu-compositing");
    // cmd->AppendSwitch("disable-gpu-vsync");

#if defined(__linux__)
    cmd->AppendSwitchWithValue("enable-features", features);
    cmd->AppendSwitchWithValue("disable-features", dis_features);
#else
    if (! features.empty()) cmd->AppendSwitchWithValue("enable-features", features);
    if (! dis_features.empty()) cmd->AppendSwitchWithValue("disable-features", dis_features);
#endif

    // Autoplay video / audio without user-gesture prompts. WE wallpapers
    // routinely auto-play media on load.
    cmd->AppendSwitchWithValue("autoplay-policy", "no-user-gesture-required");

    // Skip KDE/GNOME password-store probes (kwalletd6 / libsecret /
    // klauncher D-Bus calls). Wallpapers never store credentials.
    cmd->AppendSwitchWithValue("password-store", "basic");

    // Misc
    cmd->AppendSwitch("no-first-run");
    cmd->AppendSwitch("no-default-browser-check");
    cmd->AppendSwitch("disable-plugins");
    cmd->AppendSwitch("disable-sync");
    cmd->AppendSwitch("disable-translate");
    cmd->AppendSwitch("disable-default-apps");
    cmd->AppendSwitch("disable-extensions");
    cmd->AppendSwitch("disable-client-side-phishing-detection");
    cmd->AppendSwitch("disable-popup-blocking");
    cmd->AppendSwitch("disable-pinch");
    cmd->AppendSwitch("metrics-recording-only");
    cmd->AppendSwitch("disable-component-update");
    cmd->AppendSwitch("disable-session-crashed-bubble");
    cmd->AppendSwitch("disable-search-engine-choice-screen");

    // Honour the host's enable_audio gate. Chromium never instantiates an
    // output stream when this switch is present, so no system audio device
    // (PulseAudio/PipeWire) gets opened.
    if (m_mute_audio) {
        cmd->AppendSwitch("mute-audio");
    }
}

void AppHandler::OnContextInitialized() {
    // Browser is created from BrowserHost::OpenWallpaper; nothing to do here.
}

void AppHandler::OnContextCreated(CefRefPtr<CefBrowser> /*browser*/, CefRefPtr<CefFrame> frame,
                                  CefRefPtr<CefV8Context> context) {
    if (! frame || ! frame->IsMain() || ! context) return;

    const int generation = m_next_audio_context_generation++;
    (void)m_audio_context_generations.insert(reinterpret_cast<rstd::uintptr_t>(context.get()),
                                             int(generation));
    auto handler = CefRefPtr<AudioDemandHandler>(new AudioDemandHandler(frame, generation));
    context->GetGlobal()->SetValue("__weweb_setAudioDemand",
                                   CefV8Value::CreateFunction("__weweb_setAudioDemand", handler),
                                   V8_PROPERTY_ATTRIBUTE_DONTDELETE);
    SendAudioDemand(frame, generation, false);

    // WE web audio-response API. The page calls wallpaperRegisterAudioListener
    // to subscribe; the browser process feeds samples each tick by invoking
    // __weweb_pushAudio with a 128-float array (64 left + 64 right). Values
    // remain linear and may exceed 1 Installed here so it exists before the
    // page's own scripts run.
    static const char kAudioApi[] =
        "(function(){"
        "  if (window.__weweb_audio_installed) return;"
        "  window.__weweb_audio_installed = true;"
        "  var listeners = [];"
        "  window.wallpaperRegisterAudioListener = function(cb){"
        "    if (typeof cb !== 'function' || listeners.indexOf(cb) >= 0) return;"
        "    listeners.push(cb);"
        "    if (listeners.length === 1) window.__weweb_setAudioDemand(true);"
        "  };"
        "  window.wallpaperRemoveAudioListener = function(cb){"
        "    var i = listeners.indexOf(cb); if (i < 0) return;"
        "    listeners.splice(i, 1);"
        "    if (listeners.length === 0) window.__weweb_setAudioDemand(false);"
        "  };"
        "  window.__weweb_pushAudio = function(arr){"
        "    for (var i = 0; i < listeners.length; i++){"
        "      try { listeners[i](arr); } catch (e) {}"
        "    }"
        "  };"
        "})();";
    frame->ExecuteJavaScript(kAudioApi, "weweb://internal/audio_api.js", 0);
}

void AppHandler::OnContextReleased(CefRefPtr<CefBrowser> /*browser*/, CefRefPtr<CefFrame> frame,
                                   CefRefPtr<CefV8Context> context) {
    if (! frame || ! frame->IsMain() || ! context) return;
    auto found =
        m_audio_context_generations.remove(reinterpret_cast<rstd::uintptr_t>(context.get()));
    if (found.is_none()) return;
    SendAudioDemand(frame, *found, false);
}

} // namespace weweb
