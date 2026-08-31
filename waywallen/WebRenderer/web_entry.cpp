module;

#include <rstd/enum.hpp>

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

#include <sys/prctl.h>
#include <sys/socket.h>

module waywallen.web_entry;

import rstd;
import rstd.argparse;
import rstd.cppstd;
import rstd.log;
import owe.user_property;
import owe.audio_response;
import wescene.cli;
import wescene.json;
import vvk;
import weweb;
import waywallen.bridge;
import waywallen.bridge_audio;
import waywallen.bridge_producer_core;
import waywallen.web_producer_device;

namespace
{

using namespace rstd::prelude;
using namespace rstd::literals;

struct Options {
    std::string           ipc_path;
    u32                   width { 1920 };
    u32                   height { 1080 };
    std::filesystem::path workshop_dir;
    std::string           workshop_id;
    u32                   initial_fps { 60 };
    f32                   initial_volume { 1.0f };
    i32                   remote_debugging_port {};
    bool                  enable_audio { true };
    bool                  shared_texture_enabled { true };
    std::string           render_node;
    rstd::json::Map       initial_user_properties;
};

[[noreturn]] void die(const std::string& msg) {
    rstd_error("waywallen-weweb-renderer: {}", msg);
    std::exit(1);
}

std::string ToStdString(const rstd::string::String& value) {
    return rstd::cppstd::to_string(value.as_str());
}

template<typename T>
const T& ArgValue(const rstd::argparse::Matches& matches, const rstd::argparse::ArgKey<T>& key) {
    auto value = matches.get_one(key);
    if (value.is_err() || value->is_none()) rstd::unreachable();
    return ***value;
}

Options parse_args(int argc, char** argv) {
    using namespace rstd::argparse;

    auto command = Command::make("waywallen-weweb-renderer"_str);
    auto ipc     = command.add_arg(Arg<rstd::string::String>::value("ipc"_str, string_parser())
                                       .long_name("ipc"_str)
                                       .help("Unix-domain socket path for daemon IPC"_str)
                                       .required());
    auto path =
        command.add_arg(Arg<rstd::string::String>::value("path"_str, string_parser())
                            .long_name("path"_str)
                            .help("Workshop directory (containing project.json + index.html)"_str)
                            .default_value(""_str));
    auto workshop_id = command.add_arg(
        Arg<rstd::string::String>::value("workshop_id"_str, string_parser())
            .long_name("workshop_id"_str)
            .help("Optional Steam workshop id (informational; used for cache dir)"_str)
            .default_value(""_str));
    auto render_node =
        command.add_arg(Arg<rstd::string::String>::value("render-node"_str, string_parser())
                            .long_name("render-node"_str)
                            .help("DRM render-node path to pin Vulkan/CEF GPU selection to "
                                  "(empty => let the renderer pick the default)"_str)
                            .default_value(""_str));
    command.add_arg(Arg<rstd::string::String>::value("remaining"_str, string_parser())
                        .num_args(NumArgs::any())
                        .allow_hyphen_values());

    auto parsed = owe::cli::ParseArgs(rstd::move(command), argc, argv);
    if (parsed.is_err()) std::exit(parsed.unwrap_err().code);
    auto matches = rstd::move(parsed).unwrap();

    Options options;
    options.ipc_path     = ToStdString(ArgValue(matches, ipc));
    options.workshop_dir = ToStdString(ArgValue(matches, path));
    options.workshop_id  = ToStdString(ArgValue(matches, workshop_id));
    options.render_node  = ToStdString(ArgValue(matches, render_node));
    return options;
}

const char* kv_get(const ww_kv_list_t& kv, const char* key) {
    for (uint32_t i = 0; i < kv.count; ++i) {
        if (kv.data[i].key && std::strcmp(kv.data[i].key, key) == 0) return kv.data[i].value;
    }
    return nullptr;
}

f32 parse_f32(const char* s, f32 fallback) {
    if (! s || ! *s) return fallback;
    char* end   = nullptr;
    errno       = 0;
    float value = std::strtof(s, &end);
    if (errno != 0 || end == s) return fallback;
    return f32(value);
}

// Daemon serializes bool settings as the literal "true"/"false".
bool parse_bool(const char* s, bool def) {
    if (! s || ! *s) return def;
    if (std::strcmp(s, "true") == 0) return true;
    if (std::strcmp(s, "false") == 0) return false;
    return def;
}

u32 parse_u32(const char* s, u32 fallback) {
    if (! s || ! *s) return fallback;
    char*         end = nullptr;
    unsigned long v   = std::strtoul(s, &end, 10);
    if (end == s) return fallback;
    return u32(static_cast<uint32_t>(v));
}

i32 parse_i32(const char* s, i32 fallback) {
    if (! s || ! *s) return fallback;
    char* end = nullptr;
    errno     = 0;
    long v    = std::strtol(s, &end, 10);
    if (errno != 0 || end == s) return fallback;
    return i32(static_cast<int32_t>(v));
}

std::filesystem::path derive_cache_dir(const std::string& workshop_id) {
    namespace fs    = std::filesystem;
    const char* xdg = std::getenv("XDG_CACHE_HOME");
    fs::path    base;
    if (xdg && *xdg) {
        base = xdg;
    } else {
        const char* home = std::getenv("HOME");
        if (! home || ! *home) return {};
        base = fs::path(home) / ".cache";
    }
    fs::path dir = base / "waywallen-weweb-renderer";
    if (! workshop_id.empty()) dir /= workshop_id;
    std::error_code ec;
    fs::create_directories(dir, ec); // best-effort
    return dir;
}

std::filesystem::path executable_dir(const char* argv0) {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto            self = fs::read_symlink("/proc/self/exe", ec);
    if (! ec && ! self.empty()) return self.parent_path();
    if (argv0) return fs::path(argv0).parent_path();
    return fs::current_path();
}

// --- Host event queue -------------------------------------------------------
// The reader thread decodes bridge events; the CEF UI thread applies them.
class HostMsg final {
    RSTD_ENUM(HostMsg, (Setting, (std::string key; std::string value;)), (SyncPauseVisibility),
              (OpenAudioGate), (RuntimeMute, (bool muted;)),
              (PointerMove, (i32 x; i32 y; bool left_down;)),
              (PointerButton, (i32 x; i32 y; i32 button; bool down;)),
              (PointerAxis, (i32 x; i32 y; i32 delta_x; i32 delta_y;)))
};

using HostSender   = rstd::sync::mpmc::Sender<HostMsg>;
using HostReceiver = rstd::sync::mpmc::Receiver<HostMsg>;

using BridgeSubscriptions = std::shared_ptr<ww_wescene::BridgeSubscriptionController>;

struct AudioState {
    owe::audio::ResponseEngine engine;
    owe::audio::ResponseFrame  response {};
    rstd::time::Instant        received {};
    bool                       primed { false };
};

struct ReaderState {
    bool left_down { false };
    u64  last_audio_generation {};
    u64  last_audio_sequence {};
};

struct HostState {
    HostState(HostSender tx, HostReceiver rx)
        : control_tx(rstd::move(tx)), control_rx(rstd::move(rx)) {}

