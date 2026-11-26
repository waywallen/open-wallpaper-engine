module;

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>

#define HELPER_REFCOUNT()                                                       \
public:                                                                         \
    void AddRef() const override { refs_.AddRef(); }                            \
    bool Release() const override {                                             \
        if (! refs_.Release()) return false;                                    \
        delete this;                                                            \
        return true;                                                            \
    }                                                                           \
    bool HasOneRef() const override { return refs_.HasOneRef(); }               \
    bool HasAtLeastOneRef() const override { return refs_.HasAtLeastOneRef(); } \
                                                                                \
private:                                                                        \
    mutable CefRefCount refs_

module weweb;

import rstd;
import rstd.cppstd;
import rstd.json;
import :browser_helper;
import :cef;

using namespace rstd::prelude;
using namespace rstd::literals;

namespace weweb
{
namespace
{
using rstd::json::Value;
constexpr usize       max_frame    = usize(1024 * 1024);
constexpr const char* config_key   = "waywallen.login.config";
constexpr const char* message_key  = "waywallen.login.message";
constexpr const char* evaluate_key = "waywallen.login.evaluate";

auto text(const Value& value, ref<str> key) -> ref<str> {
    return value[key].as_str().unwrap_or(""_str);
}
auto json_string(ref<str> value) -> Value { return Value::String(String::make(value)); }
auto number(u64 value) -> Value { return Value::Number(rstd::json::Number::from_u64(value)); }
auto cef_string(ref<str> value) -> CefString { return rstd::cppstd::to_string(value); }
auto owned_string(const CefString& value) -> String {
    const auto bytes = value.ToString();
    return String::make(rstd::cppstd::as_str(bytes).unwrap_or(""_str));
}
bool valid_text(ref<str> value, usize limit) {
    return value.len() <= limit && ! value.contains("\0"_str);
}
bool output(const Value& value) {
    auto bytes = rstd::json::to_string(value);
    if (bytes.len() > max_frame) return false;
    bytes.push_ascii('\n');
    auto stream = rstd::io::stdout();
    return rstd::io::write_all(stream, bytes.as_str().as_bytes()).is_ok();
}
auto event(ref<str> kind) -> Value {
    Value result;
    result["version"_str] = number(u64(1));
    result["event"_str]   = json_string(kind);
    return result;
}
void error(ref<str> code, ref<str> message) {
    auto result           = event("error"_str);
    result["code"_str]    = json_string(code);
    result["message"_str] = json_string(message);
    output(result);
}

auto uri_scheme(ref<str> value) -> Option<String> {
    if (! valid_text(value, usize(16384))) return None();
    auto separator = value.find(":"_str);
    if (! separator || *separator == usize()) return None();
    auto prefix = value.get(usize(), *separator).unwrap();
    if (! value.get(*separator + usize(1), value.len()).unwrap().starts_with("//"_str))
        return None();
    for (usize i {}; i < prefix.len(); ++i) {
        auto c = prefix[i].to_primitive();
        if (! ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (i != usize() && ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.'))))
            return None();
    }
    auto scheme = String::make(prefix);
    scheme->make_ascii_lowercase();
    return Some(rstd::move(scheme));
}
bool builtin_scheme(ref<str> scheme) {
    for (auto name : { "http"_str,
                       "https"_str,
                       "file"_str,
                       "ftp"_str,
                       "about"_str,
                       "data"_str,
                       "javascript"_str,
                       "ws"_str,
                       "wss"_str,
                       "chrome"_str,
                       "chrome-extension"_str,
                       "devtools"_str,
                       "blob"_str,
                       "filesystem"_str })
        if (scheme == name) return true;
    return false;
}

struct Uri {
    String scheme, host, path, port;
    bool   valid {}, has_query {}, has_fragment {};

    explicit Uri(ref<str> value) {
        if (! valid_text(value, usize(16384))) return;
        for (auto byte : value.as_bytes())
            if (byte < u8(33) || byte == u8(127)) return;
        auto parsed_scheme = uri_scheme(value);
        if (! parsed_scheme) return;
        scheme = parsed_scheme.take().unwrap();
        CefURLParts parts;
        if (! CefParseURL(cef_string(value), parts)) return;
        if (CefString(&parts.username).length() || CefString(&parts.password).length()) return;
        host = owned_string(CefString(&parts.host));
        if (host.is_empty()) return;
        host->make_ascii_lowercase();
        path = owned_string(CefString(&parts.path));
        port = owned_string(CefString(&parts.port));
        if (port.is_empty() && scheme == "https"_str) port = String::make("443"_str);
        if (port.is_empty() && scheme == "http"_str) port = String::make("80"_str);
        if (path.is_empty()) path = String::make("/"_str);
        has_query    = value.contains("?"_str);
        has_fragment = value.contains("#"_str);
        valid        = true;
    }
    bool http_scheme() const { return scheme == "http"_str || scheme == "https"_str; }
    bool http() const { return valid && http_scheme(); }
    bool origin() const { return http() && path == "/"_str && ! has_query && ! has_fragment; }
    bool same_origin(const Uri& other) const {
        return valid && other.valid && scheme == other.scheme && host == other.host &&
               port == other.port;
    }
    bool same_target(const Uri& other) const { return same_origin(other) && path == other.path; }
    auto origin_string() const -> String {
        bool standard = (scheme == "http"_str && port == "80"_str) ||
                        (scheme == "https"_str && port == "443"_str) || port.is_empty();
        return standard ? rstd::format("{}://{}", scheme, host)
                        : rstd::format("{}://{}:{}", scheme, host, port);
    }
};

bool array_valid(const Value& value, usize limit) {
    return value.is_Null() || (value.as_array() && (**value.as_array()).len() <= limit);
}
bool names_valid(const Value& names) {
    if (! array_valid(names, usize(64))) return false;
    if (auto values = names.as_array())
        for (const auto& item : **values) {
            auto name = item.as_str();
            if (! name || name->is_empty() || ! valid_text(*name, usize(256))) return false;
            for (auto byte : name->as_bytes())
                if (byte < u8(32) || byte == u8(127)) return false;
        }
    return true;
}
bool wanted(const Value& names, ref<str> name) {
    if (! names.as_array() || (**names.as_array()).is_empty()) return true;
    for (const auto& item : **names.as_array())
        if (item.as_str() == Some(name)) return true;
    return false;
}
bool open_valid(const Value& request) {
    if (! Uri(text(request, "url"_str)).http() ||
        ! valid_text(text(request, "title"_str), usize(4096)) ||
        ! array_valid(request["redirects"_str], usize(32)) ||
        ! array_valid(request["cookie_requests"_str], usize(32)) ||
        ! array_valid(request["scripts"_str], usize(16)))
        return false;
    if (auto redirects = request["redirects"_str].as_array())
        for (const auto& value : **redirects) {
            if (! value.as_str()) return false;
            auto scheme = uri_scheme(*value.as_str());
            if (! scheme || *scheme == "file"_str || *scheme == "javascript"_str ||
                *scheme == "data"_str)
                return false;
        }
    if (auto requests = request["cookie_requests"_str].as_array())
        for (const auto& value : **requests)
            if (! Uri(text(value, "uri"_str)).http() || ! names_valid(value["names"_str]))
                return false;
    if (auto scripts = request["scripts"_str].as_array())
        for (const auto& value : **scripts) {
            auto origins = value["origins"_str].as_array();
            if (! value["source"_str].as_str() ||
                ! valid_text(text(value, "source"_str), usize(256 * 1024)) || ! origins ||
                (**origins).is_empty() || (**origins).len() > usize(32))
                return false;
            for (const auto& origin : **origins)
                if (! origin.as_str() || ! Uri(*origin.as_str()).origin()) return false;
        }
    return true;
}
bool script_allowed(const Value& request, const Uri& uri) {
    if (auto scripts = request["scripts"_str].as_array())
        for (const auto& script : **scripts)
            for (const auto& origin : **script["origins"_str].as_array())
                if (uri.same_origin(Uri(*origin.as_str()))) return true;
    return false;
}
void send(CefRefPtr<CefFrame> frame, ref<str> name, const Value& value, CefProcessId target) {
    auto message = CefProcessMessage::Create(cef_string(name));
    message->GetArgumentList()->SetString(0, cef_string(rstd::json::to_string(value)));
    frame->SendProcessMessage(target, message);
}

class ScriptBinding final : public CefV8Handler {
public:
    ScriptBinding(String origin, String token)
        : origin_(rstd::move(origin)), token_(rstd::move(token)) {}
    bool Execute(const CefString&, CefRefPtr<CefV8Value>, const CefV8ValueList& arguments,
                 CefRefPtr<CefV8Value>&, CefString& exception) override {
        auto context = CefV8Context::GetCurrentContext();
        auto frame   = context ? context->GetFrame() : nullptr;
        if (! frame || ! frame->IsMain() ||
            ! Uri(owned_string(frame->GetURL())).same_origin(Uri(origin_)) ||
            arguments.size() != 1 || ! arguments[0]->IsString())
            return false;
        auto bytes = owned_string(arguments[0]->GetStringValue());
        if (bytes.len() > max_frame) {
            Value failure;
            failure["kind"_str]   = json_string("script_message_error"_str);
            failure["origin"_str] = json_string(origin_);
            failure["token"_str]  = json_string(token_);
            send(frame, "waywallen.login.message"_str, failure, PID_BROWSER);
            exception = "Browser script message exceeds protocol limit";
            return true;
        }
        auto parsed = rstd::json::from_str(bytes);
        if (parsed.is_err()) return false;
        Value value;
        value["kind"_str]   = json_string("script_message"_str);
        value["origin"_str] = json_string(origin_);
        value["token"_str]  = json_string(token_);
        value["value"_str]  = rstd::move(parsed).unwrap();
        send(frame, "waywallen.login.message"_str, value, PID_BROWSER);
        return true;
    }

private:
    String origin_, token_;
    HELPER_REFCOUNT();
};

struct RenderBrowser {
    int   id;
    Value request;
};
struct RenderDocument {
    int    browser;
    String frame, token;
};

#if __is_target_os(linux)
auto ozone_platform() -> ref<str> {
    if (auto backend = rstd::env::var("OWE_WINDOW_BACKEND"_str); backend.is_ok())
        for (auto name : { "x11"_str, "wayland"_str, "headless"_str })
            if (backend->as_str() == name) return name;

    auto display = rstd::env::var_os("WAYLAND_DISPLAY"_str);
    if (! display || display->is_empty()) return "x11"_str;
    auto socket = rstd::path::PathBuf::from(rstd::move(*display));
    if (! socket.as_path().is_absolute()) {
        auto runtime = rstd::env::var_os("XDG_RUNTIME_DIR"_str);
        if (! runtime || runtime->is_empty()) return "x11"_str;
        auto directory = rstd::path::PathBuf::from(rstd::move(*runtime));
        if (! directory.as_path().is_absolute()) return "x11"_str;
        socket = directory.join(socket.as_path());
    }
    auto metadata = rstd::fs::metadata(socket.as_path());
    return metadata.is_ok() && metadata->file_type().is_socket() ? "wayland"_str : "x11"_str;
}
#endif

class HelperApp final : public CefApp,
                        public CefBrowserProcessHandler,
                        public CefRenderProcessHandler {
public:
    Vec<String> schemes;
    bool        registration_succeeded { true };
    void        OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) override {
        for (const auto& scheme : schemes)
            if (! registrar->AddCustomScheme(cef_string(scheme), CEF_SCHEME_OPTION_STANDARD))
                registration_succeeded = false;
    }
    CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }
    CefRefPtr<CefRenderProcessHandler>  GetRenderProcessHandler() override { return this; }
    void OnBeforeChildProcessLaunch(CefRefPtr<CefCommandLine> command) override {
        command->AppendSwitch("web-helper");
        String names;
        for (const auto& scheme : schemes) {
            if (! names.is_empty()) names.push_ascii(',');
            names.push_str(scheme);
        }
        if (! names.is_empty())
            command->AppendSwitchWithValue("web-helper-schemes", cef_string(names));
    }
    void OnBeforeCommandLineProcessing(const CefString&          process_type,
                                       CefRefPtr<CefCommandLine> command) override {
        if (! process_type.empty()) return;
        command->AppendSwitch("no-sandbox");
        command->AppendSwitch("disable-logging");
        command->AppendSwitch("disable-breakpad");
        command->AppendSwitch("disable-crash-reporter");
        command->AppendSwitch("disable-component-update");
        command->AppendSwitchWithValue("password-store", "basic");
        command->AppendSwitchWithValue("disable-features", "AutofillServerCommunication,Crashpad");
#if __is_target_os(linux)
        command->AppendSwitchWithValue("ozone-platform", cef_string(ozone_platform()));
#endif
    }
    void OnBrowserCreated(CefRefPtr<CefBrowser>         browser,
                          CefRefPtr<CefDictionaryValue> extra) override {
        if (! extra || ! extra->HasKey(config_key)) return;
        auto parsed = rstd::json::from_str(owned_string(extra->GetString(config_key)));
        if (parsed.is_ok())
            browsers_.push(RenderBrowser { browser->GetIdentifier(), rstd::move(parsed).unwrap() });
    }
    void OnBrowserDestroyed(CefRefPtr<CefBrowser> browser) override {
        const int id = browser->GetIdentifier();
        browsers_.retain([id](const auto& item) {
            return item.id != id;
        });
        documents_.retain([id](const auto& item) {
            return item.browser != id;
        });
    }
    void OnContextCreated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                          CefRefPtr<CefV8Context> context) override {
        if (! frame->IsMain()) return;
        const Value* request = nullptr;
        for (const auto& item : browsers_)
            if (item.id == browser->GetIdentifier()) request = &item.request;
        if (! request) return;
        Uri  uri(owned_string(frame->GetURL()));
        auto token = rstd::format("{}:{}", i64(::getpid()), u64(++serial_));
        documents_.push(RenderDocument {
            browser->GetIdentifier(), owned_string(frame->GetIdentifier()), token.clone() });
        Value document;
        document["kind"_str]  = json_string("context"_str);
        document["token"_str] = json_string(token);
        send(frame, "waywallen.login.message"_str, document, PID_BROWSER);
        if (! script_allowed(*request, uri)) return;
        auto native = CefV8Value::CreateFunction(
            "postMessage", new ScriptBinding(uri.origin_string(), token.clone()));
        auto api = CefV8Value::CreateObject(nullptr, nullptr);
        api->SetValue("send", native, V8_PROPERTY_ATTRIBUTE_DONTDELETE);
        context->GetGlobal()->SetValue("__waywallenLoginNative", api, V8_PROPERTY_ATTRIBUTE_NONE);
        String code = String::make(
            "(() => { const send = globalThis.__waywallenLoginNative.send; "
            "delete globalThis.__waywallenLoginNative; "
            "Object.defineProperty(globalThis, 'waywallen', { value: Object.freeze({ "
            "postMessage(value) { send(JSON.stringify(value)); } }), configurable: false }); "
            "const run = () => {\n"_str);
        for (const auto& script : **(*request)["scripts"_str].as_array()) {
            bool allowed = false;
            for (const auto& origin : **script["origins"_str].as_array())
                if (uri.same_origin(Uri(*origin.as_str()))) allowed = true;
            if (allowed) {
                code.push_str("(() => {\n"_str);
                code.push_str(text(script, "source"_str));
                code.push_str("\n})();\n"_str);
            }
        }
        code.push_str(
            "}; if (document.readyState === 'loading') "
            "document.addEventListener('DOMContentLoaded', run, { once: true }); else run(); })();"_str);
        CefRefPtr<CefV8Value>     result;
        CefRefPtr<CefV8Exception> exception;
        context->Eval(cef_string(code), "waywallen://login/scripts", 1, result, exception);
    }
    void OnContextReleased(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                           CefRefPtr<CefV8Context>) override {
        auto id = owned_string(frame->GetIdentifier());
        documents_.retain([&](const auto& item) {
            return item.browser != browser->GetIdentifier() || item.frame != id;
        });
    }
    bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                                  CefProcessId                 source,
                                  CefRefPtr<CefProcessMessage> message) override {
        if (source != PID_BROWSER || message->GetName() != evaluate_key || ! frame->IsMain())
            return false;
        auto parsed = rstd::json::from_str(owned_string(message->GetArgumentList()->GetString(0)));
        if (parsed.is_err()) return true;
        const auto& request = *parsed;
        auto        token   = text(request, "token"_str);
        bool        current = false;
        auto        id      = owned_string(frame->GetIdentifier());
        for (const auto& item : documents_)
            if (item.browser == browser->GetIdentifier() && item.frame == id && item.token == token)
                current = true;
        Value result;
        result["kind"_str]   = json_string("javascript"_str);
        result["id"_str]     = request["id"_str].clone();
        result["origin"_str] = request["origin"_str].clone();
        result["token"_str]  = request["token"_str].clone();
        auto context         = frame->GetV8Context();
        if (! current || ! context ||
            ! Uri(owned_string(frame->GetURL())).same_origin(Uri(text(request, "origin"_str))))
            result["error"_str] = json_string("navigation_changed"_str);
        else {
            context->Enter();
            auto expression = rstd::format("JSON.stringify((0, eval)({}))",
                                           rstd::json::to_string(request["source"_str]));
            CefRefPtr<CefV8Value>     value;
            CefRefPtr<CefV8Exception> exception;
            bool                      success = context->Eval(
                cef_string(expression), "waywallen://login/evaluate", 1, value, exception);
            if (! success)
                result["error"_str] = json_string("javascript"_str);
            else if (! value || value->IsUndefined())
                result["value"_str] = Value::Null();
            else if (auto bytes = owned_string(value->GetStringValue()); bytes.len() <= max_frame) {
                auto json = rstd::json::from_str(bytes);
                if (json.is_ok())
                    result["value"_str] = rstd::move(json).unwrap();
                else
                    result["error"_str] = json_string("javascript"_str);
            } else
                result["error"_str] = json_string("output_limit"_str);
            context->Exit();
        }
        send(frame, "waywallen.login.message"_str, result, PID_BROWSER);
        return true;
    }

private:
    Vec<RenderBrowser>  browsers_;
    Vec<RenderDocument> documents_;
    unsigned long long  serial_ {};
    HELPER_REFCOUNT();
};

