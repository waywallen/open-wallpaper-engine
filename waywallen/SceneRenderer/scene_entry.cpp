module;

#include <rstd/macro.hpp>

#include <errno.h>
#include <ctype.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

#include <sys/prctl.h>
#include <sys/socket.h>

module waywallen.scene_entry;

import rstd;
import vvk;
import rstd.argparse;
import rstd.cppstd;
import rstd.log;
import wescene.cli;
import wescene.fs;
import wescene.json;
import wescene.rgraph;
import wescene.scene_wallpaper;
import wescene.pkg.parse;
import waywallen.bridge;
import waywallen.bridge_audio;
import waywallen.bridge_ex_swapchain;
import waywallen.bridge_session;

using rstd::sync::Mutex;
using rstd::time::Duration;

using rstd::ffi::CStr;

namespace
{

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::collections::BTreeSet;
using rstd::cppstd::as_str;
using rstd::ffi::CString;
using rstd::io::error::Error;
using rstd::path::PathBuf;
using rstd::sync::Arc;
using rstd::sync::Weak;

struct Options {
    String   ipc_path;
    uint32_t width { 1920 };
    uint32_t height { 1080 };
    PathBuf  initial_scene;
    PathBuf  initial_assets;
    String   workshop_id;
    uint32_t initial_fps { 30 };
    bool     test_pattern { false };
    float    initial_volume { 1.0f };
    float    initial_playback_rate { 1.0f };
    bool     initial_mouse_parallax { true };
    bool     settings_enable_audio { true };
    bool     property_enable_audio { true };
    String   render_node;
    String   video_hwdec;