    int                             sock { -1 };
    ww_wescene::BridgeProducerCore* core { nullptr };
    weweb::BrowserHost*             host { nullptr };
    owe::Json*                      user_properties { nullptr };

    HostSender   control_tx;
    HostReceiver control_rx;

    rstd::sync::atomic::Atomic<bool> shutdown { false };
    rstd::sync::atomic::Atomic<bool> paused { false };
    rstd::sync::atomic::Atomic<bool> submitted_since_negotiate { false };
    rstd::sync::atomic::Atomic<u32>  target_fps { u32(60) };
    bool                             audio_enabled { true };
    f32                              base_volume { 1.0f };
    bool                             muted { false };
    bool                             audio_gate_open { false };

    rstd::sync::Mutex<BridgeSubscriptions> subscriptions { BridgeSubscriptions {} };
    rstd::sync::atomic::Atomic<bool>       audio_response_demand { false };
    rstd::sync::Mutex<AudioState>          audio { AudioState {} };
};

void set_audio_response_demand(HostState& s, bool active) {
    s.audio_response_demand.store(active, rstd::sync::atomic::Ordering::Release);
    {
        auto audio = s.audio.lock().unwrap();
        if (! active) {
            audio->engine.end();
            audio->primed = false;
        }
    }
    auto subscriptions = *s.subscriptions.lock().unwrap();
    if (subscriptions && ! subscriptions->set("audio", active)) {
        rstd_warn("waywallen-weweb-renderer: failed to update audio subscription");
    }
}

// Linux input-event-code → CEF cef_mouse_button_type_t. -1 for codes
// CEF can't represent (BTN_SIDE, BTN_EXTRA, …).
i32 cef_button_from_linux(u32 btn) {
    switch (btn.to_primitive()) {
    case 0x110: return i32();  // BTN_LEFT  -> MBT_LEFT
    case 0x111: return i32(2); // BTN_RIGHT -> MBT_RIGHT
    case 0x112: return i32(1); // BTN_MIDDLE-> MBT_MIDDLE
    default: return i32(-1);
    }
}

void enqueue_host_message(HostState& s, HostMsg msg) { (void)s.control_tx.send(rstd::move(msg)); }

void enqueue_setting(HostState& s, std::string key, std::string value) {
    enqueue_host_message(s, HostMsg::Setting(rstd::move(key), rstd::move(value)));
}

f32 effective_volume(const HostState& s) {
    if (! s.audio_gate_open || ! s.audio_enabled || s.muted) return f32();
    return s.base_volume.clamp(f32(), f32(1.0f));
}

void apply_effective_volume(HostState& s) {
    if (s.host) s.host->ApplyVolume(effective_volume(s).to_primitive());
}

void open_audio_gate(HostState& s) {
    s.audio_gate_open = true;
    apply_effective_volume(s);
}

void set_runtime_mute(HostState& s, bool muted) {
    s.muted = muted;
    if (! muted) s.audio_gate_open = true;
    apply_effective_volume(s);
}

void merge_user_property_overrides(owe::Json& properties, const rstd::json::Map& overrides) {
    if (! properties.is_object()) properties = owe::Json::Object(rstd::json::Map::make());
    overrides.iter().for_each([&](auto entry) {
        auto [entry_key, entry_value] = entry;
        auto current                  = properties.get(entry_key->as_str());
        auto descriptor =
            current.is_some()
                ? owe::MergeUserPropertyDescriptor(**current, *entry_value)
                : owe::MergeUserPropertyDescriptor(owe::JsonFromStd(""), *entry_value);
        auto object = properties.as_object_mut();
        (*object)->insert(entry_key->clone(), rstd::move(descriptor));
    });
}

void apply_setting(HostState& s, HostMsg::Setting_payload&& setting) {
    if (! s.host) return;
    if (setting.key == "volume") {
        // Wire format is u32 0..100; CEF host takes 0..1 ratio.
        s.base_volume = parse_f32(setting.value.c_str(), f32(100.0f)) / f32(100.0f);
        apply_effective_volume(s);
    } else if (setting.key == "fps") {
        auto fps = parse_u32(setting.value.c_str(), u32());
        if (fps > u32()) {
            s.target_fps.store(fps, rstd::sync::atomic::Ordering::Release);
            s.host->SetFrameRate(static_cast<int>(fps.to_primitive()));
        }
    } else {
        auto patch      = owe::MakeUserPropertyWirePatch(setting.value);
        auto descriptor = patch.clone();
        if (s.user_properties) {
            auto current = s.user_properties->get(rstd::cppstd::as_str(setting.key).unwrap());
            if (current.is_some()) {
                descriptor = owe::MergeUserPropertyDescriptor(**current, patch);
            }
            auto object = s.user_properties->as_object_mut();
            if (object.is_some()) {
                (*object)->insert(
                    ::alloc::string::String::make(rstd::cppstd::as_str(setting.key).unwrap()),
                    descriptor.clone());
            }
        }
        s.host->ApplyUserProperty(setting.key, descriptor);
    }
}

void sync_pause_visibility(HostState& s) {
    if (! s.host) return;
    const bool hide = s.paused.load(rstd::sync::atomic::Ordering::Acquire) &&
                      s.submitted_since_negotiate.load(rstd::sync::atomic::Ordering::Acquire);
    s.host->SetPaused(hide);
    if (! hide) s.host->Invalidate();
}

void drain_host_messages(HostState& s) {
    while (true) {
        auto received = s.control_rx.try_recv();
        if (received.is_err()) break;
        auto message = rstd::move(received).unwrap_unchecked();
        RSTD_MATCH(rstd::move(message)) {
            RSTD_CASE_PAYLOAD(Setting, value) { apply_setting(s, rstd::move(value)); }
            RSTD_CASE(SyncPauseVisibility) { sync_pause_visibility(s); }
            RSTD_CASE(OpenAudioGate) { open_audio_gate(s); }
            RSTD_CASE_PAYLOAD(RuntimeMute, value) { set_runtime_mute(s, value.muted); }
            RSTD_CASE_PAYLOAD(PointerMove, value) {
                if (s.host) {
                    s.host->OnMouseMove(
                        value.x.to_primitive(), value.y.to_primitive(), value.left_down);
                }
            }
            RSTD_CASE_PAYLOAD(PointerButton, value) {
                if (s.host) {
                    s.host->OnMouseButton(value.x.to_primitive(),
                                          value.y.to_primitive(),
                                          value.button.to_primitive(),
                                          value.down,
                                          1);
                }
            }
            RSTD_CASE_PAYLOAD(PointerAxis, value) {
                if (s.host) {
                    s.host->OnMouseWheel(value.x.to_primitive(),
                                         value.y.to_primitive(),
                                         value.delta_x.to_primitive(),
                                         value.delta_y.to_primitive());
                }
            }
        }
    }
}

rstd::time::Duration frame_delay(const HostState& s) {
    auto fps = s.target_fps.load(rstd::sync::atomic::Ordering::Acquire);
    if (fps == u32()) fps = u32(60);
    fps = fps.min(u32(240));
    return rstd::time::Duration::from_micros(u64(1'000'000u / fps.to_primitive()));
}

template<typename RenderToSlot>
void submit_bridge_slot(HostState& s, ww_wescene::BridgeProducerCore& core,
                        RenderToSlot&& render_to_slot) {
    VkImage  slot_image = VK_NULL_HANDLE;
    uint32_t slot_w = 0, slot_h = 0;
    if (! core.acquireSlot(&slot_image, &slot_w, &slot_h)) return;

    int sync_fd = render_to_slot(slot_image, VkExtent2D { slot_w, slot_h }, core.format());
    core.submitSlot(sync_fd);
    if (sync_fd < 0) return;

    s.submitted_since_negotiate.store(true, rstd::sync::atomic::Ordering::Release);
    if (s.paused.load(rstd::sync::atomic::Ordering::Acquire) && s.host) s.host->SetPaused(true);
}

// --- Reader thread ----------------------------------------------------------

void apply_control(HostState& s, ReaderState& reader, ww_bridge_control_t& msg) {
    switch (msg.op) {
    case WW_EVT_IN_INIT:
        rstd_warn("waywallen-weweb-renderer: unexpected late Init; ignoring");
        break;
    case WW_EVT_IN_SETTING_CHANGED: {
        const auto& settings = msg.u.setting_changed.settings;
        for (uint32_t i = 0; i < settings.count; ++i) {
            const char* key = settings.data[i].key;
            const char* val = settings.data[i].value;
            if (! key || ! val) continue;
            enqueue_setting(s, key, val);
        }
        break;
    }
    case WW_EVT_IN_PLAY:
        s.paused.store(false, rstd::sync::atomic::Ordering::Release);
        enqueue_host_message(s, HostMsg::SyncPauseVisibility());
        enqueue_host_message(s, HostMsg::OpenAudioGate());
        break;
    case WW_EVT_IN_PAUSE:
        s.paused.store(true, rstd::sync::atomic::Ordering::Release);
        enqueue_host_message(s, HostMsg::SyncPauseVisibility());
        break;
    case WW_EVT_IN_MUTE: enqueue_host_message(s, HostMsg::RuntimeMute(true)); break;
    case WW_EVT_IN_UNMUTE: enqueue_host_message(s, HostMsg::RuntimeMute(false)); break;
    case WW_EVT_IN_POINTER_MOTION: {
        // Daemon transforms display-local coords into renderer-tex
        // pixel space before sending; CEF view rect is opened at the
        // same pixel size, so the values map 1:1.
        const auto& pm = msg.u.pointer_motion.event;
        enqueue_host_message(s, HostMsg::PointerMove(i32(pm.x), i32(pm.y), reader.left_down));
        break;
    }
    case WW_EVT_IN_POINTER_BUTTON: {
        const auto& pb      = msg.u.pointer_button.event;
        auto        cef_btn = cef_button_from_linux(u32(pb.button));
        if (cef_btn >= i32()) {
            bool down = pb.state != 0;
            if (cef_btn == i32()) reader.left_down = down;
            enqueue_host_message(s, HostMsg::PointerButton(i32(pb.x), i32(pb.y), cef_btn, down));
        }
        break;
    }
    case WW_EVT_IN_POINTER_AXIS: {
        const auto& pa = msg.u.pointer_axis.event;
        // delta_* arrives in "logical notches" (1.0 per wheel
        // click). CEF wants pixel-ish deltas; 40 px/notch matches
        // the GLFW WebViewer convention.
        constexpr float kPxPerNotch = 40.0f;
        enqueue_host_message(
            s,
            HostMsg::PointerAxis(i32(pa.x),
                                 i32(pa.y),
                                 i32(static_cast<int32_t>(pa.delta_x * kPxPerNotch)),
                                 i32(static_cast<int32_t>(pa.delta_y * kPxPerNotch))));
        break;
    }
    case WW_EVT_IN_EVENT_SUBSCRIPTIONS_APPLIED: {
        auto subscriptions = *s.subscriptions.lock().unwrap();
        if (subscriptions) subscriptions->applied(msg.u.event_subscriptions_applied.result);
        break;
    }
    case WW_EVT_IN_AUDIO_WINDOW: {
        owe::audio::PcmWindow audio {};
        bool                  ended = false;
        if (! ww_wescene::DecodeAudioWindow(msg, audio, ended)) break;
        if (! s.audio_response_demand.load(rstd::sync::atomic::Ordering::Acquire)) break;
        auto        subscriptions = *s.subscriptions.lock().unwrap();
        const auto& wire          = msg.u.audio_window.window;
        if (! subscriptions || ! subscriptions->acceptsAudio(wire.subscription_revision)) break;
        auto generation = u64(wire.generation);
        auto sequence   = u64(wire.sequence);
        if (generation < reader.last_audio_generation ||
            (generation == reader.last_audio_generation && sequence <= reader.last_audio_sequence))
            break;
        auto state = s.audio.lock().unwrap();
        if (ended) {
            state->engine.end();
            state->primed = false;
        } else if (state->engine.analyze(audio, state->response)) {
            state->received = rstd::time::Instant::now();
            state->primed   = true;
        }
        reader.last_audio_generation = generation;
        reader.last_audio_sequence   = sequence;
        break;
    }
    case WW_EVT_IN_SHUTDOWN: s.shutdown.store(true, rstd::sync::atomic::Ordering::Release); break;
    case WW_EVT_IN_NEGOTIATE_BUFFERS: {
        const auto& d = msg.u.negotiate_buffers.directive;
        if (s.core) s.core->queueDirective(d);
        s.submitted_since_negotiate.store(false, rstd::sync::atomic::Ordering::Release);
        enqueue_host_message(s, HostMsg::SyncPauseVisibility());
        break;
    }
    case WW_EVT_IN_REQUEST_FRAME:
        if (s.core) s.core->requestFrame();
        break;
    case WW_EVT_IN_SET_LOG_LEVEL: ww_renderer_log_set_level(msg.u.set_log_level.level); break;
    default:
        rstd_warn("waywallen-weweb-renderer: unknown control op {}", static_cast<int>(msg.op));
        break;
    }
}

void reader_loop(HostState& s) {
    ReaderState reader;
    while (! s.shutdown.load(rstd::sync::atomic::Ordering::Acquire)) {
        ww_bridge_control_t msg {};
        int                 rc = ww_bridge_recv_control(s.sock, &msg);
        if (rc != 0) {
            if (! s.shutdown.load(rstd::sync::atomic::Ordering::Acquire)) {
                rstd_error("waywallen-weweb-renderer: recv_control failed: {}", rc);
            }
            s.shutdown.store(true, rstd::sync::atomic::Ordering::Release);
            return;
        }
        apply_control(s, reader, msg);
        ww_bridge_control_free(&msg);
    }
}

} // namespace