class Session;
struct Browser {
    CefRefPtr<CefBrowser> value;
    String                token;
    bool                  close_requested {};
};
struct Evaluation {
    Value  id;
    int    browser;
    String frame, origin, token;
};
struct CookieSnapshot {
    String selector, encoded;
};

class CookieResult final : public CefTask {
public:
    CookieResult(CefRefPtr<Session> session, Value request, bool automatic)
        : session_(session), request_(rstd::move(request)), automatic_(automatic) {}
    void       Execute() override;
    Vec<Value> cookies;

private:
    CefRefPtr<Session> session_;
    Value              request_;
    bool               automatic_;
    HELPER_REFCOUNT();
};
class Cookies final : public CefCookieVisitor {
public:
    Cookies(CefRefPtr<CookieResult> result, Value names)
        : result_(result), names_(rstd::move(names)) {}
    ~Cookies() override { CefPostTask(TID_UI, result_); }
    bool Visit(const CefCookie& cookie, int, int, bool&) override {
        auto name = owned_string(CefString(&cookie.name));
        if (! wanted(names_, name)) return true;
        Value value;
        value["name"_str]      = json_string(name);
        value["value"_str]     = json_string(owned_string(CefString(&cookie.value)));
        value["domain"_str]    = json_string(owned_string(CefString(&cookie.domain)));
        value["path"_str]      = json_string(owned_string(CefString(&cookie.path)));
        value["secure"_str]    = Value::Bool(cookie.secure != 0);
        value["http_only"_str] = Value::Bool(cookie.httponly != 0);
        value["same_site"_str] = Value::Number(rstd::json::Number::from_i64(i64(cookie.same_site)));
        value["expires"_str]   = cookie.has_expires
                                     ? Value::Number(rstd::json::Number::from_i64(
                                           i64(cookie.expires.val / 1000000 - 11644473600LL)))
                                     : Value::Null();
        result_->cookies.push(rstd::move(value));
        return true;
    }

private:
    CefRefPtr<CookieResult> result_;
    Value                   names_;
    HELPER_REFCOUNT();
};

