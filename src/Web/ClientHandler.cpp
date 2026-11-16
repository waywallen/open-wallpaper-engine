module weweb;

import rstd;
import rstd.cppstd;

import :cef;
import :cef_internal;

using namespace rstd::prelude;
using rstd::cppstd::as_str;
using rstd::io::eprintln;
using rstd::sync::atomic::Ordering;

namespace weweb
{

ClientHandler::ClientHandler(owe::Json user_props, CefRefPtr<OsrRenderHandler> render_handler,
                             bool initially_muted)
    : user_props_(rstd::move(user_props)),
      render_handler_(rstd::move(render_handler)),
      audio_muted_(initially_muted) {}

void ClientHandler::SetAudioDemandCallback(Option<AudioDemandCallback> cb) {
    if (IsClosing()) return;
    audio_demand_cb_ = rstd::move(cb);
    if (auto callback = audio_demand_cb_.clone(); callback) (*callback)->operator()(audio_demand_);
}

void ClientHandler::SetMediaReplayCallback(Option<MediaReplayCallback> cb) {
    if (IsClosing()) return;
    media_replay_cb_ = rstd::move(cb);
    // A snapshot may already be cached before the browser even exists; the
    // load-end hook replays it once the page is ready.
}

void ClientHandler::SetAudioMuted(bool muted) {
    audio_muted_ = muted;
    if (auto browser = GetBrowser()) browser->GetHost()->SetAudioMuted(muted);
}

bool ClientHandler::IsClosing() const {
    return state_ == State::Closing || state_ == State::Closed;
}

bool ClientHandler::IsClosed() const { return state_ == State::Closed; }

CefRefPtr<CefBrowser> ClientHandler::GetBrowser() const { return IsClosing() ? nullptr : browser_; }

void ClientHandler::RequestClose() {
    if (IsClosing()) return;
    state_ = State::Closing;
    render_handler_->SetAcceleratedPaintCallback(None());
    render_handler_->SetCpuPaintCallback(None());
    audio_demand_ = false;
    if (auto callback = audio_demand_cb_.take()) (*callback)->operator()(false);

    pending_close_.reserve(browsers_.len());
    for (const auto& browser : browsers_) pending_close_.push(CefRefPtr<CefBrowser>(browser));
    FinishClose();
}

void ClientHandler::CloseBrowsers() {
    // Do not destroy a newly created browser from its OnAfterCreated callback.
    auto closing = rstd::move(pending_close_);
    for (const auto& browser : closing)
        if (browser->IsValid()) browser->GetHost()->CloseBrowser(true);
}

void ClientHandler::FinishClose() {
    if (state_ == State::Closing && ! creation_pending_ && pending_popups_ == usize() &&
        browsers_.is_empty())
        state_ = State::Closed;
}

void ClientHandler::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
    if (browser->IsPopup()) {
        if (pending_popups_ > usize()) --pending_popups_;
    } else {
        creation_pending_ = false;
        browser_          = browser;
    }
    browsers_.push(CefRefPtr<CefBrowser>(browser));
    if (IsClosing()) {
        pending_close_.push(CefRefPtr<CefBrowser>(browser));
        return;
    }
    state_ = State::Open;
    browser->GetHost()->SetAudioMuted(audio_muted_);
}

bool ClientHandler::OnBeforePopup(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, int, const CefString&,
                                  const CefString&, cef_window_open_disposition_t, bool,
                                  const CefPopupFeatures&, CefWindowInfo&, CefRefPtr<CefClient>&,
                                  CefBrowserSettings&, CefRefPtr<CefDictionaryValue>&, bool*) {
    if (IsClosing()) return true;
    ++pending_popups_;
    return false;
}

void ClientHandler::OnBeforePopupAborted(CefRefPtr<CefBrowser>, int) {
    if (pending_popups_ > usize()) --pending_popups_;
    FinishClose();
}

bool ClientHandler::DoClose(CefRefPtr<CefBrowser> /*browser*/) {
    return false; // proceed with the default close
}

void ClientHandler::OnBeforeClose(CefRefPtr<CefBrowser> browser) {
    for (usize index {}; index < browsers_.len(); ++index) {
        if (browsers_[index]->IsSame(browser)) {
            (void)browsers_.remove(index);
            break;
        }
    }
    if (browser_ && browser_->IsSame(browser)) {
        browser_ = nullptr;
        RequestClose();
    }
    FinishClose();
}

bool ClientHandler::OnProcessMessageReceived(CefRefPtr<CefBrowser> /*browser*/,
                                             CefRefPtr<CefFrame> /*frame*/,
                                             CefProcessId                 source_process,
                                             CefRefPtr<CefProcessMessage> message) {
    if (source_process != PID_RENDERER || ! message || message->GetName() != "weweb.audio-demand")
        return false;
    if (IsClosing()) return true;
    auto args = message->GetArgumentList();
    if (! args || args->GetSize() < 2) return true;
    const int generation = args->GetInt(0);
    if (generation < audio_context_generation_) return true;
    if (generation > audio_context_generation_) {
        audio_context_generation_ = generation;
        if (audio_demand_) {
            audio_demand_ = false;
            if (auto callback = audio_demand_cb_.clone(); callback) (*callback)->operator()(false);
        }
    }
    const bool active = args->GetBool(1);
    if (active != audio_demand_) {
        audio_demand_ = active;
        if (auto callback = audio_demand_cb_.clone(); callback) (*callback)->operator()(active);
    }
    return true;
}

void ClientHandler::OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                              int /*httpStatusCode*/) {
    if (IsClosing() || ! frame || ! frame->IsMain() || ! browser_ || ! browser_->IsSame(browser))
        return;
    // MPRIS snapshots are unsolicited, so one can arrive while the page is
    // still loading (the DOM is not ready, listeners not registered yet).
    // Replaying the cached snapshot here matches InjectUserProperties below.
    if (auto callback = media_replay_cb_.clone(); callback) (*callback)->operator()();
    bool expected = false;
    if (! property_injected_.compare_exchange_strong(
            expected, true, Ordering::SeqCst, Ordering::SeqCst))
        return;
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
    eprintln("weweb [{}] {} ({}:{})",
             as_str(level_str).unwrap(),
             as_str(message.ToString()).unwrap(),
             as_str(source.ToString()).unwrap(),
             line);
    return false; // also let CEF's default handler log
}

} // namespace weweb