    String load_bench_output;
    // 1 disables MSAA. Clamped against device caps in VulkanRender::init.
    uint32_t                                 msaa_samples { 1 };
    rstd::json::Map                          initial_user_properties;
    Option<Arc<owe::wpscene::SceneDocument>> initial_scene_document;
};

constexpr const char* kSettingsEnableAudioKey = "enable_audio";
constexpr const char* kPropertyEnableAudioKey = "waywallen.enable_audio";
constexpr const char* kPlaybackSpeedKey       = "waywallen.playback_speed";
constexpr const char* kMouseParallaxKey       = "waywallen.mouse_parallax";

[[noreturn]] void die(ref<str> msg) {
    rstd_error("waywallen-wescene-renderer: {}", msg);
    rstd::process::exit(i32(1));
}

const char* diagnostic_code_name(owe::SceneUserPropertyDiagnosticCode code) {
    switch (code) {
    case owe::SceneUserPropertyDiagnosticCode::SceneVfsUnavailable: return "scene-vfs-unavailable";
    case owe::SceneUserPropertyDiagnosticCode::UnsupportedShaderComboValue:
        return "unsupported-shader-combo-value";
    case owe::SceneUserPropertyDiagnosticCode::MissingShaderVariantDescriptor:
        return "missing-shader-variant-descriptor";
    case owe::SceneUserPropertyDiagnosticCode::ShaderComboCompileFailed:
        return "shader-combo-compile-failed";
    }
    return "unknown";
}

bool env_flag_enabled(const char* name) {
    auto value = rstd::env::var_os(rstd::ffi::CStr::from_ptr(name).to_str().unwrap());
    if (! value || value->is_empty()) return false;
    auto bytes = value->as_os_str().as_encoded_bytes();
    return bytes != "0"_str.as_bytes() && bytes != "false"_str.as_bytes() &&
           bytes != "FALSE"_str.as_bytes() && bytes != "off"_str.as_bytes() &&
           bytes != "OFF"_str.as_bytes();
}

PathBuf shader_cache_dir() {
    auto    configured = rstd::env::var_os("XDG_CACHE_HOME"_str);
    PathBuf path;
    if (configured.is_some() && ! configured->is_empty()) {
        path = PathBuf::from(rstd::move(configured).unwrap_unchecked());
    } else {
        auto home = rstd::env::var_os("HOME"_str);
        if (home.is_none() || home->is_empty()) return {};
        path = PathBuf::from(rstd::move(home).unwrap_unchecked());
        path.push(".cache"_str);
    }
    path.push("wescene-renderer"_str);
    return path;
}

const char* pass_type_name(owe::rg::PassNode::Type type) {
    switch (type) {
    case owe::rg::PassNode::Type::CustomShader: return "custom-shader";
    case owe::rg::PassNode::Type::Copy: return "copy";
    case owe::rg::PassNode::Type::Virtual: return "virtual";
    }
    return "unknown";
}

const char* texture_request_kind_name(owe::vulkan::TextureRequestKind kind) {
    switch (kind) {
    case owe::vulkan::TextureRequestKind::Imported: return "imported";
    case owe::vulkan::TextureRequestKind::RenderTarget: return "render-target";
    case owe::vulkan::TextureRequestKind::RenderTargetMsaa: return "render-target-msaa";
    case owe::vulkan::TextureRequestKind::DepthAttachment: return "depth-attachment";
    }
    return "unknown";
}

const char* texture_usage_name(owe::resource::TextureUsage usage) {
    switch (usage) {
    case owe::resource::TextureUsage::Color: return "color";
    case owe::resource::TextureUsage::Depth: return "depth";
    }
    return "unknown";
}

const char* texture_lifetime_name(owe::resource::TextureLifetimeClass lifetime) {
    switch (lifetime) {
    case owe::resource::TextureLifetimeClass::FrameLocal: return "frame-local";
    case owe::resource::TextureLifetimeClass::Retained: return "retained";
    case owe::resource::TextureLifetimeClass::Dedicated: return "dedicated";
    case owe::resource::TextureLifetimeClass::ExternalOwned: return "external-owned";
    }
    return "unknown";
}

String render_item_text(const Option<owe::RenderItemId>& id) {
    if (id.is_none()) return "-"_Str;
    return rstd::format("{}/{}", id->index, id->generation);
}

String texture_request_text(const owe::vulkan::PassTextureRequestDiagnostic& diagnostic) {
    if (! diagnostic.request) return "none"_Str;
    const auto& request = *diagnostic.request;
    auto        text    = rstd::format(
        "{} name='{}'", as_str(texture_request_kind_name(request.kind)).unwrap(), request.name);
    if (request.source) {
        text.push_str(
            rstd::format(" source={}/{}", request.source->index, request.source->generation)
                .as_str());
    }
    if (request.definition) {
        const auto& definition = *request.definition;
        text.push_str(rstd::format(" definition={}x{} usage={} mip={} samples={}",
                                   definition.width,
                                   definition.height,
                                   as_str(texture_usage_name(definition.usage)).unwrap(),
                                   definition.mip_levels,
                                   definition.samples)
                          .as_str());
    }
    text.push_str(
        rstd::format(" lifetime={}", as_str(texture_lifetime_name(request.lifetime)).unwrap())
            .as_str());
    return text;
}

void log_prepared_pass_diagnostics(Vec<owe::vulkan::PreparedPassDiagnostic> diagnostics) {
    rstd_info("waywallen-wescene-renderer: prepared pass diagnostics: {} pass(es)",
              diagnostics.len());
    for (const auto& diagnostic : diagnostics) {
        const String graph_node = diagnostic.graph_node.is_some()
                                      ? rstd::format("{}", diagnostic.graph_node->index)
                                      : "-"_Str;
        const char*  pass_type  = diagnostic.pass_type.is_some()
                                      ? pass_type_name(*diagnostic.pass_type)
                                      : (diagnostic.frame_pass ? "frame" : "unknown");
        const String pipeline_cache_key =
            diagnostic.pipeline_cache_key ? rstd::format("{}", diagnostic.pipeline_cache_key->value)
                                          : "-"_Str;
        const String render_pass_cache_key =
            diagnostic.render_pass_cache_key
                ? rstd::format("{}", diagnostic.render_pass_cache_key->value)
                : "-"_Str;
        const String framebuffer_cache_key =
            diagnostic.framebuffer_cache_key
                ? rstd::format("{}", diagnostic.framebuffer_cache_key->value)
                : "-"_Str;
        rstd_info("waywallen-wescene-renderer: pass '{}' type={} graph={} render_item={} "
                  "dirty={} pipeline_key={} pipeline_seen={} pipeline_count={} "
                  "render_pass_key={} render_pass_seen={} render_pass_count={} "
                  "framebuffer_key={} framebuffer_seen={} framebuffer_count={} "
                  "prepared={} releases={}",
                  diagnostic.pass_name,
                  pass_type,
                  graph_node,
                  render_item_text(diagnostic.render_item),
                  diagnostic.invalidation_flags,
                  pipeline_cache_key,
                  diagnostic.pipeline_cache_hit,
                  diagnostic.pipeline_cache_observed_count,
                  render_pass_cache_key,
                  diagnostic.render_pass_cache_hit,
                  diagnostic.render_pass_cache_observed_count,
                  framebuffer_cache_key,
                  diagnostic.framebuffer_cache_hit,
                  diagnostic.framebuffer_cache_observed_count,
                  diagnostic.prepared,
                  diagnostic.release_textures.len());
        for (const auto& texture : diagnostic.texture_requests) {
            rstd_info("waywallen-wescene-renderer:   texture role={} slot={} binding='{}' {}",
                      texture.role,
                      texture.slot,
                      texture.name,
                      texture_request_text(texture));
        }
    }
}

template<typename T>
const T& ArgValue(const rstd::argparse::Matches& matches, const rstd::argparse::ArgKey<T>& key) {
    auto value = matches.get_one(key);
    if (value.is_err() || value->is_none()) rstd::unreachable();
    return ***value;
}

Options parse_args(int argc, char** argv) {
    using namespace rstd::argparse;

    auto command = Command::make("waywallen-wescene-renderer"_str);
    auto ipc     = command.add_arg(Arg<String>::value("ipc"_str, string_parser())
                                       .long_name("ipc"_str)
                                       .help("Unix-domain socket path for daemon IPC"_str)
                                       .required());
    auto path    = command.add_arg(Arg<String>::value("path"_str, string_parser())
                                       .long_name("path"_str)
                                       .help("Wallpaper Engine .pkg path (canonical resource)"_str)
                                       .default_value(""_str));
    auto assets  = command.add_arg(Arg<String>::value("assets"_str, string_parser())
                                       .long_name("assets"_str)
                                       .help("Optional Wallpaper Engine assets directory"_str)
                                       .default_value(""_str));
    auto workshop_id = command.add_arg(Arg<String>::value("workshop_id"_str, string_parser())
                                           .long_name("workshop_id"_str)
                                           .help("Optional Steam workshop id (informational)"_str)
                                           .default_value(""_str));
    auto render_node =
        command.add_arg(Arg<String>::value("render-node"_str, string_parser())
                            .long_name("render-node"_str)
                            .help("DRM render-node path to pin Vulkan device selection to "
                                  "(empty => let Vulkan pick the default)"_str)
                            .default_value(""_str));
    auto hwdec =
        command.add_arg(Arg<String>::value("hwdec"_str, string_parser())
                            .long_name("hwdec"_str)
                            .help("Video texture decoder mode: auto, vulkan, vaapi, or none"_str)
                            .default_value(""_str));
    auto load_bench_output =
        command.add_arg(Arg<String>::value("load-bench-output"_str, string_parser())
                            .long_name("load-bench-output"_str)
                            .help("Write scene load probe report to FILE"_str)
                            .value_name("FILE"_str)
                            .default_value(""_str));
    command.add_arg(Arg<String>::value("remaining"_str, string_parser())
                        .num_args(NumArgs::any())
                        .allow_hyphen_values());

    auto parsed = owe::cli::ParseArgs(rstd::move(command), argc, argv);
    if (parsed.is_err()) rstd::process::exit(i32(parsed.unwrap_err().code));
    auto matches = rstd::move(parsed).unwrap();

    Options options;
    options.ipc_path       = ArgValue(matches, ipc).clone();
    options.initial_scene  = PathBuf::from(ArgValue(matches, path).as_str());
    options.initial_assets = PathBuf::from(ArgValue(matches, assets).as_str());
    options.workshop_id    = ArgValue(matches, workshop_id).clone();
    options.render_node    = ArgValue(matches, render_node).clone();
    options.video_hwdec    = ArgValue(matches, hwdec).clone();

    options.load_bench_output = ArgValue(matches, load_bench_output).clone();
    return options;
}

// Linear-scan lookup for ww_kv_list_t. Lists are tiny (manifest-driven
// settings have <10 entries today).
const char* kv_get(const ww_kv_list_t& kv, const char* key) {
    for (uint32_t i = 0; i < kv.count; ++i) {
        if (kv.data[i].key &&
            (CStr::from_ptr(kv.data[i].key).to_bytes() == CStr::from_ptr(key).to_bytes()))
            return kv.data[i].value;
    }
    return nullptr;
}

// Parse a setting string as f32; falls back to `def` on parse error
// or a NULL pointer.
float parse_f32(const char* s, float def) {
    if (! s) return def;
    auto text = as_str(s);
    if (text.is_err()) return def;
    auto value = rstd::from_str<f32>(*text);
    return value.is_ok() && value->is_finite() ? value->to_primitive() : def;
}

bool parse_bool_wire(const char* raw, bool& out) {
    if (! raw) return false;
    auto parsed = as_str(raw);
    if (parsed.is_err()) return false;
    auto text       = parsed.unwrap();
    auto bytes      = text.as_bytes();
    auto whitespace = [](u8 ch) {
        return ch == u8(' ') || ch == u8('\t') || ch == u8('\r') || ch == u8('\n');
    };
    usize first {};
    auto  last = bytes.len();
    while (first < last && whitespace(bytes[first])) ++first;
    while (last > first && whitespace(bytes[last - usize(1)])) --last;
    if (first == last) return false;
    auto normalized = Vec<u8>::with_capacity(last - first);
    for (auto index = first; index < last; ++index) {
        auto ch = bytes[index];
        if (ch >= u8('A') && ch <= u8('Z')) ch += u8('a' - 'A');
        normalized.push(rstd::move(ch));
    }
    auto s = String::from_utf8(rstd::move(normalized)).unwrap();
    if (s == "true"_str || s == "1"_str || s == "yes"_str || s == "on"_str) {
        out = true;
        return true;
    }
    if (s == "false"_str || s == "0"_str || s == "no"_str || s == "off"_str) {
        out = false;
        return true;
    }
    return false;
}

bool parse_bool(const char* s, bool def) {
    bool out = def;
    return parse_bool_wire(s, out) ? out : def;
}

bool parse_user_property_bool(const owe::Json& raw, bool& out) {
    auto        member = raw.get("value"_str);
    const auto& value  = member.is_some() ? **member : raw;
    if (value.is_boolean()) {
        out = *value.as_bool();
        return true;
    }
    if (value.is_string()) {
        auto s = rstd::cppstd::to_string(*value.as_str());
        return parse_bool_wire(s.c_str(), out);
    }
    return false;
}

bool parse_playback_rate(ref<str> text, float& out) {
    auto pct = rstd::from_str<f64>(text.trim_ascii());
    if (pct.is_err() || ! pct->is_finite() || *pct < f64(10.0) || *pct > f64(400.0)) return false;
    out = static_cast<float>(pct->to_primitive() / 100.0);
    return true;
}

bool parse_playback_rate_wire(const char* raw, float& out) {
    if (! raw) return false;
    auto text = as_str(raw);
    return text.is_ok() && parse_playback_rate(*text, out);
}

bool parse_user_property_playback_rate(const owe::Json& raw, float& out) {
    auto        member = raw.get("value"_str);
    const auto& value  = member.is_some() ? **member : raw;
    if (value.is_number()) {
        auto number = value.as_f64();
        if (number.is_none() || ! number->is_finite()) return false;
        const auto pct = number->to_primitive();
        if (pct < 10.0 || pct > 400.0) return false;
        out = static_cast<float>(pct / 100.0);
        return true;
    }
    if (value.is_string()) {
        return parse_playback_rate(*value.as_str(), out);
    }
    return false;
}

int32_t parse_i32(const char* s, int32_t def) {
    if (! s) return def;
    auto text = as_str(s);
    if (text.is_err()) return def;
    return rstd::from_str<i32>(*text).unwrap_or(i32(def)).to_primitive();
}

auto parse_u32_value(const char* s) -> Option<u32> {
    if (! s) return None();
    auto text = as_str(s);
    if (text.is_err()) return None();
    return rstd::from_str<u32>(*text).ok();
}

uint32_t parse_u32(const char* s, uint32_t def) {
    return parse_u32_value(s).unwrap_or(u32(def)).to_primitive();
}

bool resolve_render_node_to_uuid(ref<str> path, rstd::array<uint8_t, VK_UUID_SIZE>& out_uuid,
                                 String& err_msg) {
    VkApplicationInfo app {};
    app.sType            = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "wescene-render-node-probe";
    app.apiVersion       = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici {};
    ici.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo        = &app;
    ici.enabledExtensionCount   = 0;
    ici.ppEnabledExtensionNames = nullptr;
    auto loaded                 = vvk::VulkanLoader::Open();
    if (loaded.is_err()) {
        err_msg = "Vulkan loader open failed"_Str;
        return false;
    }
    auto                  loader = loaded.unwrap_unchecked();
    vvk::InstanceDispatch dispatch {};
    vvk::Instance         instance;
    if (vvk::Instance::Create(instance, loader.global(), ici, dispatch).is_err()) {
        err_msg = "Vulkan instance creation failed"_Str;
        return false;
    }
    const VkInstance inst = *instance;

    ww_bridge_vk_dt_t dt {};
    if (ww_bridge_vk_dt_load(&dt, dispatch.resolver, inst) != 0) {
        err_msg = "ww_bridge_vk_dt_load failed"_Str;
        return false;
    }

    int rc = ww_bridge_vk_resolve_render_node(
        &dt,
        inst,
        CString::make(Vec<u8>::from(path.as_bytes())).unwrap().as_ptr(),
        out_uuid.data());

    if (rc == 0) return true;
    if (rc == -ENOENT) {
        err_msg = rstd::format("no Vulkan device with VK_EXT_physical_device_drm matches {}", path);
    } else if (rc == -ENOTSUP) {
        err_msg = "Vulkan instance lacks vkGetPhysicalDeviceProperties2 chain"_Str;
    } else if (rc < 0) {
        err_msg = rstd::format("ww_bridge_vk_resolve_render_node: {}",
                               Error::from_raw_os_error(i32(-rc)));
    } else {
        err_msg = rstd::format("ww_bridge_vk_resolve_render_node returned {}", rc);
    }
    return false;
}

// ---------------------------------------------------------------------------
// Host state shared between reader thread and main thread.
// ---------------------------------------------------------------------------

using BridgeSubscriptions = Option<Arc<ww_wescene::BridgeSubscriptionController>>;

struct ClearColorPublishState {
    bool                          ready { false };
    Option<rstd::array<float, 3>> pending;
};

struct HostState {
    int                             sock { -1 };
    Weak<ww_wescene::BridgeSession> session { Weak<ww_wescene::BridgeSession>::make() };
    // Non-owning pointer; the unique_ptr lives inside VulkanRender.
    ww_wescene::BridgeExSwapchain* swapchain { nullptr };
    // Non-owning pointer to the SceneWallpaper that lives in main's
    // stack frame; the reader thread uses it to dispatch ApplySettings
    // hot-reload (volume / fps) into the looper.
    owe::SceneWallpaper* wp { nullptr };