class ProtocolTask final : public CefTask {
public:
    ProtocolTask(CefRefPtr<Session> session, String uri)
        : session_(session), uri_(rstd::move(uri)) {}
    void Execute() override;

private:
    CefRefPtr<Session> session_;
    String             uri_;
    HELPER_REFCOUNT();
};

class LoginWindow final : public CefWindowDelegate {
public:
    LoginWindow(CefRefPtr<CefBrowserView> view, String title)
        : view_(view), title_(rstd::move(title)) {}
    void OnWindowCreated(CefRefPtr<CefWindow> window) override {
        window->SetTitle(cef_string(title_));
        window->SetToFillLayout();
        window->AddChildView(view_);
        window->Show();
        view_->RequestFocus();
    }
    void OnWindowDestroyed(CefRefPtr<CefWindow>) override { view_ = nullptr; }
    bool CanClose(CefRefPtr<CefWindow>) override {
        auto browser = view_ ? view_->GetBrowser() : nullptr;
        return ! browser || browser->GetHost()->TryCloseBrowser();
    }
    CefRect GetInitialBounds(CefRefPtr<CefWindow>) override { return CefRect(0, 0, 960, 720); }
    cef_runtime_style_t GetWindowRuntimeStyle() override { return CEF_RUNTIME_STYLE_ALLOY; }

private:
    CefRefPtr<CefBrowserView> view_;
    String                    title_;
    HELPER_REFCOUNT();
};

