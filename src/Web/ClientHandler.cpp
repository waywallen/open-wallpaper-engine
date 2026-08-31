module;

#include <cstdio>

module weweb;

import rstd.cppstd;

import :cef;
import :cef_internal;

namespace weweb
{

ClientHandler::ClientHandler(owe::Json user_props, CefRefPtr<OsrRenderHandler> render_handler,
                             bool initially_muted)
    : user_props_(std::move(user_props)),
      render_handler_(std::move(render_handler)),
      audio_muted_(initially_muted) {}

void ClientHandler::SetCloseCallback(std::function<void()> cb) { close_cb_ = std::move(cb); }

void ClientHandler::SetAudioDemandCallback(std::function<void(bool)> cb) {
    audio_demand_cb_ = std::move(cb);
    if (audio_demand_cb_) audio_demand_cb_(audio_demand_);
}

void ClientHandler::SetAudioMuted(bool muted) {
    audio_muted_ = muted;
    if (browser_ && browser_->GetHost()) browser_->GetHost()->SetAudioMuted(muted);
}

void ClientHandler::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
    browser_ = browser;
    if (browser_ && browser_->GetHost()) {
        browser_->GetHost()->SetAudioMuted(audio_muted_);
    }
}

bool ClientHandler::DoClose(CefRefPtr<CefBrowser> /*browser*/) {
    return false; // proceed with the default close
}

void ClientHandler::OnBeforeClose(CefRefPtr<CefBrowser> /*browser*/) {
    if (audio_demand_) {
        audio_demand_ = false;
        if (audio_demand_cb_) audio_demand_cb_(false);
    }
    browser_ = nullptr;
    if (close_cb_) close_cb_();
}

bool ClientHandler::OnProcessMessageReceived(CefRefPtr<CefBrowser> /*browser*/,
                                             CefRefPtr<CefFrame> /*frame*/,
                                             CefProcessId                 source_process,
                                             CefRefPtr<CefProcessMessage> message) {
    if (source_process != PID_RENDERER || ! message || message->GetName() != "weweb.audio-demand")
        return false;
    auto args = message->GetArgumentList();
    if (! args || args->GetSize() < 2) return true;
    const int generation = args->GetInt(0);
    if (generation < audio_context_generation_) return true;
    if (generation > audio_context_generation_) {
        audio_context_generation_ = generation;
        if (audio_demand_) {
            audio_demand_ = false;
            if (audio_demand_cb_) audio_demand_cb_(false);
        }
    }
    const bool active = args->GetBool(1);
    if (active != audio_demand_) {
        audio_demand_ = active;
        if (audio_demand_cb_) audio_demand_cb_(active);
    }
    return true;
}

void ClientHandler::OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                              int /*httpStatusCode*/) {
    if (! frame || ! frame->IsMain()) return;
    bool expected = false;
    if (! property_injected_.compare_exchange_strong(expected, true)) return;
    InjectUserProperties(browser, user_props_);
}

bool ClientHandler::OnConsoleMessage(CefRefPtr<CefBrowser> /*browser*/, cef_log_severity_t level,
                                     const CefString& message, const CefString& source, int line) {
    const char* level_str = "info";
    // LOGSEVERITY_VERBOSE and LOGSEVERITY_DEBUG share the same numeric
    // value in CEF; only one needs a case label.
    switch (level) {
    case LOGSEVERITY_DEBUG: level_str = "debug"; break;
    case LOGSEVERITY_INFO: level_str = "info"; break;
    case LOGSEVERITY_WARNING: level_str = "warn"; break;
    case LOGSEVERITY_ERROR: level_str = "error"; break;
    case LOGSEVERITY_FATAL: level_str = "fatal"; break;
    default: break;
    }
    std::fprintf(stderr,
                 "weweb [%s] %s (%s:%d)\n",
                 level_str,
                 message.ToString().c_str(),
                 source.ToString().c_str(),
                 line);
    return false; // also let CEF's default handler log
}

} // namespace weweb