    // Render-target extent. Pointer events arrive in pixel coords from
    // the consumer display;
    u32 width {};
    u32 height {};

    rstd::sync::atomic::Atomic<bool> shutdown { false };
    rstd::sync::atomic::Atomic<bool> paused { false };
    rstd::sync::atomic::Atomic<bool> muted { false };
    rstd::sync::atomic::Atomic<bool> audio_gate_open { false };
    rstd::sync::atomic::Atomic<bool> settings_enable_audio { true };
    rstd::sync::atomic::Atomic<bool> property_enable_audio { true };
    rstd::sync::atomic::Atomic<bool> audio_response_demand { false };
    u64                              last_audio_generation {};
    u64                              last_audio_sequence {};
    f32                              base_volume { 1.0f };

    Mutex<BridgeSubscriptions> subscriptions { BridgeSubscriptions {} };

    // Daemon enforces "Ready before any ReportState" during the spawn
    // handshake; the scene-load path can fire `setOnClearColor` (and
    // thus a ReportState send) earlier than `ww_bridge_pool_advertise_caps`
    // (which is what actually triggers Ready). Stash any clear-colour
    // emitted before Ready, and flush after advertise_caps succeeds.
    Mutex<ClearColorPublishState> clear_color { ClearColorPublishState {} };
};

void signal_shutdown(HostState& s) {
    s.shutdown.store(true, rstd::sync::atomic::Ordering::Release);
    if (s.swapchain) s.swapchain->cancelFrameWait();
}

void set_audio_response_demand(HostState& s, bool active) {
    s.audio_response_demand.store(active, rstd::sync::atomic::Ordering::Release);
    if (! active && s.wp) {
        s.wp->endAudioResponse();
    }
    auto subscriptions = s.subscriptions.lock().unwrap()->clone();
    if (subscriptions && ! (*subscriptions)->set("audio"_str, active)) {
        rstd_warn("waywallen-wescene-renderer: failed to update audio subscription");
    }
}

float effective_volume(const HostState& s) {
    return s.base_volume.clamp(f32(), f32(1.0f)).to_primitive();
}

bool effective_audio_enabled(const HostState& s) {
    return s.settings_enable_audio.load(rstd::sync::atomic::Ordering::Acquire) &&
           s.property_enable_audio.load(rstd::sync::atomic::Ordering::Acquire);
}

void apply_volume_scale(HostState& s, float scale, uint32_t fade_ms) {
    if (s.wp) s.wp->setVolumeScale(f32(scale).clamp(f32(), f32(1.0f)).to_primitive(), fade_ms);
}

float runtime_volume_scale(const HostState& s) {
    const bool audio_gate_open = s.audio_gate_open.load(rstd::sync::atomic::Ordering::Acquire);
    return (audio_gate_open && effective_audio_enabled(s) &&
            ! s.paused.load(rstd::sync::atomic::Ordering::Acquire) &&
            ! s.muted.load(rstd::sync::atomic::Ordering::Acquire))
               ? 1.0f
               : 0.0f;
}

void apply_runtime_volume_scale(HostState& s, uint32_t fade_ms) {
    apply_volume_scale(s, runtime_volume_scale(s), fade_ms);
}

void set_base_volume(HostState& s, float volume) {
    s.base_volume = f32(volume);
    if (s.wp) s.wp->setVolume(effective_volume(s));
}

void set_runtime_pause(HostState& s, bool paused, uint32_t fade_ms) {
    const bool was_paused = s.paused.exchange(paused, rstd::sync::atomic::Ordering::AcqRel);
    if (! paused) s.audio_gate_open.store(true, rstd::sync::atomic::Ordering::Release);
    const bool was_audible = effective_audio_enabled(s) && ! was_paused &&
                             ! s.muted.load(rstd::sync::atomic::Ordering::Acquire);
    if (! s.wp) return;

    if (paused) {
        apply_runtime_volume_scale(s, fade_ms);
        s.wp->pause(was_audible ? fade_ms : 0);
        s.wp->requestFrame();
    } else {
        s.wp->play();
        apply_runtime_volume_scale(s, fade_ms);
    }
}

void set_runtime_mute(HostState& s, bool muted, uint32_t fade_ms) {
    s.muted.store(muted, rstd::sync::atomic::Ordering::Release);
    if (! muted) s.audio_gate_open.store(true, rstd::sync::atomic::Ordering::Release);
    apply_runtime_volume_scale(s, fade_ms);
}

void apply_audio_enabled(HostState& s, bool was_enabled) {
    if (! s.wp) return;
    const bool enabled = effective_audio_enabled(s);
    if (enabled == was_enabled) return;
    s.wp->setMuted(! enabled);
    if (enabled && ! s.paused.load(rstd::sync::atomic::Ordering::Acquire)) s.wp->play();
    apply_runtime_volume_scale(s, 0);
}

void set_settings_enable_audio(HostState& s, const char* value) {
    bool enabled = true;
    if (! parse_bool_wire(value, enabled)) {
        rstd_warn("waywallen-wescene-renderer: invalid {} value '{}'; ignoring",
                  kSettingsEnableAudioKey,
                  value ? value : "");
        return;
    }
    const bool was_enabled = effective_audio_enabled(s);
    s.settings_enable_audio.store(enabled, rstd::sync::atomic::Ordering::Release);
    apply_audio_enabled(s, was_enabled);
}

void set_property_enable_audio(HostState& s, const char* value) {
    bool enabled = true;
    if (! parse_bool_wire(value, enabled)) {
        rstd_warn("waywallen-wescene-renderer: invalid {} value '{}'; ignoring",
                  kPropertyEnableAudioKey,
                  value ? value : "");
        return;
    }
    const bool was_enabled = effective_audio_enabled(s);
    s.property_enable_audio.store(enabled, rstd::sync::atomic::Ordering::Release);
    apply_audio_enabled(s, was_enabled);
}

void set_playback_rate(HostState& s, const char* value) {
    float rate = 1.0f;
    if (! parse_playback_rate_wire(value, rate)) {
        rstd_warn("waywallen-wescene-renderer: invalid {} value '{}'; ignoring",
                  kPlaybackSpeedKey,
                  value ? value : "");
        return;
    }
    if (s.wp) s.wp->setSpeed(rate);
}

void set_mouse_parallax(HostState& s, const char* value) {
    bool enabled = true;
    if (! parse_bool_wire(value, enabled)) {
        rstd_warn("waywallen-wescene-renderer: invalid {} value '{}'; ignoring",
                  kMouseParallaxKey,
                  value ? value : "");
        return;
    }
    if (s.wp) s.wp->setMouseParallax(enabled);
}

void set_fps(HostState& s, uint32_t fps) {
    if (! s.wp || fps == 0) return;
    s.wp->setFps(fps);
}

String bridge_string(const char* value) {
    return value ? String::make(rstd::cppstd::as_str(value).unwrap()) : String {};
}

void apply_control(HostState& s, ww_bridge_control_t& msg) {
    switch (msg.op) {
    case WW_EVT_IN_INIT:
        // Init is consumed at the top of main before the reader thread
        // starts. A late Init is either a buggy daemon resending or a
        // protocol violation; log and ignore.
        rstd_warn("waywallen-wescene-renderer: unexpected late Init; ignoring");
        break;
    case WW_EVT_IN_SETTING_CHANGED: {
        const auto& settings = msg.u.setting_changed.settings;
        for (uint32_t i = 0; i < settings.count; ++i) {
            const char* key = settings.data[i].key;
            const char* val = settings.data[i].value;
            if (! key || ! val) continue;
            if ((CStr::from_ptr(key).to_bytes() == CStr::from_ptr("volume").to_bytes())) {
                // Wire format is u32 0..100; engine takes 0..1 ratio.
                set_base_volume(s, parse_f32(val, 100.0f) / 100.0f);
            } else if ((CStr::from_ptr(key).to_bytes() == CStr::from_ptr("fps").to_bytes())) {
                if (auto n = parse_u32_value(val)) set_fps(s, n->to_primitive());
            } else if ((CStr::from_ptr(key).to_bytes() ==
                        CStr::from_ptr(kSettingsEnableAudioKey).to_bytes())) {
                set_settings_enable_audio(s, val);
            } else if ((CStr::from_ptr(key).to_bytes() ==
                        CStr::from_ptr(kPropertyEnableAudioKey).to_bytes())) {
                set_property_enable_audio(s, val);
            } else if ((CStr::from_ptr(key).to_bytes() ==
                        CStr::from_ptr(kPlaybackSpeedKey).to_bytes())) {
                set_playback_rate(s, val);
            } else if (CStr::from_ptr(key).to_bytes() ==
                       CStr::from_ptr(kMouseParallaxKey).to_bytes()) {
                set_mouse_parallax(s, val);
            } else if ((CStr::from_ptr(key).to_bytes() ==
                        CStr::from_ptr("test_pattern").to_bytes())) {
                // Wescene's test_pattern flag is set on initial spawn
                // through RenderInit; runtime toggling is not wired
                // (would require respawn). Log and ignore.
            } else {
                if (s.wp)
                    s.wp->setUserPropertyRaw(rstd::cppstd::as_str(key).unwrap(),
                                             rstd::cppstd::as_str(val).unwrap());
            }
        }
        break;
    }
    case WW_EVT_IN_PLAY: set_runtime_pause(s, false, msg.u.play.transition.fade_ms); break;
    case WW_EVT_IN_PAUSE: set_runtime_pause(s, true, msg.u.pause.transition.fade_ms); break;
    case WW_EVT_IN_MUTE: set_runtime_mute(s, true, msg.u.mute.transition.fade_ms); break;
    case WW_EVT_IN_UNMUTE: set_runtime_mute(s, false, msg.u.unmute.transition.fade_ms); break;
    case WW_EVT_IN_POINTER_MOTION: {
        const auto& pm = msg.u.pointer_motion.event;
        if (s.wp && s.width > u32() && s.height > u32()) {
            s.wp->mouseInput(static_cast<double>(pm.x) / s.width.to_primitive(),
                             static_cast<double>(pm.y) / s.height.to_primitive());
            // The bridge has no explicit enter/leave; treat every motion
            // event as proof the cursor is inside.
            s.wp->mouseEnter(true);
        }
        break;
    }
    case WW_EVT_IN_POINTER_BUTTON: {
        const auto& pb = msg.u.pointer_button.event;
        if (s.wp) {
            // Linux BTN_* → SceneWallpaper button index (0=L, 1=R, 2=M),
            // matching the GLFW numbering scripts expect.
            int idx = -1;
            switch (pb.button) {
            case 0x110: idx = 0; break; // BTN_LEFT
            case 0x111: idx = 1; break; // BTN_RIGHT
            case 0x112: idx = 2; break; // BTN_MIDDLE
            default: break;
            }
            if (idx >= 0) s.wp->mouseButton(idx, pb.state != 0);
        }
        break;
    }
    case WW_EVT_IN_POINTER_AXIS: break;
    case WW_EVT_IN_MPRIS: {
        const auto& mpris = msg.u.mpris.snapshot;
        if (s.wp) {
            s.wp->setMediaStatus(owe::MediaStatus {
                .state            = mpris.state,
                .title            = bridge_string(mpris.title),
                .artist           = bridge_string(mpris.artist),
                .album            = bridge_string(mpris.album),
                .album_artist     = bridge_string(mpris.album_artist),
                .art_url          = bridge_string(mpris.art_url),
                .previous_art_url = bridge_string(mpris.previous_art_url),
            });
        }
        break;
    }
    case WW_EVT_IN_EVENT_SUBSCRIPTIONS_APPLIED: {
        auto subscriptions = s.subscriptions.lock().unwrap()->clone();
        if (subscriptions) (*subscriptions)->applied(msg.u.event_subscriptions_applied.result);
        break;
    }
    case WW_EVT_IN_AUDIO_WINDOW: {
        owe::audio::PcmWindow audio {};
        bool                  ended = false;
        if (! ww_wescene::DecodeAudioWindow(msg, audio, ended)) break;
        if (! s.audio_response_demand.load(rstd::sync::atomic::Ordering::Acquire)) break;
        auto        subscriptions = s.subscriptions.lock().unwrap()->clone();
        const auto& wire          = msg.u.audio_window.window;
        const bool  fresh         = u64(wire.generation) > s.last_audio_generation ||
                                    (u64(wire.generation) == s.last_audio_generation &&
                                     u64(wire.sequence) > s.last_audio_sequence);
        if (fresh && subscriptions && (*subscriptions)->acceptsAudio(wire.subscription_revision) &&
            s.wp) {
            if (ended)
                s.wp->endAudioResponse();
            else
                s.wp->setAudioPcmWindow(rstd::move(audio));
            s.last_audio_generation = u64(wire.generation);
            s.last_audio_sequence   = u64(wire.sequence);
        }
        break;
    }
    case WW_EVT_IN_SHUTDOWN: signal_shutdown(s); break;
    case WW_EVT_IN_NEGOTIATE_BUFFERS: {
        const auto& d = msg.u.negotiate_buffers.directive;
        // Hand off to the swapchain directly. The render thread drains
        // the pending directive at the head of its next acquireRenderTarget,
        // so this thread does no Vk / bridge slot work.
        if (s.swapchain) s.swapchain->queueDirective(d);
        if (s.wp && s.paused.load(rstd::sync::atomic::Ordering::Acquire)) s.wp->requestFrame();
        break;
    }
    case WW_EVT_IN_REQUEST_FRAME:
        if (s.swapchain && s.swapchain->requestFrame() && s.wp &&
            s.paused.load(rstd::sync::atomic::Ordering::Acquire))
            s.wp->requestFrame();
        break;
    case WW_EVT_IN_SET_LOG_LEVEL: ww_renderer_log_set_level(msg.u.set_log_level.level); break;
    default:
        rstd_warn("waywallen-wescene-renderer: unknown control op {}", static_cast<int>(msg.op));
        break;
    }
}

void reader_loop(HostState& s) {
    while (! s.shutdown.load(rstd::sync::atomic::Ordering::Acquire)) {
        ww_bridge_control_t msg {};
        int                 rc = ww_bridge_recv_control(s.sock, &msg);
        if (rc != 0) {
            if (! s.shutdown.load(rstd::sync::atomic::Ordering::Acquire)) {
                rstd_error("waywallen-wescene-renderer: recv_control failed: {}", rc);
            }
            signal_shutdown(s);
            return;
        }
        apply_control(s, msg);
        ww_bridge_control_free(&msg);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// entry
// ---------------------------------------------------------------------------

namespace waywallen
{
int run(int argc, char** argv) {
    ww_renderer_log_init();

    Options opts = parse_args(argc, argv);

    auto load_bench = owe::CreateSceneLoadBench(opts.load_bench_output.as_str());

    ::prctl(PR_SET_PDEATHSIG, SIGTERM);

    HostState host;
    host.sock = ww_bridge_connect(
        CString::make(Vec<u8>::from(opts.ipc_path.as_str().as_bytes())).unwrap().as_ptr());
    if (host.sock < 0)
        die(rstd::format("ww_bridge_connect: {}", Error::from_raw_os_error(i32(-host.sock)))
                .as_str());

    {
        waywallen_renderer_init_t init {};
        if (int rc = ww_bridge_recv_init(host.sock, &init); rc != 0) {
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
            ww_bridge_send_init_nack(host.sock, &rejection);
            waywallen_renderer_init_free(&init);
            die(rstd::format("{} rc={}", as_str(reason).unwrap(), rc).as_str());
        }

        // Scene .pkg path + assets + workshop_id arrive via CLI argv
        // (already parsed into opts.{initial_scene, initial_assets,
        // workshop_id}). Init carries only the resolved settings kv.
        // Use scene.json's authored canvas as the native aspect and then
        // apply the user's short-edge resolution setting.
        {
            int32_t resolution = ww_resolution_sanitize(parse_i32(
                kv_get(init.settings, "resolution"), static_cast<int32_t>(WW_RESOLUTION_1080P)));
            if (resolution == static_cast<int32_t>(WW_RESOLUTION_ORIGIN))
                resolution = static_cast<int32_t>(WW_RESOLUTION_1080P);
            uint32_t custom_extent = parse_u32(kv_get(init.settings, "custom_extent"), 1080u);
            if (! opts.initial_scene.is_empty()) {
                auto scene_doc = [&] {
                    if (load_bench.is_none())
                        return owe::wpscene::LoadSceneDocumentFromSource(
                            opts.initial_scene.as_path());

                    auto& context  = **load_bench;
                    auto  recorder = context.session().recorder();
                    auto  loaded   = [&] {
                        auto span = recorder.span(context.ids().preload_scene_document);
                        return owe::wpscene::LoadSceneDocumentFromSource(
                            opts.initial_scene.as_path());
                    }();
                    auto batch = recorder.drain();
                    if (batch.is_ok()) {
                        context.add_preload_batch(rstd::move(batch).unwrap_unchecked());
                    } else {
                        rstd_warn("waywallen-wescene-renderer: preload probe drain failed");
                    }
                    return loaded;
                }();
                if (scene_doc) {
                    opts.initial_scene_document =
                        Some(Arc<owe::wpscene::SceneDocument>::make(rstd::move(*scene_doc)));
                }
            }
            if (opts.initial_scene_document &&
                (*opts.initial_scene_document)->metadata.canvas_extent) {
                const auto extent = *(*opts.initial_scene_document)->metadata.canvas_extent;
                opts.width        = extent[usize(0)].to_primitive();
                opts.height       = extent[usize(1)].to_primitive();
                rstd_info(
                    "waywallen-wescene-renderer: scene canvas {}x{}", opts.width, opts.height);
            } else {
                opts.width  = 16;
                opts.height = 9;
                rstd_info("waywallen-wescene-renderer: scene canvas unknown, using 16:9 fallback");
            }
            if (resolution == static_cast<int32_t>(WW_RESOLUTION_CUSTOM)) {
                ww_resolution_apply_short_edge(
                    custom_extent, WW_RESOLUTION_CAP_ALLOW_UPSCALE, &opts.width, &opts.height);
            } else {
                ww_resolution_apply_cap(
                    resolution, WW_RESOLUTION_CAP_ALLOW_UPSCALE, &opts.width, &opts.height);
            }
            rstd_info("waywallen-wescene-renderer: render extent {}x{}", opts.width, opts.height);
        }
        if (const char* v = kv_get(init.settings, "fps"); v && *v) {
            if (auto n = parse_u32_value(v)) opts.initial_fps = n->to_primitive();
        }
        if (const char* v = kv_get(init.settings, "test_pattern"); v && *v) {
            opts.test_pattern = ((CStr::from_ptr(v).to_bytes() != CStr::from_ptr("0").to_bytes()));
        }
        // Wire format is u32 0..100; engine takes 0..1 ratio.
        opts.initial_volume = parse_f32(kv_get(init.settings, "volume"), 100.0f) / 100.0f;
        opts.settings_enable_audio =
            parse_bool(kv_get(init.settings, kSettingsEnableAudioKey), true);
        // CLI `--render-node` wins over Init kv (mirroring mpv/video).
        // Empty ⇒ let SceneWallpaper pick the default Vulkan device.
        if (opts.render_node.is_empty()) {
            if (const char* v = kv_get(init.settings, "render_node"); v && *v) {
                opts.render_node = String::make(as_str(v).unwrap());
            }
        }
        if (opts.video_hwdec.is_empty()) {
            if (const char* v = kv_get(init.settings, "hwdec"); v && *v) {
                opts.video_hwdec = String::make(as_str(v).unwrap());
            }
        }
        if (opts.video_hwdec.is_empty()) opts.video_hwdec = "auto"_Str;
        if (const char* v = kv_get(init.settings, "msaa"); v && *v) {
            if (auto n = parse_u32_value(v)) opts.msaa_samples = n->to_primitive();
        }
        // Per-item user-property overrides arrive as a raw JSON object
        // (the DB column verbatim) — decoupled from the schema-validated
        // plugin settings above so no name collision is possible.
        if (init.user_properties && *init.user_properties) {
            auto parsed_result = owe::ParseJson(rstd::cppstd::as_str(init.user_properties).unwrap(),
                                                { .allow_comments = true });
            if (parsed_result.is_err()) {
                rstd_warn("init.user_properties is invalid JSON; ignored: {}",
                          parsed_result.unwrap_err());
            } else {
                auto parsed = parsed_result.unwrap();
                if (! parsed.is_object()) {
                    rstd_warn("init.user_properties is not a JSON object; ignored");
                } else {
                    auto object = parsed.as_object();
                    (*object)->iter().for_each([&](auto entry) {
                        auto [entry_key, entry_value] = entry;
                        auto        k                 = entry_key->as_str();
                        const auto& v                 = *entry_value;
                        if (k == as_str(kPropertyEnableAudioKey).unwrap()) {
                            bool enabled = true;
                            if (parse_user_property_bool(v, enabled)) {
                                opts.property_enable_audio = enabled;
                            } else {
                                rstd_warn("waywallen-wescene-renderer: invalid {} initial value; "
                                          "using true",
                                          kPropertyEnableAudioKey);
                            }
                            return;
                        }
                        if (k == as_str(kPlaybackSpeedKey).unwrap()) {
                            float rate = 1.0f;
                            if (parse_user_property_playback_rate(v, rate)) {
                                opts.initial_playback_rate = rate;
                            } else {
                                rstd_warn("waywallen-wescene-renderer: invalid {} initial value; "
                                          "using 100",
                                          kPlaybackSpeedKey);
                            }
                            return;
                        }
                        if (k == as_str(kMouseParallaxKey).unwrap()) {
                            bool enabled = true;
                            if (parse_user_property_bool(v, enabled)) {
                                opts.initial_mouse_parallax = enabled;
                            } else {
                                rstd_warn("waywallen-wescene-renderer: invalid {} initial value; "
                                          "using true",
                                          kMouseParallaxKey);
                            }
                            return;
                        }
                        opts.initial_user_properties.insert(String::make(k), v.clone());
                    });
                }
            }
        }

        waywallen_renderer_init_free(&init);
    }

    owe::SceneWallpaper wp;
    if (! wp.init()) die("SceneWallpaper::init failed"_str);
    wp.setAudioClientIdentity({
        .application_name = bridge_string(WAYWALLEN_AUDIO_APPLICATION_NAME),
        .application_id   = bridge_string(WAYWALLEN_AUDIO_APPLICATION_ID),
        .stream_prefix    = bridge_string(WAYWALLEN_AUDIO_STREAM_PREFIX),
        .component        = "wescene"_Str,
        .media_name       = "Waywallen Scene Renderer"_Str,
        .media_role       = "music"_Str,
    });

    host.wp          = &wp;
    host.width       = u32(opts.width);
    host.height      = u32(opts.height);
    host.base_volume = f32(opts.initial_volume);
    host.settings_enable_audio.store(opts.settings_enable_audio,
                                     rstd::sync::atomic::Ordering::Release);
    host.property_enable_audio.store(opts.property_enable_audio,
                                     rstd::sync::atomic::Ordering::Release);

    wp.setAudioResponseDemandCallback([&host](bool active) {
        set_audio_response_demand(host, active);
    });

    // Forward the effective wallpaper background to the daemon. Alpha is
    // forced to 1.0 because the rendered DMA-BUF is opaque.
    wp.setOnClearColor([&host](float r, float g, float b) {
        if (host.sock < 0) return;
        auto clear = host.clear_color.lock().unwrap();
        if (! clear->ready) {
            // Daemon will reject ReportState received before Ready; stash
            // the latest value and replay after advertise_caps fires.
            clear->pending = Some(rstd::array<float, 3> { r, g, b });
            return;
        }
        auto session = host.session.upgrade();
        if (! session) return;
        if (int rc = session->sendClearColor(r, g, b, 1.0f); rc != 0) {
            rstd_warn("waywallen-wescene-renderer: report_state(clear_color) failed ({})", rc);
        }
    });
    wp.setOnUserPropertyDiagnostics([](Vec<owe::SceneUserPropertyDiagnostic> diagnostics) {
        for (const auto& diagnostic : diagnostics) {
            rstd_warn("waywallen-wescene-renderer: user property '{}' diagnostic {} "
                      "material='{}' combo='{}': {}",
                      diagnostic.key,
                      diagnostic_code_name(diagnostic.code),
                      diagnostic.material,
                      diagnostic.combo,
                      diagnostic.message);
        }
    });
    if (env_flag_enabled("OWE_DUMP_PREPARED_PASSES")) {
        wp.setOnFirstFrame([&wp] {
            wp.requestPreparedPassDiagnostics(
                owe::RenderPassDiagnosticCallback::make(&log_prepared_pass_diagnostics));
        });
    }

    owe::SceneWallpaperConfig wp_config;
    wp_config.source_pkg_path = opts.initial_scene.clone();
    wp_config.assets_dir      = opts.initial_assets.clone();
    wp_config.cache_dir       = shader_cache_dir();
    wp_config.scene_document  = opts.initial_scene_document.clone();
    wp_config.load_bench      = load_bench.clone();
    wp_config.user_properties = rstd::move(opts.initial_user_properties);
    wp_config.fps             = opts.initial_fps;
    wp_config.speed           = opts.initial_playback_rate;
    wp_config.mouse_parallax  = opts.initial_mouse_parallax;
    wp_config.volume          = effective_volume(host);
    wp_config.volume_scale    = 0.0f;
    wp_config.muted           = ! effective_audio_enabled(host);
    wp.configure(rstd::move(wp_config));

    // The factory runs inside VulkanRender::init after the GPU is picked
    // and the VkDevice is created; that's when ww_bridge_pool_create can
    // succeed. The swapchain owns the bridge session; host keeps only a
    // weak control-path reference.
    const bool msaa_enabled = opts.msaa_samples > 1;
    auto       factory =
        [&host, msaa_enabled](
            const owe::RenderInitInfo::ExSwapchainHandles& h) -> Option<owe::ExSwapchainOwner> {
        ww_pool_vulkan_init_t pi {};
        pi.instance           = h.instance;
        pi.physical_device    = h.physical_device;
        pi.device             = h.device;
        pi.queue              = h.graphics_queue;
        pi.queue_family_index = h.graphics_queue_family;
        pi.get_instance_proc_addr =
            reinterpret_cast<void* (*)(void*, const char*)>(h.get_instance_proc_addr);
        pi.device_uuid = nullptr; // bridge will zero
        pi.driver_uuid = nullptr;

        ww_bridge_vk_dt_t dt {};
        ww_bridge_vk_dt_load(&dt, h.get_instance_proc_addr, h.instance);
        if (int rc = ww_bridge_vk_query_render_node(
                &dt, h.physical_device, &pi.drm_render_major, &pi.drm_render_minor);
            rc != 0) {
            rstd_warn("waywallen-wescene-renderer: drm render-node query failed ({}); "
                      "topology will be unknown to daemon",
                      rc);
        }
        pi.drm_render_fd = -1; // bridge opens by minor
        // FinPass writes the slot via vkCmdCopyImage by default (single-
        // sample screen RT, extent + RGBA8 already match), so
        // TRANSFER_DST is always required. MSAA additionally needs
        // BLIT_DST because vkCmdBlitImage is the only op that does the
        // multi-sample → single-sample resolve at the same step.
        pi.image_usage_flags    = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        pi.format_feature_flags = VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        if (msaa_enabled) {
            pi.format_feature_flags |= VK_FORMAT_FEATURE_BLIT_DST_BIT;
        }
        ww_pool_t* pool = nullptr;
        if (int rc = ww_bridge_pool_create(WW_POOL_BACKEND_VULKAN, &pi, &pool); rc != 0) {
            rstd_error("waywallen-wescene-renderer: ww_bridge_pool_create failed: {}", rc);
            return None();
        }
        auto adopted = ww_wescene::BridgeSession::Adopt(pool, host.sock);
        if (! adopted) {
            int error = errno;
            ww_bridge_pool_destroy(pool);
            rstd_error("waywallen-wescene-renderer: bridge session socket dup failed: {}", error);
            return None();
        }
        (void)h; // Vulkan handles no longer needed by BridgeExSwapchain.
        auto session   = adopted.unwrap();
        auto sw        = owe::ExSwapchain::Make<ww_wescene::BridgeExSwapchain>(session.clone());
        host.session   = session.downgrade();
        host.swapchain = &static_cast<ww_wescene::BridgeExSwapchain&>(sw->AsSwapchain());
        return Some(rstd::move(sw));
    };

    rstd::array<uint8_t, VK_UUID_SIZE> chosen_uuid {};
    bool                               have_uuid = false;
    if (! opts.render_node.is_empty()) {
        String err;
        if (resolve_render_node_to_uuid(opts.render_node.as_str(), chosen_uuid, err)) {
            have_uuid = true;
            rstd_info("waywallen-wescene-renderer: render_node={} pinning Vulkan device by UUID",
                      opts.render_node);
        } else {
            rstd_warn("waywallen-wescene-renderer: render_node={} not honored: {}; "
                      "falling back to default device",
                      opts.render_node,
                      err);
        }
    }

    {
        owe::RenderInitInfo info;
        info.offscreen         = true;
        info.offscreen_tiling  = owe::TexTiling::OPTIMAL;
        info.width             = static_cast<uint16_t>(opts.width);
        info.height            = static_cast<uint16_t>(opts.height);
        info.video_hwdec       = opts.video_hwdec.clone();
        info.video_render_node = opts.render_node.clone();
        info.msaa_samples      = opts.msaa_samples;
        info.surface_info.createSurfaceOp =
            Some(owe::CreateSurfaceCallback::make([](VkInstance, VkSurfaceKHR*) -> VkResult {
                return VK_SUCCESS;
            }));
        info.ex_swapchain_factory =
            Some(Box<dyn<FnOnce<Option<owe::ExSwapchainOwner>(
                     const owe::RenderInitInfo::ExSwapchainHandles&)>>>::make(rstd::move(factory)));
        if (have_uuid) {
            info.uuid = Some(chosen_uuid);
        }
        wp.initVulkan(rstd::move(info));
    }

    if (! wp.waitVulkanInited(/*timeout_ms*/ 10000))
        die("VulkanRender did not finish init within 10s"_str);
    if (host.session.expired() || ! host.swapchain)
        die("ex_swapchain_factory did not produce a bridge session / swapchain"_str);

    host.swapchain->setOnFirstNegotiated([&] {
        if (host.paused.load(rstd::sync::atomic::Ordering::Acquire)) {
            rstd_info("waywallen-wescene-renderer: negotiated while paused");
        } else {
            wp.play();
            rstd_info("waywallen-wescene-renderer: negotiated, scene playback started");
        }
    });

    // Bridge sends ready + release_syncobj + format_caps in one go.
    auto session = host.session.upgrade();
    if (! session) die("bridge session expired before advertise"_str);
    if (int rc = session->advertiseCaps(
            opts.width, opts.height, WW_MEM_HINT_DEVICE_LOCAL | WW_MEM_HINT_HOST_VISIBLE);
        rc != 0)
        die(rstd::format("ww_bridge_pool_advertise_caps failed: {}", rc).as_str());

    rstd_info("waywallen-wescene-renderer: ready, advertise sent to daemon");

    auto subscriptions = Arc<ww_wescene::BridgeSubscriptionController>::make(session.clone());
    *host.subscriptions.lock().unwrap() = Some(subscriptions.clone());
    auto event_kinds                    = BTreeSet<String>::make();
    event_kinds.insert("pointer"_Str);
    event_kinds.insert("mpris"_Str);
    if (host.audio_response_demand.load(rstd::sync::atomic::Ordering::Acquire)) {
        event_kinds.insert("audio"_Str);
    }
    if (! subscriptions->replace(rstd::move(event_kinds))) {
        die("failed to register renderer event subscriptions"_str);
    }

    // Flip the clear-colour gate now that Ready has been emitted. Replay
    // any value the scene-load callback stashed during init.
    {
        auto clear   = host.clear_color.lock().unwrap();
        clear->ready = true;
        if (clear->pending) {
            auto c = *clear->pending;
            if (int rc = session->sendClearColor(c[usize()], c[usize(1)], c[usize(2)], 1.0f);
                rc != 0) {
                rstd_warn("waywallen-wescene-renderer: pending report_state(clear_color) "
                          "flush failed ({})",
                          rc);
            }
            clear->pending = None();
        }
    }

    auto reader = rstd::thread::spawn([&]() {
        reader_loop(host);
    });
    if (reader.is_err()) die("failed to spawn bridge reader thread"_str);
    auto reader_handle = rstd::move(reader).unwrap_unchecked();

    // Idle until shutdown; the reader thread dispatches live controls.
    while (! host.shutdown.load(rstd::sync::atomic::Ordering::Acquire)) {
        rstd::thread::sleep(Duration::from_millis(u64(100)));
    }

    ::shutdown(host.sock, SHUT_RD);
    rstd::move(reader_handle).join().unwrap();
    (void)subscriptions->replace(BTreeSet<String>::make());
    host.subscriptions.lock().unwrap()->take();
    session.reset();
    ww_bridge_close(host.sock);

    return 0;
}
} // namespace waywallen