class Session final : public CefClient,
                      public CefLifeSpanHandler,
                      public CefRequestHandler,
                      public CefResourceRequestHandler,
                      public CefDisplayHandler,
                      public CefBrowserViewDelegate {
public:
    explicit Session(Value request): request_(rstd::move(request)) {}
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefRequestHandler>  GetRequestHandler() override { return this; }
    CefRefPtr<CefDisplayHandler>  GetDisplayHandler() override { return this; }
    cef_runtime_style_t GetBrowserRuntimeStyle() override { return CEF_RUNTIME_STYLE_ALLOY; }
    bool OnPopupBrowserViewCreated(CefRefPtr<CefBrowserView>, CefRefPtr<CefBrowserView> popup,
                                   bool) override {
        return CefWindow::CreateTopLevelWindow(new LoginWindow(popup, Title())) != nullptr;
    }
    bool OnConsoleMessage(CefRefPtr<CefBrowser>, cef_log_severity_t, const CefString&,
                          const CefString&, int) override {
        return true;
    }
    void Fail(ref<str> code, ref<str> message) {
        if (closing_) return;
        error(code, message);
        failed_ = true;
        Stop();
    }
    void Stop() {
        if (closing_) return;
        closing_ = true;
    }
    void CloseBrowsers() {
        if (! closing_) return;
        Vec<CefRefPtr<CefBrowser>> pending;
        for (auto& item : browsers_)
            if (! item.close_requested) {
                item.close_requested = true;
                pending.push(CefRefPtr<CefBrowser>(item.value));
            }
        for (const auto& browser : pending) browser->GetHost()->CloseBrowser(true);
    }
    bool Redirect(ref<str> uri) {
        if (closing_) return false;
        Uri actual(uri);
        for (const auto& target : redirects_)
            if (actual.same_target(target)) {
                auto result       = event("redirect"_str);
                result["uri"_str] = json_string(uri);
                if (! output(result))
                    Fail("output_limit"_str, "Browser redirect exceeds protocol limit"_str);
                return true;
            }
        return false;
    }
    bool Block(ref<str> uri) {
        return closing_ || Redirect(uri) || (! Uri(uri).http() && uri != "about:blank"_str);
    }
    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
        if (! browser->IsPopup())
            creating_ = false;
        else if (pending_popups_ > 0)
            --pending_popups_;
        browsers_.push(Browser { browser, {} });
        if (main_id_ == 0 && ! browser->IsPopup()) main_id_ = browser->GetIdentifier();
        if (! closing_ && ! ready_) {
            ready_ = true;
            if (! output(event("ready"_str))) Stop();
        }
    }
    void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
        browsers_.retain([&](const auto& item) {
            return item.value->GetIdentifier() != browser->GetIdentifier();
        });
        if (browser->GetIdentifier() == main_id_) Stop();
        if (! closing_)
            for (const auto& item : evaluations_)
                if (item.browser == browser->GetIdentifier()) {
                    Fail("navigation_changed"_str,
                         "The browser document closed during script evaluation"_str);
                    break;
                }
    }
    bool OnBeforePopup(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, int, const CefString& target,
                       const CefString&, cef_window_open_disposition_t, bool,
                       const CefPopupFeatures&, CefWindowInfo&, CefRefPtr<CefClient>&,
                       CefBrowserSettings&, CefRefPtr<CefDictionaryValue>& extra, bool*) override {
        if ((! target.empty() && Block(owned_string(target))) || closing_ ||
            browsers_.len() + usize(pending_popups_) >= usize(8))
            return true;
        extra = Extra();
        ++pending_popups_;
        return false;
    }
    void OnBeforePopupAborted(CefRefPtr<CefBrowser>, int) override {
        if (pending_popups_ > 0) --pending_popups_;
    }
    bool OnBeforeBrowse(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                        CefRefPtr<CefRequest> request, bool, bool) override {
        if (Block(owned_string(request->GetURL()))) return true;
        if (frame->IsMain()) {
            for (auto& item : browsers_)
                if (item.value->GetIdentifier() == browser->GetIdentifier()) item.token.clear();
            for (const auto& item : evaluations_)
                if (item.browser == browser->GetIdentifier()) {
                    Fail("navigation_changed"_str,
                         "The browser document changed during script evaluation"_str);
                    break;
                }
        }
        return false;
    }
    bool OnOpenURLFromTab(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, const CefString& target,
                          cef_window_open_disposition_t, bool) override {
        return Block(owned_string(target));
    }
    CefRefPtr<CefResourceRequestHandler>
    GetResourceRequestHandler(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefRequest>,
                              bool, bool, const CefString&, bool&) override {
        return this;
    }
    void OnProtocolExecution(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
                             CefRefPtr<CefRequest> request, bool& allow) override {
        allow = false;
        CefPostTask(TID_UI, new ProtocolTask(this, owned_string(request->GetURL())));
    }
    void OnRenderProcessTerminated(CefRefPtr<CefBrowser>, TerminationStatus, int,
                                   const CefString&) override {
        Fail("web_process"_str, "The browser web process terminated"_str);
    }
    auto Extra() -> CefRefPtr<CefDictionaryValue> {
        auto extra = CefDictionaryValue::Create();
        extra->SetString(config_key, cef_string(rstd::json::to_string(request_)));
        return extra;
    }
    auto Title() const -> String {
        auto title = text(request_, "title"_str);
        return String::make(title.is_empty() ? "Waywallen sign-in"_str : title);
    }
    bool Open() {
        if (auto values = request_["redirects"_str].as_array())
            for (const auto& value : **values) {
                Uri uri(*value.as_str());
                if (! uri.valid) {
                    Fail("invalid_request"_str, "Invalid browser redirect URI"_str);
                    return false;
                }
                redirects_.push(rstd::move(uri));
            }
        CefRequestContextSettings settings;
        context_ = CefRequestContext::CreateContext(settings, nullptr);
        if (! context_) return false;
        cookies_ = context_->GetCookieManager(nullptr);
        CefBrowserSettings browser;
        auto               view = CefBrowserView::CreateBrowserView(
            this, cef_string(text(request_, "url"_str)), browser, Extra(), context_, this);
        if (! view) return false;
        creating_ = true;
        if (! CefWindow::CreateTopLevelWindow(new LoginWindow(view, Title()))) {
            creating_ = false;
            return false;
        }
        return true;
    }
    bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                                  CefProcessId                 source,
                                  CefRefPtr<CefProcessMessage> message) override {
        if (source != PID_RENDERER || message->GetName() != message_key) return false;
        if (closing_ || ! frame || ! frame->IsMain() || ! frame->IsValid()) return true;
        auto bytes = owned_string(message->GetArgumentList()->GetString(0));
        if (bytes.len() > max_frame) {
            Fail("output_limit"_str, "Browser script result exceeds protocol limit"_str);
            return true;
        }
        auto parsed = rstd::json::from_str(bytes);
        if (parsed.is_err()) {
            Fail("javascript"_str, "Invalid browser script result"_str);
            return true;
        }
        auto&    value = *parsed;
        Browser* view  = nullptr;
        for (auto& item : browsers_)
            if (item.value->GetIdentifier() == browser->GetIdentifier()) view = &item;
        if (! view) return true;
        auto kind = text(value, "kind"_str);
        if (kind == "context"_str) {
            view->token = String::make(text(value, "token"_str));
            return true;
        }
        Uri current(owned_string(frame->GetURL()));
        if (view->token.is_empty() || view->token != text(value, "token"_str) ||
            ! current.same_origin(Uri(text(value, "origin"_str)))) {
            if (kind == "javascript"_str)
                Fail("navigation_changed"_str,
                     "The browser document changed during script evaluation"_str);
            return true;
        }
        if (kind == "script_message_error"_str && script_allowed(request_, current)) {
            Fail("output_limit"_str, "Browser script message exceeds protocol limit"_str);
        } else if (kind == "script_message"_str && script_allowed(request_, current)) {
            auto result          = event("script_message"_str);
            result["origin"_str] = json_string(current.origin_string());
            result["value"_str]  = value["value"_str].clone();
            if (! output(result))
                Fail("output_limit"_str, "Browser script message exceeds protocol limit"_str);
        } else if (kind == "javascript"_str) {
            bool found = false;
            for (const auto& item : evaluations_)
                if (item.id == value["id"_str] && item.browser == browser->GetIdentifier() &&
                    item.frame == owned_string(frame->GetIdentifier()) && item.token == view->token)
                    found = true;
            if (! found) return true;
            evaluations_.retain([&](const auto& item) {
                return item.id != value["id"_str];
            });
            if (! text(value, "error"_str).is_empty())
                Fail(text(value, "error"_str), "Browser script evaluation failed"_str);
            else {
                auto result         = event("javascript"_str);
                result["id"_str]    = value["id"_str].clone();
                result["value"_str] = value["value"_str].clone();
                if (! output(result))
                    Fail("output_limit"_str, "Browser script result exceeds protocol limit"_str);
            }
        }
        return true;
    }
    void GetCookies(const Value& request, bool automatic = false) {
        if (closing_) return;
        if (! Uri(text(request, "uri"_str)).http() || ! names_valid(request["names"_str]) ||
            pending_cookies_ >= 32) {
            Fail("invalid_command"_str, "Invalid browser cookie request"_str);
            return;
        }
        ++pending_cookies_;
        CefRefPtr<CookieResult> result  = new CookieResult(this, request.clone(), automatic);
        CefRefPtr<Cookies>      visitor = new Cookies(result, request["names"_str].clone());
        if (! cookies_->VisitUrlCookies(cef_string(text(request, "uri"_str)), true, visitor))
            Fail("cookies"_str, "Could not read browser cookies"_str);
    }
    void CookieReady(const Value& request, Vec<Value> values, bool automatic) {
        --pending_cookies_;
        if (closing_) return;
        auto result       = event("cookies"_str);
        result["id"_str]  = request["id"_str].is_Null() ? number(u64()) : request["id"_str].clone();
        result["uri"_str] = request["uri"_str].clone();
        result["cookies"_str] = Value::Array(rstd::move(values));
        if (automatic) {
            auto selector = rstd::json::to_string(request);
            auto encoded  = rstd::json::to_string(result["cookies"_str]);
            for (auto& snapshot : snapshots_)
                if (snapshot.selector == selector) {
                    if (snapshot.encoded == encoded) return;
                    snapshot.encoded = rstd::move(encoded);
                    if (! output(result))
                        Fail("output_limit"_str,
                             "Browser cookie result exceeds protocol limit"_str);
                    return;
                }
            snapshots_.push(CookieSnapshot { rstd::move(selector), rstd::move(encoded) });
        }
        if (! output(result))
            Fail("output_limit"_str, "Browser cookie result exceeds protocol limit"_str);
    }
    void TickCookies() {
        if (closing_ || ! ready_ || pending_cookies_) return;
        if (auto requests = request_["cookie_requests"_str].as_array())
            for (const auto& request : **requests) GetCookies(request, true);
    }
    void Command(const Value& value) {
        if (closing_) return;
        if (! value.as_object() || value["version"_str].as_u64() != Some(u64(1))) {
            Fail("protocol_version"_str, "Unsupported browser protocol version"_str);
            return;
        }
        auto command = text(value, "command"_str);
        if (command == "close"_str)
            Stop();
        else if (command == "get_cookies"_str && value["id"_str].as_u64())
            GetCookies(value);
        else if (command == "navigate"_str && Uri(text(value, "url"_str)).http()) {
            for (const auto& browser : browsers_)
                if (browser.value->GetIdentifier() == main_id_)
                    browser.value->GetMainFrame()->LoadURL(cef_string(text(value, "url"_str)));
        } else if (command == "evaluate"_str && value["id"_str].as_u64() &&
                   Uri(text(value, "origin"_str)).origin() && value["source"_str].as_str() &&
                   valid_text(text(value, "source"_str), usize(256 * 1024)) &&
                   evaluations_.len() < usize(32)) {
            for (const auto& pending : evaluations_)
                if (pending.id == value["id"_str]) {
                    Fail("invalid_command"_str, "Duplicate browser evaluation ID"_str);
                    return;
                }
            for (const auto& browser : browsers_) {
                auto frame = browser.value->GetMainFrame();
                if (! frame || browser.token.is_empty() ||
                    ! Uri(owned_string(frame->GetURL()))
                          .same_origin(Uri(text(value, "origin"_str))))
                    continue;
                auto request         = value.clone();
                request["token"_str] = json_string(browser.token);
                evaluations_.push(Evaluation { value["id"_str].clone(),
                                               browser.value->GetIdentifier(),
                                               owned_string(frame->GetIdentifier()),
                                               String::make(text(value, "origin"_str)),
                                               browser.token.clone() });
                send(frame, "waywallen.login.evaluate"_str, request, PID_RENDERER);
                return;
            }
            Fail("invalid_command"_str,
                 "Script target origin is not open in this browser session"_str);
        } else
            Fail("invalid_command"_str, "Invalid browser command"_str);
    }
    bool Done() const {
        return closing_ && browsers_.is_empty() && ! creating_ && pending_popups_ == 0 &&
               pending_cookies_ == 0;
    }
    bool Closing() const { return closing_; }
    bool Failed() const { return failed_; }