namespace waywallen
{
int run(int argc, char** argv) {
    // CRITICAL: CEF re-execs this binary as helper procs. Must run
    // before argparse / logging / anything with side effects.
    weweb::BrowserHost host;
    if (int helper_exit = host.RunOrExitIfHelper(argc, argv); helper_exit >= 0) {
        return helper_exit;
    }

    ww_renderer_log_init();

    Options opts = parse_args(argc, argv);

    ::prctl(PR_SET_PDEATHSIG, SIGTERM);

    if (opts.workshop_dir.empty() || ! std::filesystem::is_directory(opts.workshop_dir)) {
        die("--path must be an existing workshop directory");
    }

    auto [control_tx, control_rx] = rstd::sync::mpmc::channel<HostMsg>();
    HostState state(rstd::move(control_tx), rstd::move(control_rx));
    state.sock = ww_bridge_connect(opts.ipc_path.c_str());
    if (state.sock < 0) die("ww_bridge_connect: " + std::string(::strerror(-state.sock)));

    {
        waywallen_renderer_init_t init {};
        if (int rc = ww_bridge_recv_init(state.sock, &init); rc != 0) {
            const char* reason = (rc == -EPROTO)
                                     ? "init: protocol error or unsupported spawn_version"
                                     : "init: recv failed";
            waywallen_init_rejection_t rejection {
                .received_protocol_version  = init.protocol_version,
                .supported_protocol_version = WW_BRIDGE_SUPPORTED_PROTOCOL_VERSION,
                .received_spawn_version     = init.spawn_version,
                .supported_spawn_version    = WW_BRIDGE_SUPPORTED_SPAWN_VERSION,
                .reason                     = const_cast<char*>(reason),
            };
            ww_bridge_send_init_nack(state.sock, &rejection);
            waywallen_renderer_init_free(&init);
            die(std::string(reason) + " rc=" + std::to_string(rc));
        }

        // Web wallpapers don't have a fixed native resolution; the
        // `resolution` setting drives the actual extent against a
        // 16:9 baseline (compositor handles final letterbox / scale
        // on present). Schema disallows ORIGIN, so any invalid kv
        // falls back to 1080p.
        {
            auto resolution = i32(ww_resolution_sanitize(
                parse_i32(kv_get(init.settings, "resolution"), i32(WW_RESOLUTION_1080P))
                    .to_primitive()));
            if (resolution == i32(WW_RESOLUTION_ORIGIN) ||
                resolution == i32(WW_RESOLUTION_CUSTOM)) {
                resolution = i32(WW_RESOLUTION_1080P);
            }
            uint32_t width  = 16;
            uint32_t height = 9;
            ww_resolution_apply_cap(
                resolution.to_primitive(), WW_RESOLUTION_CAP_ALLOW_UPSCALE, &width, &height);
            opts.width  = u32(width);
            opts.height = u32(height);
        }
        opts.initial_fps = parse_u32(kv_get(init.settings, "fps"), opts.initial_fps);
        // Wire format is u32 0..100; CEF host takes 0..1 ratio.
        opts.initial_volume = parse_f32(kv_get(init.settings, "volume"), f32(100.0f)) / f32(100.0f);
        // identity=true: respawn-only. Translates to --mute-audio so
        // Chromium never opens an output device.
        opts.enable_audio = parse_bool(kv_get(init.settings, "enable_audio"), true);
        opts.shared_texture_enabled =
            parse_bool(kv_get(init.settings, "shared_texture_enabled"), true);
        opts.remote_debugging_port = i32(static_cast<int32_t>(
            parse_u32(kv_get(init.settings, "remote_debugging_port"), u32()).to_primitive()));
        // CLI `--render-node` wins over Init kv (mirroring scene/mpv/video).
        if (opts.render_node.empty()) {
            if (const char* v = kv_get(init.settings, "render_node"); v && *v) {
                opts.render_node = v;
            }
        }
        state.target_fps.store(opts.initial_fps > u32() ? opts.initial_fps : u32(60),
                               rstd::sync::atomic::Ordering::Release);
        state.audio_enabled = opts.enable_audio;
        state.base_volume   = opts.initial_volume;

        if (init.user_properties && *init.user_properties) {
            auto parsed = owe::ParseJson(init.user_properties, { .allow_comments = true });
            if (parsed.is_err()) {
                rstd_warn("init.user_properties is invalid JSON; ignored: {}", parsed.unwrap_err());
            } else {
                auto value  = parsed.unwrap();
                auto object = value.as_object();
                if (object.is_some()) {
                    (*object)->iter().for_each([&](auto entry) {
                        auto [key, value] = entry;
                        opts.initial_user_properties.insert(key->clone(), value->clone());
                    });
                } else {
                    rstd_warn("init.user_properties is not a JSON object; ignored");
                }
            }
        }

        waywallen_renderer_init_free(&init);
    }

    auto manifest_opt = weweb::LoadWebManifest(opts.workshop_dir);
    if (! manifest_opt) die("LoadWebManifest failed");
    auto& manifest = *manifest_opt;
    merge_user_property_overrides(manifest.user_props, opts.initial_user_properties);
    state.user_properties = &manifest.user_props;

    ww_wescene::WebProducerDevice producer;
    if (! opts.render_node.empty()) {
        rstd_info("waywallen-weweb-renderer: render_node={} pinning Vulkan/CEF device",
                  opts.render_node);
        producer.SetRenderNode(opts.render_node);
    }
    if (! producer.Init()) die("WebProducerDevice::Init failed");

    ww_pool_vulkan_init_t pi {};
    pi.instance           = producer.Instance();
    pi.physical_device    = producer.Physical();
    pi.device             = producer.Device();
    pi.queue              = producer.Queue();
    pi.queue_family_index = producer.QueueFamily();
    pi.get_instance_proc_addr =
        reinterpret_cast<void* (*)(void*, const char*)>(vkGetInstanceProcAddr);
    pi.device_uuid = producer.DeviceUuid();
    pi.driver_uuid = producer.DriverUuid();
    {
        ww_bridge_vk_dt_t dt {};
        ww_bridge_vk_dt_load(&dt, vkGetInstanceProcAddr, producer.Instance());
        if (int rc = ww_bridge_vk_query_render_node(
                &dt, producer.Physical(), &pi.drm_render_major, &pi.drm_render_minor);
            rc != 0) {
            rstd_warn("waywallen-weweb-renderer: drm render-node query failed ({}); "
                      "topology will be unknown to daemon",
                      rc);
        }
    }
    pi.drm_render_fd     = -1; // bridge opens by minor
    pi.image_usage_flags = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    // TRANSFER_DST is the default contract for any producer slot.
    // weweb additionally writes via vkCmdBlitImage in
    // WebProducerDevice::BlitToSlot (CEF source extent/format never
    // matches the slot), so OR in BLIT_DST.
    pi.format_feature_flags = VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;

    ww_pool_t* pool = nullptr;
    if (int rc = ww_bridge_pool_create(WW_POOL_BACKEND_VULKAN, &pi, &pool); rc != 0)
        die("ww_bridge_pool_create failed: " + std::to_string(rc));

    auto session = ww_wescene::BridgeSession::Adopt(pool, state.sock);
    if (! session) {
        int error = errno;
        ww_bridge_pool_destroy(pool);
        die("bridge session socket dup failed: " + std::to_string(error));
    }
    ww_wescene::BridgeProducerCore core(session);
    state.core = &core;
    state.host = &host;

    core.setOnFirstNegotiated([&]() {
        // Apply the daemon's initial fps once slots are live. Set the
        // initial volume too — we couldn't before OpenWallpaper because
        // there was no browser yet, but we can post a property update
        // now and the listener will pick it up.
        if (opts.initial_fps > u32()) {
            host.SetFrameRate(static_cast<int>(opts.initial_fps.to_primitive()));
        }
        apply_effective_volume(state);
        rstd_info("waywallen-weweb-renderer: negotiated, fps={} volume={:.2}",
                  opts.initial_fps,
                  effective_volume(state));
    });

    // BrowserHost::Init wants resources / locales relative to argv[0].
    auto                            exe_dir = executable_dir(argv[0]);
    weweb::BrowserHost::InitOptions ho;
    ho.resources_dir = exe_dir;
    ho.locales_dir   = exe_dir / "locales";
    ho.cache_dir     = derive_cache_dir(opts.workshop_id);
    if (opts.remote_debugging_port > i32()) {
        ho.enable_remote_debugging = true;
        ho.remote_debugging_port   = opts.remote_debugging_port.to_primitive();
    }
    ho.enable_audio           = opts.enable_audio;
    ho.shared_texture_enabled = opts.shared_texture_enabled;
    if (! opts.render_node.empty()) {
        ho.render_node_override = opts.render_node;
    }
    if (! host.Init(ho)) die("BrowserHost::Init failed");

    host.SetAudioResponseDemandCallback([&state](bool active) {
        set_audio_response_demand(state, active);
    });

    // OnAcceleratedPaint runs synchronously on the CEF UI thread (=
    // the thread that drives Pump, which is this main thread). Drain
    // any pending negotiate directive first so the slot pool reflects
    // the latest extent / fourcc before acquiring.
    host.SetAcceleratedPaintCallback([&state, &core, &producer](const weweb::DmaBufFrame& frame) {
        core.drainPendingDirective();
        if (! core.ready()) return;

        auto imp = producer.Import(frame);
        if (! imp.ok) return;

        submit_bridge_slot(state,
                           core,
                           [&producer, &imp](VkImage    slot_image,
                                             VkExtent2D slot_extent,
                                             VkFormat /*slot_format*/) {
                               return producer.BlitToSlot(imp, slot_image, slot_extent);
                           });
        producer.DestroyImported(imp);
    });

    host.SetCpuPaintCallback([&state, &core, &producer](const weweb::CpuPaintFrame& frame) {
        core.drainPendingDirective();
        if (! core.ready()) return;

        submit_bridge_slot(
            state,
            core,
            [&producer, &frame](VkImage slot_image, VkExtent2D slot_extent, VkFormat slot_format) {
                return producer.UploadToSlot(frame, slot_image, slot_extent, slot_format);
            });
    });

    weweb::BrowserHost::OpenOptions open_opts;
    open_opts.shared_texture_enabled = opts.shared_texture_enabled;
    open_opts.initially_muted        = true;
    open_opts.frame_rate             = static_cast<int>(opts.initial_fps.to_primitive());
    if (! host.OpenWallpaper(manifest,
                             opts.workshop_dir,
                             static_cast<int>(opts.width.to_primitive()),
                             static_cast<int>(opts.height.to_primitive()),
                             open_opts)) {
        die("BrowserHost::OpenWallpaper failed");
    }

    if (int rc = session->advertiseCaps(
            opts.width.to_primitive(), opts.height.to_primitive(), WW_MEM_HINT_DEVICE_LOCAL);
        rc != 0)
        die("ww_bridge_pool_advertise_caps failed: " + std::to_string(rc));

    rstd_info("waywallen-weweb-renderer: ready, advertised caps {}x{}", opts.width, opts.height);

    auto subscriptions = std::make_shared<ww_wescene::BridgeSubscriptionController>(session);
    *state.subscriptions.lock().unwrap() = subscriptions;
    std::vector<std::string> event_kinds { "pointer" };
    if (state.audio_response_demand.load(rstd::sync::atomic::Ordering::Acquire)) {
        event_kinds.emplace_back("audio");
    }
    if (! subscriptions->replace(rstd::move(event_kinds))) {
        die("failed to register renderer event subscriptions");
    }

    auto reader = rstd::thread::spawn([&]() {
        reader_loop(state);
    });
    if (reader.is_err()) die("failed to spawn bridge reader thread");
    auto reader_handle = rstd::move(reader).unwrap_unchecked();

    auto next_audio_push = rstd::time::Instant::now();

    while (! state.shutdown.load(rstd::sync::atomic::Ordering::Acquire) && ! host.ShouldExit()) {
        drain_host_messages(state);
        core.drainPendingDirective();
        (void)core.republishRequestedFrame();
        host.Pump();
        // CEF's OSR pacing goes idle without explicit invalidate kicks
        // (see project memory: CEF 147 OSR + DMA-BUF). Dedup happens
        // inside CEF at windowless_frame_rate.
        host.Invalidate();

        const auto now = rstd::time::Instant::now();
        if (state.audio_response_demand.load(rstd::sync::atomic::Ordering::Acquire) &&
            now >= next_audio_push) {
            next_audio_push = now + rstd::time::Duration::from_millis(u64(33));
            std::array<float, 128> response {};
            {
                auto audio = state.audio.lock().unwrap();
                if (audio->primed &&
                    now - audio->received <= rstd::time::Duration::from_millis(u64(250))) {
                    for (std::size_t index = 0; index < 64; ++index) {
                        response[index]      = audio->response.left[usize(index)];
                        response[index + 64] = audio->response.right[usize(index)];
                    }
                }
            }
            host.PushAudioData(response.data(), response.size());
        }

        rstd::thread::sleep(frame_delay(state));
    }

    state.shutdown.store(true, rstd::sync::atomic::Ordering::Release);
    ::shutdown(state.sock, SHUT_RD);
    rstd::move(reader_handle).join().unwrap();
    (void)subscriptions->replace({});
    state.subscriptions.lock().unwrap()->reset();
    host.Shutdown();
    session.reset();
    ww_bridge_close(state.sock);
    return 0;
}
} // namespace waywallen
