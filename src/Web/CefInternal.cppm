module weweb:cef_internal;

import rstd.cppstd;
import wescene.json;

import :cef;
import :frame;

namespace weweb
{

class AppHandler : public CefApp, public CefBrowserProcessHandler, public CefRenderProcessHandler {
public:
    AppHandler();

    AppHandler(const AppHandler&)            = delete;
    AppHandler& operator=(const AppHandler&) = delete;

    void SetMuteAudio(bool m) { m_mute_audio = m; }
    void SetSharedTextureEnabled(bool enabled) { m_shared_texture_enabled = enabled; }
    void SetRenderNodeOverride(const std::string& path) { m_render_node_override = path; }

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
    bool                                 m_mute_audio { false };
    bool                                 m_shared_texture_enabled { true };
    std::string                          m_render_node_override;
    int                                  m_next_audio_context_generation { 1 };
    std::unordered_map<const void*, int> m_audio_context_generations;
    CefRefCount                          ref_count_;
};

class OsrRenderHandler : public CefRenderHandler {
public:
    OsrRenderHandler() = default;

    OsrRenderHandler(const OsrRenderHandler&)            = delete;
    OsrRenderHandler& operator=(const OsrRenderHandler&) = delete;

    void SetViewSize(int width, int height);
    void SetAcceleratedPaintCallback(AcceleratedPaintCallback cb) { accel_cb_ = std::move(cb); }
    void SetCpuPaintCallback(CpuPaintCallback cb) { cpu_cb_ = std::move(cb); }

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
    std::mutex               mu_;
    int                      view_w_ { 1280 };
    int                      view_h_ { 720 };
    AcceleratedPaintCallback accel_cb_;
    CpuPaintCallback         cpu_cb_;
    CefRefCount              ref_count_;
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

    void                  SetCloseCallback(std::function<void()> cb);
    void                  SetAudioDemandCallback(std::function<void(bool)> cb);
    void                  SetAudioMuted(bool muted);
    CefRefPtr<CefBrowser> GetBrowser() const { return browser_; }

    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefLoadHandler>     GetLoadHandler() override { return this; }
    CefRefPtr<CefDisplayHandler>  GetDisplayHandler() override { return this; }
    CefRefPtr<CefRenderHandler>   GetRenderHandler() override { return render_handler_; }

    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
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
    owe::Json                   user_props_;
    CefRefPtr<OsrRenderHandler> render_handler_;
    CefRefPtr<CefBrowser>       browser_;
    std::function<void()>       close_cb_;
    std::function<void(bool)>   audio_demand_cb_;
    int                         audio_context_generation_ { 0 };
    bool                        audio_demand_ { false };
    bool                        audio_muted_ { false };
    std::atomic<bool>           property_injected_ { false };
    CefRefCount                 ref_count_;
};

std::string BuildPropertyListenerSnippet(const owe::Json& props);
void        InjectUserProperties(CefRefPtr<CefBrowser> browser, const owe::Json& props);

} // namespace weweb