private:
    Value                        request_;
    Vec<Uri>                     redirects_;
    Vec<Browser>                 browsers_;
    Vec<Evaluation>              evaluations_;
    Vec<CookieSnapshot>          snapshots_;
    CefRefPtr<CefRequestContext> context_;
    CefRefPtr<CefCookieManager>  cookies_;
    int                          main_id_ {}, pending_cookies_ {}, pending_popups_ {};
    bool                         creating_ {}, ready_ {}, closing_ {}, failed_ {};
    HELPER_REFCOUNT();
};
void CookieResult::Execute() { session_->CookieReady(request_, rstd::move(cookies), automatic_); }
void ProtocolTask::Execute() { session_->Redirect(uri_); }

class Input {
public:
    Vec<u8> bytes;
    bool    eof {};
    auto    Next(bool blocking, bool& failure) -> Option<Value> {
        unsigned reads = 0;
        for (;;) {
            if (! blocking && ++reads > 8192) return None();
            char byte;
            auto count = ::read(STDIN_FILENO, &byte, 1);
            if (count < 0 && errno == EINTR) continue;
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return None();
            if (count <= 0) {
                eof = true;
                if (count < 0 || ! bytes.is_empty()) {
                    error("input"_str, "Incomplete browser protocol input"_str);
                    failure = true;
                }
                return None();
            }
            if (byte == '\n') {
                auto parsed = rstd::json::from_slice(bytes.as_slice());
                bytes.clear();
                if (parsed.is_ok()) return Some(rstd::move(parsed).unwrap());
                error("invalid_json"_str, "Invalid browser protocol JSON"_str);
                failure = true;
                return None();
            }
            if (bytes.len() >= max_frame) {
                error("input_limit"_str, "Browser protocol frame exceeds 1 MiB"_str);
                failure = true;
                return None();
            }
            bytes.push(u8(static_cast<unsigned char>(byte)));
        }
    }
};
class TemporaryProfile {
public:
    rstd::path::PathBuf path;
    TemporaryProfile() {
        auto template_path = rstd::env::temp_dir().join("owe-web-login-XXXXXX"_str);
        auto pattern       = template_path.as_path().to_str();
        if (! pattern) return;
        Vec<char> buffer;
        for (auto byte : pattern->as_bytes()) buffer.push(static_cast<char>(byte.to_primitive()));
        buffer.push('\0');
        if (auto created = ::mkdtemp(buffer.data()))
            path = rstd::path::PathBuf::from(rstd::ffi::CStr::from_ptr(created).to_str().unwrap());
    }
    ~TemporaryProfile() {
        if (! path.is_empty()) (void)rstd::fs::remove_dir_all(path.as_path());
    }
};
auto resources_dir() -> rstd::path::PathBuf {
    auto executable = rstd::fs::read_link("/proc/self/exe"_str);
    if (executable.is_err()) return {};
    auto parent = executable->as_path().parent();
    return parent ? rstd::path::PathBuf::from(*parent) : rstd::path::PathBuf {};
}
auto resource_error(const rstd::path::PathBuf& directory) -> String {
    if (directory.is_empty())
        return String::make("Could not locate the CEF executable directory"_str);
    for (auto name : { "icudtl.dat"_str,
                       "resources.pak"_str,
                       "chrome_100_percent.pak"_str,
                       "locales/en-US.pak"_str }) {
        auto metadata = rstd::fs::metadata(directory.join(name).as_path());
        if (metadata.is_err() || ! metadata->is_file())
            return rstd::format("CEF runtime resource is missing: {}", name);
    }
    return {};
}
} // namespace

