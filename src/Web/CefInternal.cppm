module weweb:cef_internal;

import rstd;

import wescene.json;

import :cef;
import :frame;

using namespace rstd::prelude;
using rstd::collections::HashMap;
using rstd::sync::Mutex;
using rstd::sync::atomic::Atomic;

namespace weweb
{

class AppHandler : public CefApp, public CefBrowserProcessHandler, public CefRenderProcessHandler {
public:
    AppHandler();

    AppHandler(const AppHandler&)            = delete;
    AppHandler& operator=(const AppHandler&) = delete;

    void SetMuteAudio(bool m) { m_mute_audio = m; }
    void SetSharedTextureEnabled(bool enabled) { m_shared_texture_enabled = enabled; }
    void SetRenderNodeOverride(ref<str> path) { m_render_node_override = String::make(path); }

    CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }
    CefRefPtr<CefRenderProcessHandler>  GetRenderProcessHandler() override { return this; }
    void OnBeforeCommandLineProcessing(const CefString&          process_type,
                                       CefRefPtr<CefCommandLine> cmd) override;

    void OnContextInitialized() override;
    void OnContextCreated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                          CefRefPtr<CefV8Context> context) override;
    void OnContextReleased(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                           CefRefPtr<CefV8Context> context) override;

    void AddRef() const override { ref_count_.AddRef(); }
    bool Release() const override {
        if (ref_count_.Release()) {
            delete this;
            return true;
        }
        return false;
    }
    bool HasOneRef() const override { return ref_count_.HasOneRef(); }
    bool HasAtLeastOneRef() const override { return ref_count_.HasAtLeastOneRef(); }

private:
    bool                          m_mute_audio { false };
    bool                          m_shared_texture_enabled { true };
    String                        m_render_node_override;
    int                           m_next_audio_context_generation { 1 };
    HashMap<rstd::uintptr_t, int> m_audio_context_generations;
    CefRefCount                   ref_count_;
};

class OsrRenderHandler : public CefRenderHandler {
public:
    OsrRenderHandler() = default;

    OsrRenderHandler(const OsrRenderHandler&)            = delete;
    OsrRenderHandler& operator=(const OsrRenderHandler&) = delete;

    void SetViewSize(int width, int height);
    void SetDeviceScaleFactor(float scale);
    void SetAcceleratedPaintCallback(Option<AcceleratedPaintCallback> cb);
    void SetCpuPaintCallback(Option<CpuPaintCallback> cb);

    void GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect) override;
    void OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type, const RectList& dirtyRects,
                 const void* buffer, int width, int height) override;
    void OnAcceleratedPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
                            const RectList&                dirtyRects,
                            const CefAcceleratedPaintInfo& info) override;
    bool GetScreenInfo(CefRefPtr<CefBrowser> browser, CefScreenInfo& info) override;

    void AddRef() const override { ref_count_.AddRef(); }
    bool Release() const override {
        if (ref_count_.Release()) {
            delete this;
            return true;
        }
        return false;
    }
    bool HasOneRef() const override { return ref_count_.HasOneRef(); }
    bool HasAtLeastOneRef() const override { return ref_count_.HasAtLeastOneRef(); }

private:
    struct State {
        int                              view_w { 1280 };
        int                              view_h { 720 };
        float                            device_scale_factor { 1.0f };
        Option<AcceleratedPaintCallback> accel_cb;
        Option<CpuPaintCallback>         cpu_cb;
    };
    Mutex<State> state_;
    CefRefCount  ref_count_;
};

class ClientHandler : public CefClient,
                      public CefLifeSpanHandler,
                      public CefLoadHandler,
                      public CefDisplayHandler {
public:
    explicit ClientHandler(owe::Json user_props, CefRefPtr<OsrRenderHandler> render_handler,
                           bool initially_muted);

    ClientHandler(const ClientHandler&)            = delete;
    ClientHandler& operator=(const ClientHandler&) = delete;

    void                  SetAudioDemandCallback(Option<AudioDemandCallback> cb);
    void                  SetMediaReplayCallback(Option<MediaReplayCallback> cb);
    void                  SetAudioMuted(bool muted);
    void                  RequestClose();
    void                  CloseBrowsers();
    bool                  IsClosing() const;
    bool                  IsClosed() const;
    CefRefPtr<CefBrowser> GetBrowser() const;

    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefLoadHandler>     GetLoadHandler() override { return this; }
    CefRefPtr<CefDisplayHandler>  GetDisplayHandler() override { return this; }
    CefRefPtr<CefRenderHandler>   GetRenderHandler() override { return render_handler_; }

    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
    bool OnBeforePopup(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int popup_id,
                       const CefString& target_url, const CefString& target_frame_name,
                       cef_window_open_disposition_t target_disposition, bool user_gesture,
                       const CefPopupFeatures& popup_features, CefWindowInfo& window_info,
                       CefRefPtr<CefClient>& client, CefBrowserSettings& settings,
                       CefRefPtr<CefDictionaryValue>& extra_info,
                       bool*                          no_javascript_access) override;
    void OnBeforePopupAborted(CefRefPtr<CefBrowser> browser, int popup_id) override;
    bool DoClose(CefRefPtr<CefBrowser> browser) override;
    void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;

    void OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                   int httpStatusCode) override;

    bool OnConsoleMessage(CefRefPtr<CefBrowser> browser, cef_log_severity_t level,
                          const CefString& message, const CefString& source, int line) override;
    bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                                  CefProcessId                 source_process,
                                  CefRefPtr<CefProcessMessage> message) override;

    void AddRef() const override { ref_count_.AddRef(); }
    bool Release() const override {
        if (ref_count_.Release()) {
            delete this;
            return true;
        }
        return false;
    }
    bool HasOneRef() const override { return ref_count_.HasOneRef(); }
    bool HasAtLeastOneRef() const override { return ref_count_.HasAtLeastOneRef(); }

private:
    enum class State
    {
        Creating,
        Open,
        Closing,
        Closed
    };

    void FinishClose();

    owe::Json                   user_props_;
    CefRefPtr<OsrRenderHandler> render_handler_;
    CefRefPtr<CefBrowser>       browser_;
    Vec<CefRefPtr<CefBrowser>>  browsers_;
    Vec<CefRefPtr<CefBrowser>>  pending_close_;
    State                       state_ { State::Creating };
    bool                        creation_pending_ { true };
    usize                       pending_popups_ {};
    Option<AudioDemandCallback> audio_demand_cb_;
    Option<MediaReplayCallback> media_replay_cb_;
    int                         audio_context_generation_ { 0 };
    bool                        audio_demand_ { false };
    bool                        audio_muted_ { false };
    Atomic<bool>                property_injected_ { false };
    CefRefCount                 ref_count_;
};

String BuildPropertyListenerSnippet(const owe::Json& props);
String BuildPropertyPatchSnippet(ref<str> key, const owe::Json& value);
String BuildAudioResponseSnippet(slice<float> data);
String BuildMediaSnippet(const MediaInfo& media);
void   InjectUserProperties(CefRefPtr<CefBrowser> browser, const owe::Json& props);

} // namespace weweb