bool HasBrowserHelperArgument(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (! argv[i]) continue;
        auto raw      = rstd::ffi::CStr::from_ptr(argv[i]);
        auto argument = raw.to_bytes();
        if (argument == "--"_str.as_bytes()) break;
        if (argument == "--web-helper"_str.as_bytes()) return true;
    }
    return false;
}

int RunBrowserHelper(int argc, char** argv) {
    ::signal(SIGPIPE, SIG_IGN);
    CefRefPtr<HelperApp> app = new HelperApp;
    for (int i = 1; i < argc; ++i) {
        auto           argument = rstd::ffi::CStr::from_ptr(argv[i]).to_str().unwrap_or(""_str);
        constexpr auto prefix   = "--web-helper-schemes="_str;
        if (argument.starts_with(prefix)) {
            auto names = argument.get(prefix.len(), argument.len()).unwrap();
            while (! names.is_empty()) {
                auto comma = names.find(","_str);
                auto end   = comma.unwrap_or(names.len());
                app->schemes.push(String::make(names.get(usize(), end).unwrap()));
                if (! comma) break;
                names = names.get(end + usize(1), names.len()).unwrap();
            }
        }
    }
    CefMainArgs args(argc, argv);
    if (int result = CefExecuteProcess(args, app, nullptr); result >= 0) return result;
    bool check = false;
    for (int i = 1; i < argc; ++i) {
        auto argument = rstd::ffi::CStr::from_ptr(argv[i]).to_str().unwrap_or(""_str);
        if (argument == "--web-helper"_str) continue;
        if (argument == "--check"_str && ! check)
            check = true;
        else {
            error("arguments"_str, "Usage: owe-web --web-helper [--check]"_str);
            return 2;
        }
    }
    auto directory = resources_dir();
    auto failure   = resource_error(directory);
    if (check) {
        Value result;
        result["version"_str]   = number(u64(1));
        result["available"_str] = Value::Bool(failure.is_empty());
        result["backend"_str]   = json_string("chromium"_str);
        if (! failure.is_empty()) result["error"_str] = json_string(failure);
        output(result);
        return failure.is_empty() ? 0 : 1;
    }
    Input input;
    bool  input_failure = false;
    auto  first         = input.Next(true, input_failure);
    if (! first) {
        if (! input_failure) error("input"_str, "Browser open request was not received"_str);
        return 2;
    }
    if (! first->as_object() || (*first)["version"_str].as_u64() != Some(u64(1))) {
        error("protocol_version"_str, "Unsupported browser protocol version"_str);
        return 2;
    }
    if (text(*first, "command"_str) != "open"_str) {
        error("invalid_command"_str, "The first browser command must be open"_str);
        return 2;
    }
    if (! open_valid(*first)) {
        error("invalid_request"_str, "Invalid browser open request"_str);
        return 2;
    }
    if (auto redirects = (*first)["redirects"_str].as_array())
        for (const auto& target : **redirects) {
            auto scheme = uri_scheme(*target.as_str()).unwrap();
            bool exists = builtin_scheme(scheme);
            for (const auto& previous : app->schemes)
                if (previous == scheme) exists = true;
            if (! exists) app->schemes.push(rstd::move(scheme));
        }
    if (! failure.is_empty()) {
        error("runtime_unavailable"_str, failure);
        return 1;
    }
    int flags = ::fcntl(STDIN_FILENO, F_GETFL);
    if (flags < 0 || ::fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) < 0) {
        error("input"_str, "Could not configure browser command input"_str);
        return 1;
    }
    CefSettings      settings;
    TemporaryProfile profile;
    if (profile.path.is_empty()) {
        error("storage"_str, "Could not create temporary browser storage"_str);
        return 1;
    }
    settings.no_sandbox                  = true;
    settings.log_severity                = LOGSEVERITY_DISABLE;
    settings.multi_threaded_message_loop = false;
    CefString(&settings.root_cache_path) = cef_string(profile.path.as_path().to_str().unwrap());
    CefString(&settings.resources_dir_path) =
        cef_string(directory.as_path().to_str().unwrap_or(""_str));
    CefString(&settings.locales_dir_path) =
        cef_string(directory.join("locales"_str).as_path().to_str().unwrap_or(""_str));
    if (! CefInitialize(args, settings, app, nullptr)) {
        error("runtime_unavailable"_str, "Could not initialize the Chromium browser runtime"_str);
        return 1;
    }
    CefRefPtr<Session> session = new Session(first.take().unwrap());
    if (! app->registration_succeeded)
        session->Fail("invalid_request"_str, "Could not register browser redirect schemes"_str);
    else if (! session->Open())
        session->Fail("browser"_str, "Could not create the browser window"_str);
    unsigned ticks = 0;
    while (! session->Done()) {
        session->CloseBrowsers();
        CefDoMessageLoopWork();
        if (! session->Closing()) {
            for (unsigned count = 0; count < 8; ++count) {
                auto command = input.Next(false, input_failure);
                if (input_failure || input.eof) {
                    session->Stop();
                    break;
                }
                if (! command) break;
                session->Command(*command);
                if (session->Closing()) break;
            }
            if (++ticks % 50 == 0) session->TickCookies();
        }
        rstd::thread::sleep(rstd::time::Duration::from_millis(u64(10)));
    }
    bool failed = session->Failed() || input_failure;
    session     = nullptr;
    CefShutdown();
    if (! failed) output(event("closed"_str));
    return failed ? 1 : 0;
}
} // namespace weweb
