module;

#include <rstd/macro.hpp>
#include <rstd/enum.hpp>
#include "quickjs.h"

module wescene.script;
import eigen;
import owe.scene_audio_response;
import rstd;
import rstd.log;
import rstd.cppstd;
import wescene.types;
import wescene.scene;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::collections::HashMap;

namespace owe::script
{

// ---------------------------------------------------------------------------
// Field-kind inference. The bound field's name is the only signal we have at
// parse time; this table mirrors the empirical distribution from the corpus
// (see docs/scripting/wallpaper_engine_api.md).
// ---------------------------------------------------------------------------

namespace
{

FieldKind GuessFieldKind(std::string_view field) {
    // Visible/enabled-style fields: bool. Several scripts return numbers
    // 0/1 here too; coercion table accepts both.
    if (field == "visible") return FieldKind::Bool;
    // Vec3 (position-like) fields.
    if (field == "origin" || field == "scale" || field == "angles" || field == "spriteoffset")
        return FieldKind::Vec3;
    // Color (rgb) fields.
    if (field == "color" || field == "colorn" || field == "Bg color" || field == "Bar Color" ||
        field == "Inner Color" || field == "Outer Color" || field == "Color 1" ||
        field == "Color 2" || field == "Color filter")
        return FieldKind::Color;
    // Strings (text content). Recognised here so the JS can run without
    // erroring; the actuator side ignores the result for MVP scope.
    if (field == "text") return FieldKind::String;
    // Everything else is a scalar: alpha, rate, intensity, fov, volume,
    // parallaxDepth, percentage, brightness, saturation, ... .
    return FieldKind::Scalar;
}

const char* KindName(FieldKind k) {
    switch (k) {
    case FieldKind::Unknown: return "unknown";
    case FieldKind::Scalar: return "scalar";
    case FieldKind::Bool: return "bool";
    case FieldKind::Vec2: return "vec2";
    case FieldKind::Vec3: return "vec3";
    case FieldKind::Vec4: return "vec4";
    case FieldKind::Color: return "color";
    case FieldKind::String: return "string";
    }
    return "?";
}

bool IsFinite(double value) { return std::isfinite(value); }

JSValue MakeVecValue(JSContext* ctx, double x, double y, double z, int n) {
    JSValue global  = JS_GetGlobalObject(ctx);
    JSValue ctor    = JS_GetPropertyStr(ctx, global, n == 2 ? "Vec2" : "Vec3");
    JSValue argv[3] = { JS_NewFloat64(ctx, x), JS_NewFloat64(ctx, y), JS_NewFloat64(ctx, z) };
    JSValue obj     = JS_CallConstructor(ctx, ctor, n, argv);
    for (int i = 0; i < n; ++i) JS_FreeValue(ctx, argv[i]);
    if (n < 3) JS_FreeValue(ctx, argv[2]);
    JS_FreeValue(ctx, ctor);
    JS_FreeValue(ctx, global);
    return obj;
}

JSValue MakeVec4Value(JSContext* ctx, double x, double y, double z, double w) {
    JSValue global  = JS_GetGlobalObject(ctx);
    JSValue ctor    = JS_GetPropertyStr(ctx, global, "Vec4");
    JSValue argv[4] = {
        JS_NewFloat64(ctx, x), JS_NewFloat64(ctx, y), JS_NewFloat64(ctx, z), JS_NewFloat64(ctx, w)
    };
    JSValue obj = JS_CallConstructor(ctx, ctor, 4, argv);
    for (auto& arg : argv) JS_FreeValue(ctx, arg);
    JS_FreeValue(ctx, ctor);
    JS_FreeValue(ctx, global);
    return obj;
}

// JSValue→ScriptValue coercion. Mirrors the table in the API doc; never
// throws, returns monostate for unrecognised shapes.
ScriptValue CoerceReturn(JSContext* ctx, JSValue ret, FieldKind kind) {
    if (JS_IsUndefined(ret) || JS_IsNull(ret)) return {};

    auto read_field = [&](JSValue obj, const char* name, double& out) -> bool {
        JSValue v = JS_GetPropertyStr(ctx, obj, name);
        if (JS_IsUndefined(v)) {
            JS_FreeValue(ctx, v);
            return false;
        }
        double d  = 0.0;
        int    rc = JS_ToFloat64(ctx, &d, v);
        JS_FreeValue(ctx, v);
        if (rc < 0 || ! IsFinite(d)) return false;
        out = d;
        return true;
    };
    auto read_index = [&](JSValue arr, uint32_t i, double& out) -> bool {
        JSValue v = JS_GetPropertyUint32(ctx, arr, i);
        if (JS_IsUndefined(v)) {
            JS_FreeValue(ctx, v);
            return false;
        }
        double d  = 0.0;
        int    rc = JS_ToFloat64(ctx, &d, v);
        JS_FreeValue(ctx, v);
        if (rc < 0 || ! IsFinite(d)) return false;
        out = d;
        return true;
    };

    switch (kind) {
    case FieldKind::Bool: {
        int b = JS_ToBool(ctx, ret);
        return BoolValue { b > 0 };
    }
    case FieldKind::Scalar: {
        if (JS_IsBool(ret)) {
            int b = JS_ToBool(ctx, ret);
            return ScalarValue { b > 0 ? 1.0 : 0.0 };
        }
        double d = 0.0;
        if (JS_ToFloat64(ctx, &d, ret) >= 0 && IsFinite(d)) return ScalarValue { d };
        return {};
    }
    case FieldKind::Vec2: {
        Vec2Value v;
        if (JS_IsArray(ret)) {
            read_index(ret, 0, v.x);
            read_index(ret, 1, v.y);
        } else if (JS_IsObject(ret)) {
            read_field(ret, "x", v.x);
            read_field(ret, "y", v.y);
        } else {
            return {};
        }
        return v;
    }
    case FieldKind::Vec3: {
        Vec3Value v;
        if (JS_IsArray(ret)) {
            read_index(ret, 0, v.x);
            read_index(ret, 1, v.y);
            read_index(ret, 2, v.z);
        } else if (JS_IsObject(ret)) {
            read_field(ret, "x", v.x);
            read_field(ret, "y", v.y);
            read_field(ret, "z", v.z);
        } else if (JS_IsNumber(ret)) {
            // Many audio-response scripts return a *scalar* even when bound
            // to scale (vec3). Splat into all three components.
            double d = 0.0;
            JS_ToFloat64(ctx, &d, ret);
            if (! IsFinite(d)) return {};
            return Vec3Value { d, d, d };
        } else {
            return {};
        }
        return v;
    }
    case FieldKind::Vec4: {
        Vec4Value v;
        if (JS_IsArray(ret)) {
            read_index(ret, 0, v.x);
            read_index(ret, 1, v.y);
            read_index(ret, 2, v.z);
            read_index(ret, 3, v.w);
        } else if (JS_IsObject(ret)) {
            read_field(ret, "x", v.x);
            read_field(ret, "y", v.y);
            read_field(ret, "z", v.z);
            read_field(ret, "w", v.w);
        } else if (JS_IsNumber(ret)) {
            double d = 0.0;
            JS_ToFloat64(ctx, &d, ret);
            if (! IsFinite(d)) return {};
            return Vec4Value { d, d, d, d };
        } else {
            return {};
        }
        return v;
    }
    case FieldKind::Color: {
        ColorValue v;
        if (JS_IsArray(ret)) {
            read_index(ret, 0, v.r);
            read_index(ret, 1, v.g);
            read_index(ret, 2, v.b);
        } else if (JS_IsObject(ret)) {
            read_field(ret, "r", v.r);
            read_field(ret, "g", v.g);
            read_field(ret, "b", v.b);
        } else {
            return {};
        }
        return v;
    }
    case FieldKind::String: {
        const char* s = JS_ToCString(ctx, ret);
        if (! s) return {};
        StringValue sv { std::string(s) };
        JS_FreeCString(ctx, s);
        return sv;
    }
    case FieldKind::Unknown: return {};
    }
    return {};
}

JSValue ScriptValueToJs(JSContext* ctx, const ScriptValue& value) {
    if (auto* p = std::get_if<ScalarValue>(&value)) return JS_NewFloat64(ctx, p->v);
    if (auto* p = std::get_if<BoolValue>(&value)) return JS_NewBool(ctx, p->v);
    if (auto* p = std::get_if<Vec2Value>(&value)) return MakeVecValue(ctx, p->x, p->y, 0.0, 2);
    if (auto* p = std::get_if<Vec3Value>(&value)) return MakeVecValue(ctx, p->x, p->y, p->z, 3);
    if (auto* p = std::get_if<Vec4Value>(&value)) return MakeVec4Value(ctx, p->x, p->y, p->z, p->w);
    if (auto* p = std::get_if<ColorValue>(&value)) {
        JSValue arr = JS_NewArray(ctx);
        JS_DefinePropertyValueUint32(ctx, arr, 0, JS_NewFloat64(ctx, p->r), JS_PROP_C_W_E);
        JS_DefinePropertyValueUint32(ctx, arr, 1, JS_NewFloat64(ctx, p->g), JS_PROP_C_W_E);
        JS_DefinePropertyValueUint32(ctx, arr, 2, JS_NewFloat64(ctx, p->b), JS_PROP_C_W_E);
        return arr;
    }
    if (auto* p = std::get_if<StringValue>(&value))
        return JS_NewStringLen(ctx, p->s.data(), p->s.size());
    return JS_UNDEFINED;
}

// JSON → JSValue conversion for the initial-value seed. Recursive but
// scenescript values are tiny (numbers, short strings, small objects).
JSValue JsonToJs(JSContext* ctx, const Json& value) {
    RSTD_MATCH(value) {
        RSTD_CASE(Null) { return JS_NULL; }
        RSTD_CASE(Bool, boolean) { return JS_NewBool(ctx, boolean); }
        RSTD_CASE(Number, number) {
            if (auto integer = number.as_i64(); integer.is_some())
                return JS_NewInt64(ctx, integer->to_primitive());
            if (auto integer = number.as_u64(); integer.is_some())
                return JS_NewInt64(ctx, static_cast<std::int64_t>(integer->to_primitive()));
            return JS_NewFloat64(ctx, number.as_f64()->to_primitive());
        }
        RSTD_CASE(String, string) {
            const auto view = rstd::cppstd::as_string_view(string.as_str());
            return JS_NewStringLen(ctx, view.data(), view.size());
        }
        RSTD_CASE(Array, values) {
            JSValue  array = JS_NewArray(ctx);
            uint32_t index = 0;
            for (const auto& item : values) {
                JS_DefinePropertyValueUint32(
                    ctx, array, index++, JsonToJs(ctx, item), JS_PROP_C_W_E);
            }
            return array;
        }
        RSTD_CASE(Object, values) {
            JSValue object = JS_NewObject(ctx);
            values.iter().for_each([&](auto entry) {
                auto [entry_key, entry_value] = entry;
                const auto  key_view          = rstd::cppstd::as_string_view(entry_key->as_str());
                std::string owned_key(key_view);
                JS_DefinePropertyValueStr(
                    ctx, object, owned_key.c_str(), JsonToJs(ctx, *entry_value), JS_PROP_C_W_E);
            });
            return object;
        }
    }
    rstd::unreachable();
}

JSValue UserPropertyValueToJs(JSContext* ctx, const Json& property) {
    if (auto value = property.get("value"_str); value.is_some()) return JsonToJs(ctx, **value);
    return JsonToJs(ctx, property);
}

// Resolve a config value. {"user":"name","value":X} stays as-is — the
// bootstrap getter resolves it lazily against engine.userProperties at
// access time, so SetUserProperty calls after parse propagate.
// Everything else passes through.
JSValue ResolveConfigValue(JSContext* ctx, const Json& v) { return JsonToJs(ctx, v); }

// Coerce a binding's initial-value JSON into the JS shape the script's
// `init(value)` expects, given the bound field kind. Audio-response,
// parallax, and color scripts all assume `value` is already a Vec2/Vec3,
// not a raw string or array.
//   - Numbers: passthrough for scalar; for Vec3 we splat (matching WE's
//     "uniform scale" behaviour observed in the corpus).
//   - Strings: WE serialises vec values as space-separated floats —
//     "1.0 2.0 3.0" → Vec3(1,2,3). Arrays accept the same shape.
//   - Arrays / objects with x,y[,z]: construct a Vec2 / Vec3.
//   - Color: returns an array — most "color" scripts use `[r,g,b]` access.
//
// Falls back to JsonToJs for unknown shapes; better to pass garbage than
// to fail to call init().
JSValue CoerceInitialValue(JSContext* ctx, const Json& v, FieldKind kind) {
    auto parse_floats = [](const std::string& s) -> std::vector<double> {
        std::vector<double> out;
        const char*         p   = s.c_str();
        char*               end = nullptr;
        while (*p) {
            double d = std::strtod(p, &end);
            if (end == p) break;
            out.push_back(d);
            p = end;
            while (*p == ' ' || *p == '\t') ++p;
        }
        return out;
    };
    switch (kind) {
    case FieldKind::Vec2: {
        if (v.is_string()) {
            auto source = rstd::cppstd::to_string(*v.as_str());
            auto fs     = parse_floats(source);
            return MakeVecValue(
                ctx, fs.size() > 0 ? fs[0] : 0.0, fs.size() > 1 ? fs[1] : 0.0, 0.0, 2);
        }
        if (auto values = v.as_array(); values.is_some() && (*values)->len() >= usize(2)) {
            auto x = (**values)[usize(0)].as_f64();
            auto y = (**values)[usize(1)].as_f64();
            if (x.is_some() && y.is_some())
                return MakeVecValue(ctx, x->to_primitive(), y->to_primitive(), 0.0, 2);
        }
        if (v.is_number()) {
            auto number = v.as_f64();
            if (number.is_some()) {
                const auto scalar = number->to_primitive();
                return MakeVecValue(ctx, scalar, scalar, 0.0, 2);
            }
        }
        break;
    }
    case FieldKind::Vec3: {
        if (v.is_string()) {
            auto   source = rstd::cppstd::to_string(*v.as_str());
            auto   fs     = parse_floats(source);
            double x      = fs.size() > 0 ? fs[0] : 0.0;
            double y      = fs.size() > 1 ? fs[1] : x; // splat single scalar
            double z      = fs.size() > 2 ? fs[2] : (fs.size() > 1 ? 0.0 : x);
            return MakeVecValue(ctx, x, y, z, 3);
        }
        if (auto values = v.as_array(); values.is_some() && (*values)->len() >= usize(3)) {
            auto x = (**values)[usize(0)].as_f64();
            auto y = (**values)[usize(1)].as_f64();
            auto z = (**values)[usize(2)].as_f64();
            if (x.is_some() && y.is_some() && z.is_some())
                return MakeVecValue(
                    ctx, x->to_primitive(), y->to_primitive(), z->to_primitive(), 3);
        }
        if (v.is_number()) {
            auto number = v.as_f64();
            if (number.is_some()) {
                const auto scalar = number->to_primitive();
                return MakeVecValue(ctx, scalar, scalar, scalar, 3);
            }
        }
        break;
    }
    case FieldKind::Vec4: {
        if (v.is_string()) {
            auto   source = rstd::cppstd::to_string(*v.as_str());
            auto   fs     = parse_floats(source);
            double x      = fs.size() > 0 ? fs[0] : 0.0;
            double y      = fs.size() > 1 ? fs[1] : x;
            double z      = fs.size() > 2 ? fs[2] : x;
            double w      = fs.size() > 3 ? fs[3] : x;
            return MakeVec4Value(ctx, x, y, z, w);
        }
        if (auto values = v.as_array(); values.is_some() && (*values)->len() >= usize(4)) {
            auto x = (**values)[usize(0)].as_f64();
            auto y = (**values)[usize(1)].as_f64();
            auto z = (**values)[usize(2)].as_f64();
            auto w = (**values)[usize(3)].as_f64();
            if (x.is_some() && y.is_some() && z.is_some() && w.is_some())
                return MakeVec4Value(ctx,
                                     x->to_primitive(),
                                     y->to_primitive(),
                                     z->to_primitive(),
                                     w->to_primitive());
        }
        if (v.is_number()) {
            auto number = v.as_f64();
            if (number.is_some()) {
                const auto scalar = number->to_primitive();
                return MakeVec4Value(ctx, scalar, scalar, scalar, scalar);
            }
        }
        break;
    }
    case FieldKind::Color: {
        if (v.is_string()) {
            auto    source = rstd::cppstd::to_string(*v.as_str());
            auto    fs     = parse_floats(source);
            JSValue arr    = JS_NewArray(ctx);
            for (uint32_t i = 0; i < fs.size() && i < 3; ++i)
                JS_DefinePropertyValueUint32(ctx, arr, i, JS_NewFloat64(ctx, fs[i]), JS_PROP_C_W_E);
            return arr;
        }
        break;
    }
    case FieldKind::Scalar:
    case FieldKind::Bool:
    case FieldKind::String:
    case FieldKind::Unknown: break;
    }
    return JsonToJs(ctx, v);
}

} // namespace

// ---------------------------------------------------------------------------
// FrameInputs storage. JSRuntime opaque data: a per-context FrameInputs
// snapshot the engine.* getters consult on each call.
// ---------------------------------------------------------------------------

// One scheduled engine.setTimeout / setInterval entry. fn is an owned ref;
// dead entries are tombstoned during sweep and compacted afterwards so
// callbacks that schedule more callbacks don't iterate over invalid storage.
struct DeferredCb {
    uint32_t handle;
    double   fire_at;    // engine.runtime seconds when due
    double   interval_s; // for setInterval; 0 for setTimeout
    JSValue  fn;         // owned
    bool     repeating;
    bool     dead;
};

struct AudioBufferSlot {
    uint32_t resolution;
    JSValue  object { JS_UNDEFINED };
};

struct EngineHostState {
    FrameInputs                     inputs;
    MediaStatus                     media;
    bool                            media_initialized { false };
    owe::Scene*                     scene { nullptr };
    rstd::array<AudioBufferSlot, 3> audio_buffers {
        AudioBufferSlot { .resolution = 16 },
        AudioBufferSlot { .resolution = 32 },
        AudioBufferSlot { .resolution = 64 },
    };
    Option<Arc<AudioResponseDemand>>      audio_response_demand;
    Option<Box<dyn<UniformBindingLease>>> audio_response_lease;
    // Cached `globalThis.Vec3` ctor, populated lazily on first node access.
    // Used by the SceneNode wrapper to hand back Vec3 instances so scripts
    // can call `.add` / `.subtract` on `thisLayer.origin`.
    JSValue vec3_ctor { JS_UNDEFINED };
    // The original JS-side `thisLayer` / `thisScene` stubs, captured at
    // bootstrap. Per-script binding restores them when a script has no
    // backing SceneNode.
    JSValue default_layer { JS_UNDEFINED };
    JSValue default_scene { JS_UNDEFINED };
    // engine.setTimeout / setInterval queue. Swept once per frame in
    // JsRuntime::TickAll before the script update loop runs.
    std::vector<DeferredCb> deferred;
    uint32_t                next_handle { 1 };
    // localStorage backing. Values are JSON-serialised strings so we can
    // round-trip arbitrary script values through the persistence file.
    // Empty `ls_path` means in-memory only (the legacy bootstrap shape).
    std::unordered_map<std::string, std::string> ls_data;
    std::string                                  ls_path;
    // Set around every init/update/cursor invocation so host callbacks can
    // resolve the owning field binding.
    FieldScript*                          active_field_script { nullptr };
    Vec<String>                           pending_registered_assets;
    Option<JsRuntime::LayerFactory>       layer_factory;
    Option<JsRuntime::LayerConfigFactory> layer_config_factory;
    // SceneNode -> text-content setter. Populated by text layers in the
    // parser; consulted by NodeSetText so `thisLayer.text = "..."` reaches
    // TextLayouter::SetText. Missing entry means the layer is not text-
    // capable; writes silently no-op.
    std::unordered_map<owe::SceneNode*, std::function<void(std::string_view)>> text_setters;
    struct TextAlignHooks {
        std::string                           horizontal { "center" };
        std::string                           vertical { "center" };
        double                                point_size { 1.0 };
        std::function<void(std::string_view)> set_horizontal;
        std::function<void(std::string_view)> set_vertical;
        std::function<double()>               get_point_size;
        std::function<void(double)>           set_point_size;
    };
    std::unordered_map<owe::SceneNode*, TextAlignHooks> text_align_hooks;
    struct NodeOriginHooks {
        JsRuntime::NodeOriginGetter getter;
        JsRuntime::NodeOriginSetter setter;
    };
    HashMap<owe::SceneNode*, NodeOriginHooks> node_origin_hooks;
    struct ImageAlignmentHook {
        String                          alignment;
        JsRuntime::ImageAlignmentSetter setter;
    };
    HashMap<owe::SceneNode*, ImageAlignmentHook> image_alignment_hooks;
    HashMap<owe::SceneNode*, Json>               initial_layer_configs;
    JsRuntime::BoneIndexResolver                 bone_index_resolver;
    JsRuntime::BoneTransformResolver             bone_transform_resolver;
    owe::SceneNode*                              scene_root { nullptr };
};

uint32_t NormalizeAudioResolution(int32_t requested) {
    if (requested <= 16) return 16;
    if (requested <= 32) return 32;
    return 64;
}

AudioBufferSlot& AudioBufferForResolution(EngineHostState& host, uint32_t resolution) {
    for (auto& slot : host.audio_buffers) {
        if (slot.resolution == resolution) return slot;
    }
    rstd::unreachable();
}

bool HasAudioBuffers(const EngineHostState& host) {
    for (const auto& slot : host.audio_buffers) {
        if (! JS_IsUndefined(slot.object)) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// FieldScript impl.
// ---------------------------------------------------------------------------

struct FieldScript::Impl {
    JsRuntime::Impl* rt { nullptr };
    JSContext*       ctx { nullptr };
    std::string      sha;
    FieldKind        kind { FieldKind::Unknown };
    JSValue          module_ns { JS_UNDEFINED };
    JSValue          init_fn { JS_UNDEFINED };
    JSValue          update_fn { JS_UNDEFINED };
    JSValue          animation_event_fn { JS_UNDEFINED };
    bool             update_takes_arg { false };
    bool             init_done { false };
    std::uint64_t    initialization_order { 0 };
    JSValue          current_value {
        JS_UNDEFINED
    }; // last `value` returned, kept as JSValue for the (value)-arg form
    ScriptValue last_value;
    bool        alive { true };
    bool        error_logged { false };
    // Layer-B: the SceneNode this script's `thisLayer` resolves to. Null →
    // fall back to the generic JS stub. `wrapped_layer` caches the JSValue
    // wrapper so per-frame swap doesn't reallocate.
    owe::SceneNode* node { nullptr };
    JSValue         wrapped_layer { JS_UNDEFINED };
    // Per-script cursor-inside-bbox state used to edge-detect
    // cursorEnter / cursorLeave between frames.
    bool cursor_inside { false };
    // Timelines whose markers this script's `animationEvent` reacts to.
    std::vector<owe::SceneAnimationEventCursor>                   animation_cursors;
    std::unordered_map<std::string, std::vector<owe::SceneNode*>> asset_clone_queues;
    std::unordered_map<owe::SceneNode*, std::string>              clone_asset_keys;
    Vec<String>                                                   registered_assets;
    String                                                        workshop_id;
};

FieldScript::FieldScript(): m_impl(std::make_unique<Impl>()) {}
FieldScript::~FieldScript() = default;
FieldKind          FieldScript::field_kind() const noexcept { return m_impl->kind; }
const ScriptValue& FieldScript::last_value() const noexcept { return m_impl->last_value; }
bool               FieldScript::alive() const noexcept { return m_impl->alive; }
std::string_view   FieldScript::script_sha() const noexcept { return m_impl->sha; }
slice<String>      FieldScript::RegisteredAssets() const noexcept {
    return m_impl->registered_assets.as_slice();
}
Option<ref<str>> FieldScript::WorkshopId() const noexcept {
    if (m_impl->workshop_id.is_empty()) return None();
    return Some(m_impl->workshop_id.as_str());
}

// ---------------------------------------------------------------------------
// JsRuntime impl.
// ---------------------------------------------------------------------------

struct JsRuntime::Impl {
    JSRuntime*      rt { nullptr };
    JSContext*      ctx { nullptr };
    EngineHostState host;
    // Compiled-module dedup: same script source under the same sha is
    // imported once per runtime, exposing one shared namespace. A
    // FieldScript holds a JS_DupValue of the namespace.
    std::unordered_map<std::string, JSValue>  ns_by_sha;
    std::uint64_t                             next_module_serial { 0 };
    std::vector<std::unique_ptr<FieldScript>> scripts;
    // Set of error-logged shas to log once.
    std::unordered_set<std::string> errored;
    // Scene root for `thisScene`. Wrapped lazily; freed in dtor.
    owe::SceneNode* scene_root { nullptr };
    JSValue         wrapped_scene { JS_UNDEFINED };

    void LogError(JSContext* c, std::string_view sha, const char* what) {
        if (errored.contains(std::string(sha))) return;
        errored.insert(std::string(sha));
        JSValue     exc = JS_GetException(c);
        const char* msg = JS_ToCString(c, exc);
        rstd_error("script[{}] {}: {}",
                   sha,
                   std::string_view(what),
                   std::string_view(msg ? msg : "<no message>"));
        if (msg) JS_FreeCString(c, msg);
        JSValue stack = JS_GetPropertyStr(c, exc, "stack");
        if (! JS_IsUndefined(stack) && ! JS_IsNull(stack)) {
            const char* stack_msg = JS_ToCString(c, stack);
            if (stack_msg && stack_msg[0] != '\0') {
                rstd_error("script[{}] stack:\n{}", sha, std::string_view(stack_msg));
            }
            if (stack_msg) JS_FreeCString(c, stack_msg);
        }
        JS_FreeValue(c, stack);
        JS_FreeValue(c, exc);
    }
};

// --- engine.* getters --------------------------------------------------------

namespace
{

JSValue MakeVec2Value(JSContext* ctx, double x, double y) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ctor   = JS_GetPropertyStr(ctx, global, "Vec2");
    JS_FreeValue(ctx, global);
    if (! JS_IsFunction(ctx, ctor)) {
        JS_FreeValue(ctx, ctor);
        JSValue v = JS_NewObject(ctx);
        JS_DefinePropertyValueStr(ctx, v, "x", JS_NewFloat64(ctx, x), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, v, "y", JS_NewFloat64(ctx, y), JS_PROP_C_W_E);
        return v;
    }
    JSValue args[2] { JS_NewFloat64(ctx, x), JS_NewFloat64(ctx, y) };
    JSValue out = JS_CallConstructor(ctx, ctor, 2, args);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    JS_FreeValue(ctx, ctor);
    return out;
}

JSValue EngineGetterFrametime(JSContext* ctx, JSValueConst /*this_val*/, int /*argc*/,
                              JSValueConst* /*argv*/) {
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    return JS_NewFloat64(ctx, host->inputs.frametime);
}
JSValue EngineGetterRuntime(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    return JS_NewFloat64(ctx, host->inputs.runtime);
}
JSValue EngineGetterTimeOfDay(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    return JS_NewFloat64(ctx, host->inputs.time_of_day);
}
JSValue EngineGetterCanvasSize(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    return MakeVec2Value(ctx, host->inputs.canvas_w, host->inputs.canvas_h);
}
JSValue EngineGetterScreenRes(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    return MakeVec2Value(ctx, host->inputs.screen_w, host->inputs.screen_h);
}

void SetAudioArrayValue(JSContext* ctx, JSValueConst arr, uint32_t index, float value) {
    JS_DefinePropertyValueUint32(ctx, arr, index, JS_NewFloat64(ctx, value), JS_PROP_C_W_E);
}

JSValue MakeAudioBuffer(JSContext* ctx, const FrameInputs& inputs, uint32_t resolution) {
    JSValue object  = JS_NewObject(ctx);
    JSValue left    = JS_NewArray(ctx);
    JSValue right   = JS_NewArray(ctx);
    JSValue average = JS_NewArray(ctx);
    JSValue buffer  = JS_NewArray(ctx);
    for (uint32_t index = 0; index < resolution; ++index) {
        const float l = inputs.audio.value(scene_audio::Channel::Left, resolution, index);
        const float r = inputs.audio.value(scene_audio::Channel::Right, resolution, index);
        const float a = inputs.audio.value(scene_audio::Channel::Average, resolution, index);
        SetAudioArrayValue(ctx, left, index, l);
        SetAudioArrayValue(ctx, right, index, r);
        SetAudioArrayValue(ctx, average, index, a);
        SetAudioArrayValue(ctx, buffer, index, a);
    }
    JS_DefinePropertyValueStr(ctx, object, "left", left, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, object, "right", right, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, object, "average", average, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, object, "buffer", buffer, JS_PROP_C_W_E);
    return object;
}

void RefreshAudioBuffer(JSContext* ctx, const FrameInputs& inputs, const AudioBufferSlot& slot) {
    JSValue left    = JS_GetPropertyStr(ctx, slot.object, "left");
    JSValue right   = JS_GetPropertyStr(ctx, slot.object, "right");
    JSValue average = JS_GetPropertyStr(ctx, slot.object, "average");
    JSValue buffer  = JS_GetPropertyStr(ctx, slot.object, "buffer");
    for (uint32_t index = 0; index < slot.resolution; ++index) {
        const float l = inputs.audio.value(scene_audio::Channel::Left, slot.resolution, index);
        const float r = inputs.audio.value(scene_audio::Channel::Right, slot.resolution, index);
        const float a = inputs.audio.value(scene_audio::Channel::Average, slot.resolution, index);
        SetAudioArrayValue(ctx, left, index, l);
        SetAudioArrayValue(ctx, right, index, r);
        SetAudioArrayValue(ctx, average, index, a);
        SetAudioArrayValue(ctx, buffer, index, a);
    }
    JS_FreeValue(ctx, left);
    JS_FreeValue(ctx, right);
    JS_FreeValue(ctx, average);
    JS_FreeValue(ctx, buffer);
}

// engine.registerAudioBuffers(resolution) → { left, right, average, buffer }
JSValue EngineRegisterAudioBuffers(JSContext* ctx, JSValueConst /*this_val*/, int argc,
                                   JSValueConst* argv) {
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    if (host->audio_response_demand.is_some() && host->audio_response_lease.is_none()) {
        host->audio_response_lease = Some((*host->audio_response_demand)->Acquire());
    }
    int32_t requested = 64;
    if (argc > 0) (void)JS_ToInt32(ctx, &requested, argv[0]);
    auto& slot = AudioBufferForResolution(*host, NormalizeAudioResolution(requested));
    if (JS_IsUndefined(slot.object)) {
        slot.object = MakeAudioBuffer(ctx, host->inputs, slot.resolution);
    }
    return JS_DupValue(ctx, slot.object);
}

void RefreshAudioBuffers(JSContext* ctx, EngineHostState& host) {
    for (const auto& slot : host.audio_buffers) {
        if (! JS_IsUndefined(slot.object)) RefreshAudioBuffer(ctx, host.inputs, slot);
    }
}

// Cancel CFunction returned by setTimeout / setInterval. data[0] holds the
// deferred handle ID; invoking it tombstones the corresponding entry. The
// corpus uses both `clearTimeout(handle)` and `handle()` self-cancel forms,
// so handle is itself a callable.
JSValue EngineCancelDeferred(JSContext* ctx, JSValueConst /*this_val*/, int /*argc*/,
                             JSValueConst* /*argv*/, int /*magic*/, JSValue* data) {
    auto*   host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    int32_t h    = 0;
    JS_ToInt32(ctx, &h, data[0]);
    for (auto& d : host->deferred) {
        if (d.handle == uint32_t(h)) {
            d.dead = true;
            break;
        }
    }
    return JS_UNDEFINED;
}

JSValue MakeCancelFn(JSContext* ctx, uint32_t handle) {
    JSValue data[1] = { JS_NewInt32(ctx, int32_t(handle)) };
    JSValue fn      = JS_NewCFunctionData(ctx,
                                          EngineCancelDeferred,
                                          /*length=*/0,
                                          /*magic=*/0,
                                          /*data_len=*/1,
                                          data);
    JS_FreeValue(ctx, data[0]);
    return fn;
}

JSValue EngineSetTimerImpl(JSContext* ctx, int argc, JSValueConst* argv, bool repeating) {
    if (argc < 1 || ! JS_IsFunction(ctx, argv[0])) return JS_UNDEFINED;
    auto*  host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    double ms   = 0.0;
    if (argc >= 2) JS_ToFloat64(ctx, &ms, argv[1]);
    double   interval_s = ms / 1000.0;
    uint32_t h          = host->next_handle++;
    host->deferred.push_back(DeferredCb {
        .handle     = h,
        .fire_at    = host->inputs.runtime + interval_s,
        .interval_s = interval_s,
        .fn         = JS_DupValue(ctx, argv[0]),
        .repeating  = repeating,
        .dead       = false,
    });
    return MakeCancelFn(ctx, h);
}

JSValue EngineSetTimeout(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    return EngineSetTimerImpl(ctx, argc, argv, false);
}
JSValue EngineSetInterval(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    return EngineSetTimerImpl(ctx, argc, argv, true);
}

// Accepts either the cancel function (calls it) or any other value (ignored
// — old corpus shape sometimes hardcodes -1 from the previous noop).
JSValue EngineClearDeferred(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc == 0) return JS_UNDEFINED;
    if (JS_IsFunction(ctx, argv[0])) {
        JSValue r = JS_Call(ctx, argv[0], JS_UNDEFINED, 0, nullptr);
        JS_FreeValue(ctx, r);
    }
    return JS_UNDEFINED;
}

// --- localStorage backing ---------------------------------------------------
// Three CFunctions wired onto globalThis.localStorage. Values round-trip
// through JSON.stringify/parse: scripts get JSON-restorable types only
// (primitives, plain objects, arrays). Vec3 instances become plain
// {x,y,z} objects without `.add` / `.subtract` methods — corpus scripts
// that round-trip Vec3 through localStorage must re-wrap; none observed
// in the surveyed corpus.
//
// Writes flush to disk synchronously when a persistence path is set.
// The file format is a flat JSON object: {"k1": "json string", ...}.

namespace
{
struct PersistedLocalStorage {};
} // namespace

void FlushLocalStorage(EngineHostState* host) {
    if (host->ls_path.empty()) return;
    auto object = rstd::json::Map::make();
    for (const auto& [k, v] : host->ls_data)
        object.insert(::alloc::string::String::make(rstd::cppstd::as_str(k).unwrap()),
                      JsonFromStd(v));
    auto out = Json::Object(rstd::move(object));
    // ofstream defaults to ios_base::out | trunc, which is what we want.
    std::ofstream f(host->ls_path);
    if (! f) {
        rstd_warn("localStorage flush: cannot open {}", host->ls_path);
        return;
    }
    f << Dump(out);
}

void LoadLocalStorage(EngineHostState* host) {
    host->ls_data.clear();
    if (host->ls_path.empty()) return;
    std::ifstream f(host->ls_path);
    if (! f) return;
    std::string source(std::istreambuf_iterator<char>(f), {});
    auto        parsed = ParseJson(source);
    if (parsed.is_err()) {
        rstd_warn("localStorage parse failed: {}", parsed.unwrap_err());
        return;
    }
    auto doc    = parsed.unwrap();
    auto object = doc.as_object();
    if (object.is_none()) return;
    (*object)->iter().for_each([&](auto entry) {
        auto [entry_key, entry_value] = entry;
        const auto  key               = rstd::cppstd::as_string_view(entry_key->as_str());
        const auto& value             = *entry_value;
        if (auto stored = value.as_str(); stored.is_some())
            host->ls_data[std::string(key)] = rstd::cppstd::to_string(*stored);
    });
}

JSValue LocalStorageGet(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    const char* key = JS_ToCString(ctx, argv[0]);
    if (! key) return JS_UNDEFINED;
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    auto  it   = host->ls_data.find(key);
    JS_FreeCString(ctx, key);
    if (it == host->ls_data.end()) return JS_UNDEFINED;
    const auto& s = it->second;
    return JS_ParseJSON(ctx, s.data(), s.size(), "<localStorage>");
}

JSValue LocalStorageSet(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_UNDEFINED;
    const char* key = JS_ToCString(ctx, argv[0]);
    if (! key) return JS_UNDEFINED;
    JSValue jv = JS_JSONStringify(ctx, argv[1], JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(jv) || JS_IsUndefined(jv)) {
        JS_FreeValue(ctx, jv);
        JS_FreeCString(ctx, key);
        return JS_UNDEFINED;
    }
    const char* s    = JS_ToCString(ctx, jv);
    auto*       host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    if (s) host->ls_data[key] = s;
    if (s) JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, jv);
    JS_FreeCString(ctx, key);
    FlushLocalStorage(host);
    return JS_UNDEFINED;
}

JSValue LocalStorageRemove(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    const char* key = JS_ToCString(ctx, argv[0]);
    if (! key) return JS_UNDEFINED;
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    host->ls_data.erase(key);
    JS_FreeCString(ctx, key);
    FlushLocalStorage(host);
    return JS_UNDEFINED;
}

void InstallLocalStorage(JSContext* ctx) {
    JSValue g  = JS_GetGlobalObject(ctx);
    JSValue ls = JS_NewObject(ctx);
    JS_DefinePropertyValueStr(
        ctx, ls, "get", JS_NewCFunction(ctx, LocalStorageGet, "get", 1), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(
        ctx, ls, "set", JS_NewCFunction(ctx, LocalStorageSet, "set", 2), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(
        ctx, ls, "remove", JS_NewCFunction(ctx, LocalStorageRemove, "remove", 1), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, g, "localStorage", ls, JS_PROP_C_W_E);
    JS_FreeValue(ctx, g);
}

// Project normalised canvas coordinates into the scene's world units.
// Hosts feed pointer Y top-down; the scene world uses Y-up, matching
// link_mouse particles and SceneCamera's orthographic viewport.
struct CursorWorld {
    double x { 0 }, y { 0 };
};

CursorWorld CursorToWorld(const FrameInputs& fi) {
    return CursorWorld {
        .x = double(fi.cursor_x) * double(fi.canvas_w),
        .y = (1.0 - double(fi.cursor_y)) * double(fi.canvas_h),
    };
}

void SetObjectNumber(JSContext* ctx, JSValueConst obj, const char* name, double value) {
    JS_SetPropertyStr(ctx, obj, name, JS_NewFloat64(ctx, value));
}

void SetVec2Fields(JSContext* ctx, JSValueConst obj, double x, double y) {
    SetObjectNumber(ctx, obj, "x", x);
    SetObjectNumber(ctx, obj, "y", y);
}

void SetVec3Fields(JSContext* ctx, JSValueConst obj, double x, double y, double z) {
    SetObjectNumber(ctx, obj, "x", x);
    SetObjectNumber(ctx, obj, "y", y);
    SetObjectNumber(ctx, obj, "z", z);
}

void UpdateInputObject(JSContext* ctx) {
    auto*       host  = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    const auto& fi    = host->inputs;
    JSValue     g     = JS_GetGlobalObject(ctx);
    JSValue     input = JS_GetPropertyStr(ctx, g, "input");
    if (! JS_IsObject(input)) {
        JS_FreeValue(ctx, input);
        JS_FreeValue(ctx, g);
        return;
    }

    JSValue screen = JS_GetPropertyStr(ctx, input, "cursorScreenPosition");
    if (JS_IsObject(screen))
        SetVec2Fields(ctx, screen, fi.cursor_x * fi.screen_w, fi.cursor_y * fi.screen_h);
    JS_FreeValue(ctx, screen);

    const CursorWorld world = CursorToWorld(fi);
    JSValue           wp    = JS_GetPropertyStr(ctx, input, "cursorWorldPosition");
    if (JS_IsObject(wp)) SetVec3Fields(ctx, wp, world.x, world.y, 0.0);
    JS_FreeValue(ctx, wp);

    JSValue lp = JS_GetPropertyStr(ctx, input, "cursorLocalPosition");
    if (JS_IsObject(lp)) SetVec3Fields(ctx, lp, world.x, world.y, 0.0);
    JS_FreeValue(ctx, lp);

    JS_SetPropertyStr(ctx, input, "mouseButtonsDown", JS_NewUint32(ctx, fi.mouse_buttons_down));
    JS_SetPropertyStr(
        ctx, input, "mouseButtonsPressed", JS_NewUint32(ctx, fi.mouse_buttons_pressed));
    JS_SetPropertyStr(
        ctx, input, "mouseButtonsReleased", JS_NewUint32(ctx, fi.mouse_buttons_released));
    JS_SetPropertyStr(
        ctx, input, "cursorLeftDown", JS_NewBool(ctx, (fi.mouse_buttons_down & (1u << 0)) != 0));
    JS_SetPropertyStr(
        ctx, input, "cursorRightDown", JS_NewBool(ctx, (fi.mouse_buttons_down & (1u << 1)) != 0));
    JS_SetPropertyStr(
        ctx, input, "cursorMiddleDown", JS_NewBool(ctx, (fi.mouse_buttons_down & (1u << 2)) != 0));
    JS_SetPropertyStr(ctx, input, "inWindow", JS_NewBool(ctx, fi.cursor_in_window));

    JS_FreeValue(ctx, input);
    JS_FreeValue(ctx, g);
}

bool HitTestNode(owe::SceneNode* n, const CursorWorld& c) {
    if (! n) return false;
    n->UpdateTrans();
    Eigen::Matrix4d m  = n->ModelTrans() * n->GeometryTransform();
    Eigen::Vector2f sz = n->Size();
    if (sz.x() == 0.0f && sz.y() == 0.0f) sz = Eigen::Vector2f { 100.0f, 100.0f };
    double          hx = sz.x() * 0.5, hy = sz.y() * 0.5;
    Eigen::Vector4d corners[4] = {
        { -hx, -hy, 0, 1 },
        { hx, -hy, 0, 1 },
        { hx, hy, 0, 1 },
        { -hx, hy, 0, 1 },
    };
    double minx = 1e30, miny = 1e30, maxx = -1e30, maxy = -1e30;
    for (auto& corner : corners) {
        Eigen::Vector4d w = m * corner;
        minx              = std::min(minx, w.x());
        maxx              = std::max(maxx, w.x());
        miny              = std::min(miny, w.y());
        maxy              = std::max(maxy, w.y());
    }
    return c.x >= minx && c.x <= maxx && c.y >= miny && c.y <= maxy;
}

// Build the event object passed to cursor callbacks. WE scripts read
// event.worldPosition (a Vec2 in scene units) and event.button (0/1/2).
JSValue MakeCursorEvent(JSContext* ctx, const CursorWorld& c, int button) {
    JSValue ev   = JS_NewObject(ctx);
    auto*   host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    JSValue wp;
    if (! JS_IsUndefined(host->vec3_ctor)) {
        JSValue args[3] { JS_NewFloat64(ctx, c.x), JS_NewFloat64(ctx, c.y), JS_NewFloat64(ctx, 0) };
        wp = JS_CallConstructor(ctx, host->vec3_ctor, 3, args);
        for (auto& a : args) JS_FreeValue(ctx, a);
    } else {
        wp = JS_NewObject(ctx);
        JS_DefinePropertyValueStr(ctx, wp, "x", JS_NewFloat64(ctx, c.x), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, wp, "y", JS_NewFloat64(ctx, c.y), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, wp, "z", JS_NewFloat64(ctx, 0), JS_PROP_C_W_E);
    }
    JS_DefinePropertyValueStr(ctx, ev, "worldPosition", wp, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, ev, "button", JS_NewInt32(ctx, button), JS_PROP_C_W_E);
    return ev;
}

// Build the event object passed to `animationEvent`. Scripts branch on
// event.name, which is the marker's name as authored on the timeline.
JSValue MakeAnimationEvent(JSContext* ctx, ref<str> name) {
    JSValue ev   = JS_NewObject(ctx);
    auto    text = rstd::cppstd::to_string(name);
    JS_DefinePropertyValueStr(
        ctx, ev, "name", JS_NewStringLen(ctx, text.data(), text.size()), JS_PROP_C_W_E);
    return ev;
}

// Invoke `name` on the script's module namespace if exported, passing one
// event arg. `thisLayer` should already be bound to the script's node by
// the caller. Exceptions are caught and logged once per sha.
void InvokeEventCallback(JSContext* ctx, JSValue ns, const char* name, JSValue ev,
                         JsRuntime::Impl* rt, std::string_view sha) {
    JSValue fn = JS_GetPropertyStr(ctx, ns, name);
    if (JS_IsFunction(ctx, fn)) {
        JSValue arg = JS_DupValue(ctx, ev);
        JSValue r   = JS_Call(ctx, fn, JS_UNDEFINED, 1, &arg);
        JS_FreeValue(ctx, arg);
        if (JS_IsException(r)) {
            rt->LogError(ctx, sha, name);
            JS_FreeValue(ctx, r);
        } else {
            JS_FreeValue(ctx, r);
        }
    }
    JS_FreeValue(ctx, fn);
}

// Fire any deferred callbacks whose fire_at has passed. Called by TickAll
// before the script update loop. Repeating callbacks reschedule against
// their previous fire_at so steady-state drift is bounded.
void SweepDeferred(JSContext* ctx, EngineHostState* host) {
    const double now = host->inputs.runtime;
    // Iterate by index; callbacks may push_back new entries.
    for (size_t i = 0; i < host->deferred.size(); ++i) {
        if (host->deferred[i].dead) continue;
        while (! host->deferred[i].dead && host->deferred[i].fire_at <= now) {
            JSValue fn  = JS_DupValue(ctx, host->deferred[i].fn);
            JSValue ret = JS_Call(ctx, fn, JS_UNDEFINED, 0, nullptr);
            JS_FreeValue(ctx, fn);
            if (JS_IsException(ret)) {
                JSValue     exc = JS_GetException(ctx);
                const char* msg = JS_ToCString(ctx, exc);
                rstd_error("script timer callback threw: {}", msg ? msg : "<no message>");
                if (msg) JS_FreeCString(ctx, msg);
                JS_FreeValue(ctx, exc);
                host->deferred[i].dead = true;
            }
            JS_FreeValue(ctx, ret);
            if (host->deferred[i].dead) break;
            if (! host->deferred[i].repeating) {
                host->deferred[i].dead = true;
                break;
            }
            host->deferred[i].fire_at += host->deferred[i].interval_s;
            // Guard against zero-interval intervals starving the loop.
            if (host->deferred[i].interval_s <= 0.0) {
                host->deferred[i].dead = true;
                break;
            }
        }
    }
    // Compact dead entries.
    for (auto& d : host->deferred) {
        if (d.dead && ! JS_IsUndefined(d.fn)) {
            JS_FreeValue(ctx, d.fn);
            d.fn = JS_UNDEFINED;
        }
    }
    host->deferred.erase(std::remove_if(host->deferred.begin(),
                                        host->deferred.end(),
                                        [](const DeferredCb& d) {
                                            return d.dead;
                                        }),
                         host->deferred.end());
}

JSValue EngineRegisterAsset(JSContext* ctx, JSValueConst /*this_val*/, int argc,
                            JSValueConst* argv) {
    if (argc > 0) {
        const char* value = JS_ToCString(ctx, argv[0]);
        if (value != nullptr) {
            auto* host   = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
            auto  asset  = String::make(rstd::cppstd::as_str(value).unwrap());
            auto& assets = host->active_field_script
                               ? host->active_field_script->m_impl->registered_assets
                               : host->pending_registered_assets;
            bool  found { false };
            for (const auto& registered : assets) found = found || registered == asset;
            if (! found) assets.push(rstd::move(asset));
            JS_FreeCString(ctx, value);
        }
        return JS_DupValue(ctx, argv[0]);
    }
    return JS_NewObject(ctx);
}

// createScriptProperties() — the JS-side declarative builder. We implement
// it as a thin C function that returns an object exposing addX / finish.
// addX records the descriptor on the builder object's `__props` dict; the
// host reads that dict after the module body runs to know what knobs the
// script exposes. finish() returns a Proxy whose property reads return the
// resolved current value (host fills it from scriptproperties config + the
// schema default).
//
// Implementation: we let JS itself build the builder via a small bootstrap
// snippet evaluated once into the global context. That keeps the C side
// minimal and lets the dynamic property lookup use a JS Proxy.
constexpr const char* kBootstrapJs = R"JS(
(() => {
  const nativeExec = RegExp.prototype.exec;
  const defineRegExpStatic = (name, value) => {
    Object.defineProperty(RegExp, name, {
      value,
      writable: true,
      configurable: true,
      enumerable: false,
    });
  };
  for (let i = 1; i <= 9; ++i) defineRegExpStatic('$' + i, '');
  defineRegExpStatic('$_', '');
  defineRegExpStatic('input', '');
  defineRegExpStatic('$&', '');
  defineRegExpStatic('lastMatch', '');
  defineRegExpStatic('$+', '');
  defineRegExpStatic('lastParen', '');
  defineRegExpStatic('$`', '');
  defineRegExpStatic('leftContext', '');
  defineRegExpStatic("$'", '');
  defineRegExpStatic('rightContext', '');

  const updateRegExpStatics = (match) => {
    if (!match) return;
    const input = String(match.input ?? '');
    const index = match.index ?? 0;
    const text = match[0] ?? '';
    for (let i = 1; i <= 9; ++i) RegExp['$' + i] = match[i] ?? '';
    RegExp.$_ = input;
    RegExp.input = input;
    RegExp['$&'] = text;
    RegExp.lastMatch = text;
    RegExp['$+'] = '';
    RegExp.lastParen = '';
    for (let i = match.length - 1; i >= 1; --i) {
      if (match[i] !== undefined) {
        RegExp['$+'] = match[i];
        RegExp.lastParen = match[i];
        break;
      }
    }
    RegExp['$`'] = input.slice(0, index);
    RegExp.leftContext = RegExp['$`'];
    RegExp["$'"] = input.slice(index + text.length);
    RegExp.rightContext = RegExp["$'"];
  };

  RegExp.prototype.exec = function(str) {
    const match = nativeExec.call(this, str);
    updateRegExpStatics(match);
    return match;
  };
  RegExp.prototype.test = function(str) {
    return this.exec(str) !== null;
  };
})();

globalThis.createScriptProperties = function () {
  const _props = [];
  const _byName = new Map();
  const builder = {
    _byName,
    _props,
  };
  const adder = (kind) => (opts) => {
    const d = Object.assign({ kind }, opts);
    _props.push(d);
    if (d && d.name) _byName.set(d.name, d);
    return builder;
  };
  builder.addSlider    = adder('Slider');
  builder.addCheckbox  = adder('Checkbox');
  builder.addText      = adder('Text');
  builder.addCombo     = adder('Combo');
  builder.addColor     = adder('Color');
  builder.addDelimiter = adder('Delimiter');
  // Stubs for the long tail surfaced by wpscriptdump (Animation,
  // Interpolator, AniMapper, Task, ChangedUserProperty, Listener,
  // SpaceToTimeDelimiter, SpaceToDateDelimiter, Value): no-op, returns
  // builder so `.addX().addY().finish()` chains keep parsing.
  for (const k of ['Animation','Interpolator','AniMapper','Task',
                   'ChangedUserProperty','Listener',
                   'SpaceToTimeDelimiter','SpaceToDateDelimiter','Value']) {
    builder['add' + k] = adder(k);
  }
  // .finish() returns a Proxy. Property reads:
  //   - scriptProperties.<name> : look up in _hostValues (filled by C++),
  //                               else default value from descriptor.
  //   When _hostValues[name] is a {user, value} pair, resolve at access
  //   time against engine.userProperties so SetUserProperty calls made
  //   after parse propagate.
  builder.finish = function () {
    const _hostValues = builder._hostValues || {};
    const target = {};
    const applyHostScale = (h, value) => {
      if (h && typeof h === 'object' && typeof h.__scriptValueScale === 'number' &&
          typeof value === 'number') {
        return value * h.__scriptValueScale;
      }
      return value;
    };
    const userValue = (u) => {
      if (typeof u === 'object' && u !== null && 'value' in u) return u.value;
      return u;
    };
    const sameScalar = (a, b) => String(a) === String(b);
    const unwrapUserProp = (h) => {
      if (h === undefined || h === null) return undefined;
      if (typeof h !== 'object' || !('user' in h) || !('value' in h)) return h;
      if (typeof h.user === 'object') {
        const gate = h.user;
        if (gate && typeof gate.name === 'string') {
          const u = engine.userProperties[gate.name];
          if (u !== undefined) return sameScalar(userValue(u), gate.condition);
        }
        return unwrapUserProp(h.value);
      }
      const u = engine.userProperties[h.user];
      if (u !== undefined) {
        // project.json stores user props as { type, value, ... }; pluck
        // .value when present, else use the bare value directly.
        return applyHostScale(h, userValue(u));
      }
      return applyHostScale(h, h.value);
    };
    const coerceDescriptorValue = (descriptor, value) => {
      if (!descriptor || descriptor.kind !== 'Color') return value;
      if (typeof value === 'string') {
        const components = value.trim().split(/\s+/).map(Number);
        return new globalThis.Vec3(components[0] ?? 0, components[1] ?? 0, components[2] ?? 0);
      }
      if (Array.isArray(value)) {
        return new globalThis.Vec3(value[0] ?? 0, value[1] ?? 0, value[2] ?? 0);
      }
      return value;
    };
    // WE substitutes user-prop values verbatim, even when the user's
    // slider range is wider than the script's declared range — corpus
    // wallpapers (e.g. workshop 3327063360) wire `min:-1,max:1` sliders
    // into scripts declaring `min:0,max:1` and rely on the negative
    // values reaching the formula to shift origin off-parent.
    for (const d of _props) {
      if (d && d.name) {
        Object.defineProperty(target, d.name, {
          enumerable: true,
          configurable: true,
          get() {
            if (Object.prototype.hasOwnProperty.call(_hostValues, d.name))
              return coerceDescriptorValue(d, unwrapUserProp(_hostValues[d.name]));
            return d.value;
          },
        });
      }
    }
    target.__descriptors = _props;
    target.__hostValues  = _hostValues;
    return target;
  };
  // Host writes here before evaluating the script body (per FieldScript)
  // to override defaults.
  builder._hostValues = {};
  return builder;
};
// WE editor exposes a real console; renderer scripts that log diagnostics
// touch it from init/update. Provide a no-op shim so unguarded calls
// don't throw ReferenceError mid module-body — that would leave the
// remaining const/let declarations in TDZ and break unrelated callbacks.
if (! globalThis.console) {
    const __noop = function() {};
    globalThis.console = {
        log: __noop, info: __noop, warn: __noop, error: __noop,
        debug: __noop, trace: __noop, dir: __noop, assert: __noop,
        group: __noop, groupCollapsed: __noop, groupEnd: __noop,
    };
}

// engine.userProperties is a plain object the host can mutate.
if (! globalThis.engine) globalThis.engine = {};
globalThis.engine.userProperties = {};
globalThis.engine.AUDIO_RESOLUTION_16 = 16;
globalThis.engine.AUDIO_RESOLUTION_32 = 32;
globalThis.engine.AUDIO_RESOLUTION_64 = 64;
// WE exposes these as zero-arg query functions; some scripts call them
// (`engine.isRunningInEditor()`), others read as boolean. Provide a
// callable that also coerces to false when accessed as a value (the
// function object is truthy, but scripts that use `if (engine.isRunningInEditor)`
// still see truthy → they branch into the "running in editor" path. The
// corpus only ever calls it, so callable-form is the safer default).
globalThis.engine.isRunningInEditor = function() { return false; };
globalThis.engine.isScreensaver     = function() { return false; };

// --- Vec2 / Vec3 / Vec4 ---
// Pure-JS implementations of WE's vector types. The corpus relies on
// .multiply / .add / .subtract / .divide as Vec3 instance methods (used
// by every audio-response script binding scale), so a simple class with
// these methods covers the audio-responsive cluster (1023 instances).
class Vec2 {
  constructor(x, y) {
    if (typeof x === 'object' && x !== null) {
      this.x = x.x ?? 0; this.y = x.y ?? 0; return;
    }
    // Single-number arg splats to both components (WE convention,
    // e.g. `new Vec2(0.5)` => Vec2(0.5, 0.5)).
    if (typeof x === 'number' && y === undefined) { this.x = x; this.y = x; return; }
    this.x = (typeof x === 'number') ? x : 0;
    this.y = (typeof y === 'number') ? y : 0;
  }
  add(o)      { return new Vec2(this.x + (o.x ?? o), this.y + (o.y ?? o)); }
  subtract(o) { return new Vec2(this.x - (o.x ?? o), this.y - (o.y ?? o)); }
  multiply(o) { return new Vec2(this.x * (o.x ?? o), this.y * (o.y ?? o)); }
  divide(o)   { return new Vec2(this.x / (o.x ?? o), this.y / (o.y ?? o)); }
  mix(o, t)    {
    return new Vec2(
      this.x + ((o.x ?? o) - this.x) * t,
      this.y + ((o.y ?? o) - this.y) * t);
  }
  copy()      { return new Vec2(this.x, this.y); }
  clone()     { return new Vec2(this.x, this.y); }
  toString()  { return `${this.x} ${this.y}`; }
  length()    { return Math.sqrt(this.x*this.x + this.y*this.y); }
  lengthSqr() { return this.x*this.x + this.y*this.y; }
  normalize() {
    const len = this.length();
    return len > 0 ? this.divide(len) : new Vec2(0, 0);
  }
}
class Vec3 {
  constructor(x, y, z) {
    if (typeof x === 'object' && x !== null) {
      this.x = x.x ?? 0; this.y = x.y ?? 0; this.z = x.z ?? 0;
    } else if (typeof x === 'number' && y === undefined && z === undefined) {
      // Single-number arg splats to all three components (WE convention,
      // e.g. `new Vec3(scriptProperties.barWidth)` => Vec3(5,5,5)).
      this.x = x; this.y = x; this.z = x;
    } else {
      this.x = (typeof x === 'number') ? x : 0;
      this.y = (typeof y === 'number') ? y : 0;
      this.z = (typeof z === 'number') ? z : 0;
    }
  }
  add(o)      { return new Vec3(this.x + (o.x ?? o), this.y + (o.y ?? o), this.z + (o.z ?? o)); }
  subtract(o) { return new Vec3(this.x - (o.x ?? o), this.y - (o.y ?? o), this.z - (o.z ?? o)); }
  multiply(o) { return new Vec3(this.x * (o.x ?? o), this.y * (o.y ?? o), this.z * (o.z ?? o)); }
  divide(o)   { return new Vec3(this.x / (o.x ?? o), this.y / (o.y ?? o), this.z / (o.z ?? o)); }
  mix(o, t)    {
    return new Vec3(
      this.x + ((o.x ?? o) - this.x) * t,
      this.y + ((o.y ?? o) - this.y) * t,
      this.z + ((o.z ?? o) - this.z) * t);
  }
  copy()      { return new Vec3(this.x, this.y, this.z); }
  clone()     { return new Vec3(this.x, this.y, this.z); }
  toString()  { return `${this.x} ${this.y} ${this.z}`; }
  length()    { return Math.sqrt(this.x*this.x + this.y*this.y + this.z*this.z); }
  lengthSqr() { return this.x*this.x + this.y*this.y + this.z*this.z; }
  normalize() {
    const len = this.length();
    return len > 0 ? this.divide(len) : new Vec3(0, 0, 0);
  }
}
class Vec4 {
  constructor(x, y, z, w) {
    if (typeof x === 'object' && x !== null) {
      this.x = x.x ?? 0; this.y = x.y ?? 0; this.z = x.z ?? 0; this.w = x.w ?? 0;
    } else if (typeof x === 'number' && y === undefined && z === undefined && w === undefined) {
      this.x = x; this.y = x; this.z = x; this.w = x;
    } else {
      this.x = (typeof x === 'number') ? x : 0;
      this.y = (typeof y === 'number') ? y : 0;
      this.z = (typeof z === 'number') ? z : 0;
      this.w = (typeof w === 'number') ? w : 0;
    }
  }
  add(o)      { return new Vec4(this.x + (o.x ?? o), this.y + (o.y ?? o), this.z + (o.z ?? o), this.w + (o.w ?? o)); }
  subtract(o) { return new Vec4(this.x - (o.x ?? o), this.y - (o.y ?? o), this.z - (o.z ?? o), this.w - (o.w ?? o)); }
  multiply(o) { return new Vec4(this.x * (o.x ?? o), this.y * (o.y ?? o), this.z * (o.z ?? o), this.w * (o.w ?? o)); }
  divide(o)   { return new Vec4(this.x / (o.x ?? o), this.y / (o.y ?? o), this.z / (o.z ?? o), this.w / (o.w ?? o)); }
  mix(o, t)   {
    return new Vec4(
      this.x + ((o.x ?? o) - this.x) * t,
      this.y + ((o.y ?? o) - this.y) * t,
      this.z + ((o.z ?? o) - this.z) * t,
      this.w + ((o.w ?? o) - this.w) * t);
  }
  copy()      { return new Vec4(this.x, this.y, this.z, this.w); }
  clone()     { return new Vec4(this.x, this.y, this.z, this.w); }
  toString()  { return `${this.x} ${this.y} ${this.z} ${this.w}`; }
  length()    { return Math.sqrt(this.x*this.x + this.y*this.y + this.z*this.z + this.w*this.w); }
  lengthSqr() { return this.x*this.x + this.y*this.y + this.z*this.z + this.w*this.w; }
  normalize() {
    const len = this.length();
    return len > 0 ? this.divide(len) : new Vec4(0, 0, 0, 0);
  }
}
globalThis.Vec2 = Vec2;
globalThis.Vec3 = Vec3;
globalThis.Vec4 = Vec4;
globalThis.__wwSerializeLayerConfig = function(config) {
  return JSON.stringify(config, function(_key, value) {
    if (value instanceof Vec2 || value instanceof Vec3 || value instanceof Vec4)
      return value.toString();
    return value;
  });
};

// --- thisLayer / thisScene stub ---------------------------------------------
// Stand-in for the per-script SceneNode binding. Property reads return
// sensible defaults; writes are silently accepted. getTransformMatrix returns
// a shaped value so matrix accesses don't TypeError.
function __wwCreateNodeStub() {
    const props = {
        origin:         new Vec3(0, 0, 0),
        scale:          new Vec3(1, 1, 1),
        angles:         new Vec3(0, 0, 0),
        size:           new Vec3(100, 100, 0),
        perspective:    false,
        visible:        true,
        verticalalign:  'center',
        horizontalalign:'center',
        alpha:          1,
        brightness:     1,
        color:          new Vec3(1, 1, 1),
    };
    // Identity 4x4 column-major matrix. m[13] is the y-translation slot
    // some clock scripts read.
    const identity = [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1];
    const handler = {
        get(target, key) {
            if (key === 'getParent')           return () => undefined;
            if (key === 'getTransformMatrix')  return () => ({ m: identity.slice() });
            if (key === 'getChildren')         return () => [];
            if (key === 'getName')             return () => '';
            if (key === 'getLayer')            return (_n) => __wwCreateNodeStub();
            if (key === 'getEffect')           return (_n) => __wwCreateEffectStub();
            if (key === 'getTextureAnimation') return () => __wwCreateTexAnimStub();
            if (key === 'getVideoTexture')     return () => __wwCreateVideoTextureStub();
            if (key === 'getAnimation')        return ()   => __wwCreateAnimationStub();
            if (key === 'getAnimationLayer')   return (_n) => __wwCreateAnimationStub();
            if (key === 'destroyLayer')        return (_layer) => undefined;
            if (key in target) return target[key];
            return undefined;
        },
        set(target, key, value) { target[key] = value; return true; },
        has(target, key) { return key in target; },
    };
    return new Proxy(props, handler);
}

function __wwCreateEffectStub() {
    return { visible: true };
}

function __wwCreateTexAnimStub() {
    let frame = 0, playing = false;
    return {
        play()     { playing = true;  },
        stop()     { playing = false; },
        pause()    { playing = false; },
        setFrame(n){ frame = n | 0;   },
        getFrame() { return frame;    },
        isPlaying(){ return playing;  },
    };
}

function __wwCreateVideoTextureStub() {
    let current = 0, playing = false;
    return {
        duration: 0,
        rate: 1,
        volume: 1,
        play()     { playing = true;  },
        stop()     { playing = false; current = 0; },
        pause()    { playing = false; },
        setCurrentTime(t) {
            t = Number(t);
            current = Number.isFinite(t) && t > 0 ? t : 0;
        },
        getCurrentTime() { return current; },
        isPlaying()     { return playing; },
    };
}

// Sprite-image / puppet-bone animation handle. Scripts commonly adjust
// playback rate and manually drive the current frame from init/update.
function __wwCreateAnimationStub() {
    let frame = 0, playing = false;
    const o = {
        rate: 1,
        frameCount: 1,
        play()     { playing = true;  },
        stop()     { playing = false; },
        pause()    { playing = false; },
        setFrame(n){ frame = Math.max(0, n | 0); },
        getFrame() { return frame;    },
        isPlaying(){ return playing;  },
    };
    return o;
}
globalThis.__wwCreateAnimationStub = __wwCreateAnimationStub;
globalThis.__wwCreateVideoTextureStub = __wwCreateVideoTextureStub;
globalThis.thisLayer = __wwCreateNodeStub();
globalThis.thisObject = globalThis.thisLayer;
globalThis.thisScene = __wwCreateNodeStub();

// `input` is the WE-global cursor / input state. Scripts often guard with
// `if (input && input.cursorWorldPosition)` so a populated stub is fine;
// values stay at zero until the host wires real cursor data.
globalThis.input = {
    cursorWorldPosition:  new Vec3(0, 0, 0),
    cursorLocalPosition:  new Vec3(0, 0, 0),
    cursorScreenPosition: new Vec2(0, 0),
    mouseButtonsDown:     0,
    mouseButtonsPressed:  0,
    mouseButtonsReleased: 0,
    cursorLeftDown:       false,
    cursorRightDown:      false,
    cursorMiddleDown:     false,
    inWindow:             false,
};

// Hook used by the C++ side to swap the stub for a real per-script binding.
globalThis.__wwBindLayer = function(obj) { globalThis.thisLayer = obj; globalThis.thisObject = obj; };
globalThis.__wwBindScene = function(obj) { globalThis.thisScene = obj; };

// --- MediaPlaybackEvent enum ------------------------------------------------
globalThis.MediaPlaybackEvent = Object.freeze({
    PLAYBACK_STOPPED: 0,
    PLAYBACK_PLAYING: 1,
    PLAYBACK_PAUSED:  2,
});

// --- shared --- cross-script object scripts mutate freely.
if (! globalThis.shared) globalThis.shared = {};

// localStorage is installed from C++ in InstallEngineGlobal so it can
// optionally persist to a JSON file under cache_path keyed by scene_id.
)JS";

void InstallEngineGlobal(JSContext* ctx) {
    // Run the bootstrap to create createScriptProperties + skeleton engine.
    JSValue r = JS_Eval(
        ctx, kBootstrapJs, std::strlen(kBootstrapJs), "<wescene-bootstrap>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        JSValue     exc = JS_GetException(ctx);
        const char* msg = JS_ToCString(ctx, exc);
        rstd_error("script bootstrap: {}", msg ? msg : "<exc>");
        if (msg) JS_FreeCString(ctx, msg);
        JS_FreeValue(ctx, exc);
    }
    JS_FreeValue(ctx, r);

    // Install the dynamic getters on engine.{frametime,runtime,timeOfDay,
    // canvasSize,screenResolution} via accessor properties so reads see
    // the latest FrameInputs.
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue engine = JS_GetPropertyStr(ctx, global, "engine");

    auto define_getter = [&](const char* name, JSCFunction* f) {
        JSAtom  atom = JS_NewAtom(ctx, name);
        JSValue gfun = JS_NewCFunction(ctx, f, name, 0);
        JS_DefinePropertyGetSet(
            ctx, engine, atom, gfun, JS_UNDEFINED, JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx, atom);
    };
    define_getter("frametime", EngineGetterFrametime);
    define_getter("runtime", EngineGetterRuntime);
    define_getter("timeOfDay", EngineGetterTimeOfDay);
    define_getter("canvasSize", EngineGetterCanvasSize);
    define_getter("screenResolution", EngineGetterScreenRes);

    auto define_fn = [&](const char* name, JSCFunction* f, int nargs) {
        JS_DefinePropertyValueStr(
            ctx, engine, name, JS_NewCFunction(ctx, f, name, nargs), JS_PROP_C_W_E);
    };
    define_fn("registerAudioBuffers", EngineRegisterAudioBuffers, 1);
    define_fn("setTimeout", EngineSetTimeout, 2);
    define_fn("setInterval", EngineSetInterval, 2);
    define_fn("clearTimeout", EngineClearDeferred, 1);
    define_fn("clearInterval", EngineClearDeferred, 1);
    define_fn("registerAsset", EngineRegisterAsset, 1);

    // A handful of corpus scripts call setTimeout/clearTimeout bare (no
    // `engine.` prefix). Mirror onto globalThis so they resolve.
    auto alias_to_global = [&](const char* name) {
        JSValue f = JS_GetPropertyStr(ctx, engine, name);
        JS_DefinePropertyValueStr(ctx, global, name, f, JS_PROP_C_W_E);
    };
    alias_to_global("setTimeout");
    alias_to_global("setInterval");
    alias_to_global("clearTimeout");
    alias_to_global("clearInterval");

    JS_FreeValue(ctx, engine);
    JS_FreeValue(ctx, global);

    // C++-backed localStorage. Replaces the in-memory JS Map; values
    // persist to a JSON file when JsRuntime::SetPersistence is called.
    InstallLocalStorage(ctx);
}

// --- Built-in ES modules ----------------------------------------------------
// Scripts `import * as M from 'M'`. QuickJS calls our loader with the
// bare name; we return a precompiled JSModuleDef built from the source
// below. Add an entry to extend (e.g. WEEasing) once needed.
struct BuiltinModule {
    const char* name;
    const char* source;
};

static constexpr const char* kWEMathSrc = R"JS(
export function mix(a, b, t) { return a + (b - a) * t; }
export function lerp(a, b, t) { return a + (b - a) * t; }
export function clamp(x, lo, hi) {
    return Math.max(lo, Math.min(hi, x));
}
export function saturate(x) { return Math.max(0, Math.min(1, x)); }
function smoothstep_impl(edge0, edge1, x) {
    const t = Math.max(0, Math.min(1, (x - edge0) / (edge1 - edge0)));
    return t * t * (3 - 2 * t);
}
// Corpus uses both casings; smoothStep (camelCase) is by far the more
// common form (~165 callsites vs lowercase).
export const smoothstep = smoothstep_impl;
export const smoothStep = smoothstep_impl;
export function step(edge, x) { return x < edge ? 0 : 1; }
export function sign(x) { return Math.sign(x); }
export function fract(x) { return x - Math.floor(x); }
export function deg2rad(d) { return d * (Math.PI / 180); }
export function rad2deg(r) { return r * (180 / Math.PI); }
deg2rad.valueOf = () => Math.PI / 180;
rad2deg.valueOf = () => 180 / Math.PI;
)JS";

// WE's `WEVector` module. angleVector2 and vectorAngle2 use degrees and the
// standard x = cos, y = sin axes. Other helpers are unambiguous vector math.
static constexpr const char* kWEVectorSrc = R"JS(
function v2(x, y) { return new globalThis.Vec2(x, y); }
export function angleVector2(angle) {
    const r = angle * Math.PI / 180;
    return v2(Math.cos(r), Math.sin(r));
}
export function vectorAngle2(v) {
    return Math.atan2(v.y, v.x) * 180 / Math.PI;
}
export function magnitude(v) { return Math.sqrt(v.x*v.x + v.y*v.y); }
export function normalize(v) {
    const m = Math.sqrt(v.x*v.x + v.y*v.y) || 1;
    return v2(v.x / m, v.y / m);
}
export function dot(a, b) { return a.x*b.x + a.y*b.y; }
export function distance(a, b) {
    const dx = a.x - b.x, dy = a.y - b.y;
    return Math.sqrt(dx*dx + dy*dy);
}
)JS";

static constexpr const char* kWEColorSrc = R"JS(
function v3(x, y, z) { return new globalThis.Vec3(x, y, z); }
function c(v, k, i) {
    if (v && typeof v === 'object') return Number(v[k] ?? v[i] ?? 0);
    return Number(v ?? 0);
}
function clamp01(x) { return Math.max(0, Math.min(1, x)); }
export function normalizeColor(rgb) {
    return v3(c(rgb, 'x', 0) / 255, c(rgb, 'y', 1) / 255, c(rgb, 'z', 2) / 255);
}
export function expandColor(rgb) {
    return v3(c(rgb, 'x', 0) * 255, c(rgb, 'y', 1) * 255, c(rgb, 'z', 2) * 255);
}
export function rgb2hsv(rgb) {
    const r = clamp01(c(rgb, 'x', 0));
    const g = clamp01(c(rgb, 'y', 1));
    const b = clamp01(c(rgb, 'z', 2));
    const max = Math.max(r, g, b);
    const min = Math.min(r, g, b);
    const d = max - min;
    let h = 0;
    if (d !== 0) {
        if (max === r) h = ((g - b) / d) % 6;
        else if (max === g) h = (b - r) / d + 2;
        else h = (r - g) / d + 4;
        h /= 6;
        if (h < 0) h += 1;
    }
    const s = max === 0 ? 0 : d / max;
    return v3(h, s, max);
}
export function hsv2rgb(hsv) {
    let h = c(hsv, 'x', 0) % 1;
    if (h < 0) h += 1;
    const s = clamp01(c(hsv, 'y', 1));
    const v = clamp01(c(hsv, 'z', 2));
    const i = Math.floor(h * 6);
    const f = h * 6 - i;
    const p = v * (1 - s);
    const q = v * (1 - f * s);
    const t = v * (1 - (1 - f) * s);
    switch (i % 6) {
    case 0: return v3(v, t, p);
    case 1: return v3(q, v, p);
    case 2: return v3(p, v, t);
    case 3: return v3(p, q, v);
    case 4: return v3(t, p, v);
    default: return v3(v, p, q);
    }
}
)JS";

static constexpr BuiltinModule kBuiltinModules[] = {
    { "WEMath", kWEMathSrc },
    { "WEVector", kWEVectorSrc },
    { "WEColor", kWEColorSrc },
};

JSModuleDef* BuiltinModuleLoader(JSContext* ctx, const char* module_name, void*) {
    for (const auto& m : kBuiltinModules) {
        if (std::strcmp(m.name, module_name) != 0) continue;
        JSValue compiled = JS_Eval(ctx,
                                   m.source,
                                   std::strlen(m.source),
                                   module_name,
                                   JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
        if (JS_IsException(compiled)) {
            // JS_Eval already set the pending exception; QuickJS propagates.
            return nullptr;
        }
        JSModuleDef* def = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(compiled));
        // Don't free `compiled` — the pointer is the live module def.
        return def;
    }
    JS_ThrowReferenceError(ctx, "could not load module '%s'", module_name);
    return nullptr;
}

// --- Layer-B: SceneNode wrapper class ---------------------------------------
// `thisLayer` / `thisScene` resolve to instances of WWLayer. The class holds
// the SceneNode pointer in JS_GetOpaque; lifetime is owned by Scene, the
// finalizer is a no-op (we don't dereference on free, just drop the ref).

static JSClassID s_layer_class_id             = 0;
static JSClassID s_effect_class_id            = 0;
static JSClassID s_material_class_id          = 0;
static JSClassID s_particle_instance_class_id = 0;

struct LayerHandle {
    EngineHostState* host { nullptr };
    owe::SceneNode*  node { nullptr };
    std::string      name;
};

owe::SceneNode* ResolveLayerNode(LayerHandle* h) {
    if (! h) return nullptr;
    if (h->node) return h->node;
    if (! h->host || ! h->host->scene_root || h->name.empty()) return nullptr;
    return h->host->scene_root->FindByName(h->name);
}

void LayerFinalizer(JSRuntime*, JSValue v) {
    delete static_cast<LayerHandle*>(JS_GetOpaque(v, s_layer_class_id));
}

JSClassDef s_layer_class_def {
    .class_name = "WWLayer",
    .finalizer  = LayerFinalizer,
};

struct EffectHandle {
    EngineHostState*            host { nullptr };
    Option<SceneImageEffectRef> ref;
    bool                        fallback_visible { true };
};

struct MaterialHandle {
    EngineHostState* host { nullptr };
    SceneMaterial*   material { nullptr };
};

int MaterialSetProperty(JSContext* ctx, JSValueConst obj, JSAtom atom, JSValueConst value,
                        JSValueConst receiver, int flags);

JSClassExoticMethods s_material_exotic {
    .set_property = MaterialSetProperty,
};

void EffectFinalizer(JSRuntime*, JSValue v) {
    delete static_cast<EffectHandle*>(JS_GetOpaque(v, s_effect_class_id));
}

JSClassDef s_effect_class_def {
    .class_name = "WWEffect",
    .finalizer  = EffectFinalizer,
};

void MaterialFinalizer(JSRuntime*, JSValue v) {
    delete static_cast<MaterialHandle*>(JS_GetOpaque(v, s_material_class_id));
}

JSClassDef s_material_class_def {
    .class_name = "WWMaterial",
    .finalizer  = MaterialFinalizer,
    .exotic     = &s_material_exotic,
};

JSClassDef s_particle_instance_class_def {
    .class_name = "WWParticleInstance",
};

inline owe::SceneNode* GetLayerNode(JSValueConst v) {
    return ResolveLayerNode(static_cast<LayerHandle*>(JS_GetOpaque(v, s_layer_class_id)));
}

JSValue WrapLayerNode(JSContext* ctx, owe::SceneNode* node) {
    JSValue obj = JS_NewObjectClass(ctx, s_layer_class_id);
    if (JS_IsException(obj)) return obj;
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    JS_SetOpaque(obj, new LayerHandle { .host = host, .node = node });
    return obj;
}

JSValue WrapLayerName(JSContext* ctx, std::string name) {
    JSValue obj = JS_NewObjectClass(ctx, s_layer_class_id);
    if (JS_IsException(obj)) return obj;
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    JS_SetOpaque(obj, new LayerHandle { .host = host, .node = nullptr, .name = std::move(name) });
    return obj;
}

JSValue WrapParticleInstance(JSContext* ctx, owe::SceneNode* node) {
    JSValue obj = JS_NewObjectClass(ctx, s_particle_instance_class_id);
    if (JS_IsException(obj)) return obj;
    JS_SetOpaque(obj, node);
    return obj;
}

EffectHandle* GetEffectHandle(JSValueConst v) {
    return static_cast<EffectHandle*>(JS_GetOpaque(v, s_effect_class_id));
}

JSValue WrapEffect(JSContext* ctx, Option<SceneImageEffectRef> ref) {
    JSValue obj = JS_NewObjectClass(ctx, s_effect_class_id);
    if (JS_IsException(obj)) return obj;
    auto* host   = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    bool visible = host && host->scene && ref ? host->scene->ImageEffectRuntimeVisible(*ref) : true;
    JS_SetOpaque(
        obj, new EffectHandle { .host = host, .ref = std::move(ref), .fallback_visible = visible });
    return obj;
}

JSValue WrapMaterial(JSContext* ctx, SceneMaterial* material) {
    JSValue obj = JS_NewObjectClass(ctx, s_material_class_id);
    if (JS_IsException(obj)) return obj;
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    JS_SetOpaque(obj, new MaterialHandle { .host = host, .material = material });
    return obj;
}

JSValue EffectGetVisible(JSContext* ctx, JSValueConst this_val) {
    auto* h = GetEffectHandle(this_val);
    if (! h) return JS_NewBool(ctx, true);
    if (h->host && h->host->scene && h->ref)
        return JS_NewBool(ctx, h->host->scene->ImageEffectRuntimeVisible(*h->ref));
    return JS_NewBool(ctx, h->fallback_visible);
}

JSValue EffectSetVisible(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* h       = GetEffectHandle(this_val);
    bool  visible = JS_ToBool(ctx, val) != 0;
    if (! h) return JS_UNDEFINED;
    h->fallback_visible = visible;
    if (h->host && h->host->scene && h->ref) {
        h->host->scene->SetImageEffectRuntimeVisible(*h->ref, visible);
    }
    return JS_UNDEFINED;
}

inline JSValue MakeVec3(JSContext* ctx, double x, double y, double z) {
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    if (JS_IsUndefined(host->vec3_ctor)) {
        JSValue g       = JS_GetGlobalObject(ctx);
        host->vec3_ctor = JS_GetPropertyStr(ctx, g, "Vec3");
        JS_FreeValue(ctx, g);
    }
    JSValue args[3] {
        JS_NewFloat64(ctx, x),
        JS_NewFloat64(ctx, y),
        JS_NewFloat64(ctx, z),
    };
    JSValue r = JS_CallConstructor(ctx, host->vec3_ctor, 3, args);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    JS_FreeValue(ctx, args[2]);
    return r;
}

inline bool ReadXYZ(JSContext* ctx, JSValueConst v, double& x, double& y, double& z) {
    if (! JS_IsObject(v)) return false;
    JSValue jx = JS_GetPropertyStr(ctx, v, "x");
    JSValue jy = JS_GetPropertyStr(ctx, v, "y");
    JSValue jz = JS_GetPropertyStr(ctx, v, "z");
    bool    ok = (JS_ToFloat64(ctx, &x, jx) == 0) && (JS_ToFloat64(ctx, &y, jy) == 0) &&
                 (JS_ToFloat64(ctx, &z, jz) == 0);
    JS_FreeValue(ctx, jx);
    JS_FreeValue(ctx, jy);
    JS_FreeValue(ctx, jz);
    return ok;
}

Option<ShaderValue> ReadShaderValue(JSContext* ctx, JSValueConst value) {
    if (JS_IsNumber(value)) {
        double number {};
        if (JS_ToFloat64(ctx, &number, value) != 0) return None();
        return Some(ShaderValue(static_cast<float>(number)));
    }
    if (JS_IsBool(value)) return Some(ShaderValue(JS_ToBool(ctx, value) != 0 ? 1.0f : 0.0f));
    if (! JS_IsObject(value)) return None();

    constexpr rstd::array<const char*, 4> fields { "x", "y", "z", "w" };
    rstd::array<float, 4>                 values {};
    usize                                 count {};
    for (const char* field : fields) {
        JSValue component = JS_GetPropertyStr(ctx, value, field);
        if (JS_IsUndefined(component)) {
            JS_FreeValue(ctx, component);
            break;
        }
        double     number {};
        const bool ok = JS_ToFloat64(ctx, &number, component) == 0;
        JS_FreeValue(ctx, component);
        if (! ok) return None();
        values[count++] = static_cast<float>(number);
    }
    if (count == usize()) return None();
    return Some(ShaderValue(values.data(), count));
}

int MaterialSetProperty(JSContext* ctx, JSValueConst obj, JSAtom atom, JSValueConst value,
                        JSValueConst, int) {
    auto* handle = static_cast<MaterialHandle*>(JS_GetOpaque(obj, s_material_class_id));
    if (! handle || ! handle->host || ! handle->host->scene || ! handle->material) return 1;

    const char* key = JS_AtomToCString(ctx, atom);
    if (! key) return -1;
    auto shader_value = ReadShaderValue(ctx, value);
    if (shader_value.is_some()) {
        handle->host->scene->SetMaterialShaderValueByKey(
            *handle->material, rstd::cppstd::as_str(key).unwrap(), *shader_value);
    }
    JS_FreeCString(ctx, key);
    return 1;
}

// --- property accessors -----------------------------------------------------

JSValue NodeGetOrigin(JSContext* ctx, JSValueConst this_val) {
    auto* n = GetLayerNode(this_val);
    if (! n) return MakeVec3(ctx, 0, 0, 0);
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    if (host != nullptr) {
        auto hook = host->node_origin_hooks.get_mut(n);
        if (hook.is_some()) {
            const auto origin = (*(**hook).getter)();
            return MakeVec3(ctx, origin.x, origin.y, origin.z);
        }
    }
    auto v = n->Translate();
    return MakeVec3(ctx, v.x(), v.y(), v.z());
}
JSValue NodeSetOrigin(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* n = GetLayerNode(this_val);
    if (! n) return JS_UNDEFINED;
    double x = 0, y = 0, z = 0;
    if (! ReadXYZ(ctx, val, x, y, z)) return JS_UNDEFINED;
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    if (host != nullptr) {
        auto hook = host->node_origin_hooks.get_mut(n);
        if (hook.is_some()) {
            (*(**hook).setter)(Vec3Value { .x = x, .y = y, .z = z });
            return JS_UNDEFINED;
        }
    }
    n->SetTranslate({ float(x), float(y), float(z) });
    return JS_UNDEFINED;
}
JSValue NodeGetScale(JSContext* ctx, JSValueConst this_val) {
    auto* n = GetLayerNode(this_val);
    if (! n) return MakeVec3(ctx, 1, 1, 1);
    auto v = n->Scale();
    return MakeVec3(ctx, v.x(), v.y(), v.z());
}
JSValue NodeSetScale(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* n = GetLayerNode(this_val);
    if (! n) return JS_UNDEFINED;
    double x = 0, y = 0, z = 0;
    if (! ReadXYZ(ctx, val, x, y, z)) return JS_UNDEFINED;
    n->SetScale({ float(x), float(y), float(z) });
    return JS_UNDEFINED;
}
// The JS angles API is in degrees; SceneNode::m_rotation is radians.
constexpr double kRadToDeg = 180.0 / rstd::f64::consts::PI.to_primitive();
constexpr double kDegToRad = rstd::f64::consts::PI.to_primitive() / 180.0;
JSValue          NodeGetAngles(JSContext* ctx, JSValueConst this_val) {
    auto* n = GetLayerNode(this_val);
    if (! n) return MakeVec3(ctx, 0, 0, 0);
    auto v = n->Rotation();
    return MakeVec3(ctx, v.x() * kRadToDeg, v.y() * kRadToDeg, v.z() * kRadToDeg);
}
JSValue NodeSetAngles(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* n = GetLayerNode(this_val);
    if (! n) return JS_UNDEFINED;
    double x = 0, y = 0, z = 0;
    if (! ReadXYZ(ctx, val, x, y, z)) return JS_UNDEFINED;
    n->SetRotation({ float(x * kDegToRad), float(y * kDegToRad), float(z * kDegToRad) });
    return JS_UNDEFINED;
}

// Stubs — properties scripts read but writing them would force RG rebuild.
JSValue NodeGetSize(JSContext* ctx, JSValueConst this_val) {
    auto* n = GetLayerNode(this_val);
    if (! n) return MakeVec3(ctx, 100, 100, 0);
    const auto& s = n->Size();
    // Zero is the parser's "unknown" sentinel (particle/light nodes never
    // set it). Fall back to the legacy 100×100 the bootstrap stub returned.
    if (s.x() == 0.0f && s.y() == 0.0f) return MakeVec3(ctx, 100, 100, 0);
    return MakeVec3(ctx, s.x(), s.y(), 0);
}
JSValue NodeGetVisible(JSContext* ctx, JSValueConst this_val) {
    auto* n = GetLayerNode(this_val);
    return JS_NewBool(ctx, n ? n->Visible() : true);
}
JSValue NodeSetVisible(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* n = GetLayerNode(this_val);
    if (! n) return JS_UNDEFINED;
    const bool visible = JS_ToBool(ctx, val) != 0;
    auto*      host    = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    if (host && host->scene)
        host->scene->SetNodeVisible(*n, visible);
    else
        n->SetVisible(visible);
    return JS_UNDEFINED;
}
JSValue NodeGetAlpha(JSContext* ctx, JSValueConst this_val) {
    auto* n = GetLayerNode(this_val);
    return JS_NewFloat64(ctx, n ? n->UserAlpha() : 1.0);
}
JSValue NodeSetAlpha(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* n = GetLayerNode(this_val);
    if (! n) return JS_UNDEFINED;
    double a = 1.0;
    JS_ToFloat64(ctx, &a, val);
    n->SetUserAlpha(float(a));
    return JS_UNDEFINED;
}
JSValue NodeGetBrightness(JSContext* ctx, JSValueConst this_val) {
    auto* n = GetLayerNode(this_val);
    return JS_NewFloat64(ctx, n ? n->Brightness() : 1.0);
}
JSValue NodeSetBrightness(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* n = GetLayerNode(this_val);
    if (! n) return JS_UNDEFINED;
    double b = 1.0;
    JS_ToFloat64(ctx, &b, val);
    n->SetBrightness(float(b));
    return JS_UNDEFINED;
}
JSValue NodeGetColor(JSContext* ctx, JSValueConst this_val) {
    auto* n = GetLayerNode(this_val);
    if (! n) return MakeVec3(ctx, 1, 1, 1);
    const auto& c = n->Color();
    return MakeVec3(ctx, c.x(), c.y(), c.z());
}
JSValue NodeSetColor(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* n = GetLayerNode(this_val);
    if (! n) return JS_UNDEFINED;
    double x = 0, y = 0, z = 0;
    if (! ReadXYZ(ctx, val, x, y, z)) return JS_UNDEFINED;
    n->SetColor({ float(x), float(y), float(z) });
    return JS_UNDEFINED;
}
JSValue NodeGetVolume(JSContext* ctx, JSValueConst this_val) {
    auto* n = GetLayerNode(this_val);
    return JS_NewFloat64(ctx, n ? n->Volume() : 1.0);
}
JSValue NodeSetVolume(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* n = GetLayerNode(this_val);
    if (! n) return JS_UNDEFINED;
    double volume { 1.0 };
    if (JS_ToFloat64(ctx, &volume, val) == 0) n->SetVolume(static_cast<float>(volume));
    return JS_UNDEFINED;
}

JSValue ParticleGet(JSContext* ctx, JSValueConst this_val, ref<str> field, bool vector) {
    auto* node = static_cast<owe::SceneNode*>(JS_GetOpaque(this_val, s_particle_instance_class_id));
    if (! node) return JS_UNDEFINED;
    auto control = node->ParticleControlHandle();
    if (control.is_none()) return JS_UNDEFINED;
    auto value = (*control)->Get(field);
    if (vector) {
        if (value.len() < usize(3)) return JS_UNDEFINED;
        return MakeVec3(ctx, value[usize()], value[usize(1)], value[usize(2)]);
    }
    return value.is_empty() ? JS_UNDEFINED : JS_NewFloat64(ctx, value[usize()]);
}

JSValue ParticleSet(JSContext* ctx, JSValueConst this_val, JSValueConst value, ref<str> field,
                    bool vector) {
    auto* node = static_cast<owe::SceneNode*>(JS_GetOpaque(this_val, s_particle_instance_class_id));
    if (! node) return JS_UNDEFINED;
    auto control = node->ParticleControlHandle();
    if (control.is_none()) return JS_UNDEFINED;
    Vec<float> values;
    if (vector) {
        double x {}, y {}, z {};
        if (! ReadXYZ(ctx, value, x, y, z)) return JS_UNDEFINED;
        values.push(static_cast<float>(x));
        values.push(static_cast<float>(y));
        values.push(static_cast<float>(z));
    } else {
        double scalar {};
        if (JS_ToFloat64(ctx, &scalar, value) != 0) return JS_UNDEFINED;
        values.push(static_cast<float>(scalar));
    }
    (*control)->Apply(field, values.as_slice());
    return JS_UNDEFINED;
}

#define OWE_PARTICLE_PROPERTY(Name, Field, Vector)                                         \
    JSValue ParticleGet##Name(JSContext* ctx, JSValueConst this_val) {                     \
        return ParticleGet(ctx, this_val, Field, Vector);                                  \
    }                                                                                      \
    JSValue ParticleSet##Name(JSContext* ctx, JSValueConst this_val, JSValueConst value) { \
        return ParticleSet(ctx, this_val, value, Field, Vector);                           \
    }

OWE_PARTICLE_PROPERTY(Alpha, "alpha"_str, false)
OWE_PARTICLE_PROPERTY(Size, "size"_str, false)
OWE_PARTICLE_PROPERTY(Lifetime, "lifetime"_str, false)
OWE_PARTICLE_PROPERTY(Rate, "rate"_str, false)
OWE_PARTICLE_PROPERTY(Speed, "speed"_str, false)
OWE_PARTICLE_PROPERTY(Count, "count"_str, false)
OWE_PARTICLE_PROPERTY(Brightness, "brightness"_str, false)
OWE_PARTICLE_PROPERTY(Color, "color"_str, true)
OWE_PARTICLE_PROPERTY(Colorn, "colorn"_str, true)

#undef OWE_PARTICLE_PROPERTY

JSValue NodeGetParticleInstance(JSContext* ctx, JSValueConst this_val) {
    auto* node = GetLayerNode(this_val);
    if (! node || node->ParticleControl().is_none()) return JS_UNDEFINED;
    return WrapParticleInstance(ctx, node);
}
JSValue NodeGetPerspective(JSContext* ctx, JSValueConst this_val) {
    auto* n = GetLayerNode(this_val);
    return JS_NewBool(ctx, n ? n->Perspective() : false);
}
JSValue NodeSetPerspective(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* n = GetLayerNode(this_val);
    if (n) n->SetPerspective(JS_ToBool(ctx, val) != 0);
    return JS_UNDEFINED;
}
JSValue NodeGetAlignment(JSContext* ctx, JSValueConst this_val) {
    auto* n = GetLayerNode(this_val);
    if (! n) return JS_NewString(ctx, "center");
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    auto  hook = host->image_alignment_hooks.get(n);
    if (hook.is_none()) return JS_NewString(ctx, "center");
    auto alignment = (**hook).alignment.as_str();
    return JS_NewStringLen(
        ctx, reinterpret_cast<const char*>(alignment.data()), alignment.len().to_primitive());
}
JSValue NodeSetAlignment(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* n = GetLayerNode(this_val);
    if (! n) return JS_UNDEFINED;
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    auto  hook = host->image_alignment_hooks.get_mut(n);
    if (hook.is_none()) return JS_UNDEFINED;
    const char* s = JS_ToCString(ctx, val);
    if (s == nullptr) return JS_UNDEFINED;
    (**hook).alignment = String::make(rstd::cppstd::as_str(s).unwrap());
    (*(**hook).setter)(n, (**hook).alignment.as_str());
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}
JSValue NodeGetVAlign(JSContext* ctx, JSValueConst this_val) {
    auto* n = GetLayerNode(this_val);
    if (! n) return JS_NewString(ctx, "center");
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    auto  it   = host->text_align_hooks.find(n);
    return JS_NewString(
        ctx, it == host->text_align_hooks.end() ? "center" : it->second.vertical.c_str());
}
JSValue NodeGetHAlign(JSContext* ctx, JSValueConst this_val) {
    auto* n = GetLayerNode(this_val);
    if (! n) return JS_NewString(ctx, "center");
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    auto  it   = host->text_align_hooks.find(n);
    return JS_NewString(
        ctx, it == host->text_align_hooks.end() ? "center" : it->second.horizontal.c_str());
}
JSValue NodeSetVAlign(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* n = GetLayerNode(this_val);
    if (! n) return JS_UNDEFINED;
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    auto  it   = host->text_align_hooks.find(n);
    if (it == host->text_align_hooks.end()) return JS_UNDEFINED;
    const char* s = JS_ToCString(ctx, val);
    if (s == nullptr) return JS_UNDEFINED;
    it->second.vertical = s;
    if (it->second.set_vertical) it->second.set_vertical(it->second.vertical);
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}
JSValue NodeSetHAlign(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* n = GetLayerNode(this_val);
    if (! n) return JS_UNDEFINED;
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    auto  it   = host->text_align_hooks.find(n);
    if (it == host->text_align_hooks.end()) return JS_UNDEFINED;
    const char* s = JS_ToCString(ctx, val);
    if (s == nullptr) return JS_UNDEFINED;
    it->second.horizontal = s;
    if (it->second.set_horizontal) it->second.set_horizontal(it->second.horizontal);
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}
JSValue NodeSetIgnore(JSContext*, JSValueConst, JSValueConst) { return JS_UNDEFINED; }

// `text` is the only string-valued property on WWLayer. Most scripts only
// write it (clock / date / locale formatters); GetText therefore returns
// an empty string rather than tracking last-applied text state.
JSValue NodeGetText(JSContext* ctx, JSValueConst) { return JS_NewString(ctx, ""); }
JSValue NodeSetText(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* n = GetLayerNode(this_val);
    if (! n) return JS_UNDEFINED;
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    auto  it   = host->text_setters.find(n);
    if (it == host->text_setters.end()) return JS_UNDEFINED;
    const char* s = JS_ToCString(ctx, val);
    if (s == nullptr) return JS_UNDEFINED;
    it->second(std::string_view(s));
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}
JSValue NodeGetPointSize(JSContext* ctx, JSValueConst this_val) {
    auto* n = GetLayerNode(this_val);
    if (! n) return JS_NewFloat64(ctx, 1.0);
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    auto  it   = host->text_align_hooks.find(n);
    if (it != host->text_align_hooks.end()) {
        if (it->second.get_point_size) return JS_NewFloat64(ctx, it->second.get_point_size());
        return JS_NewFloat64(ctx, it->second.point_size);
    }
    auto* mesh = n->Mesh();
    return JS_NewFloat64(ctx, mesh == nullptr ? 1.0 : mesh->PointSize().to_primitive());
}
JSValue NodeSetPointSize(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* n = GetLayerNode(this_val);
    if (! n) return JS_UNDEFINED;
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    auto  it   = host->text_align_hooks.find(n);
    if (it == host->text_align_hooks.end()) return JS_UNDEFINED;
    double point_size = it->second.point_size;
    if (JS_ToFloat64(ctx, &point_size, val) < 0 || ! std::isfinite(point_size)) return JS_UNDEFINED;
    it->second.point_size = point_size;
    if (it->second.set_point_size) it->second.set_point_size(point_size);
    return JS_UNDEFINED;
}

// --- methods ----------------------------------------------------------------

JSValue NodeGetParent(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* n = GetLayerNode(this_val);
    if (n && n->Parent()) return WrapLayerNode(ctx, n->Parent());
    return JS_UNDEFINED;
}

JSValue NodeGetTransformMatrix(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto*   n   = GetLayerNode(this_val);
    JSValue m   = JS_NewArray(ctx);
    JSValue obj = JS_NewObject(ctx);
    if (n) {
        n->UpdateTrans();
        const auto& mat = n->ModelTrans(); // Eigen Matrix4d column-major
        for (int i = 0; i < 16; ++i) {
            JS_DefinePropertyValueUint32(
                ctx, m, i, JS_NewFloat64(ctx, mat.data()[i]), JS_PROP_C_W_E);
        }
    } else {
        constexpr double id[16] { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        for (int i = 0; i < 16; ++i) {
            JS_DefinePropertyValueUint32(ctx, m, i, JS_NewFloat64(ctx, id[i]), JS_PROP_C_W_E);
        }
    }
    JS_DefinePropertyValueStr(ctx, obj, "m", m, JS_PROP_C_W_E);
    return obj;
}

JSValue NodeRotateObjectSpace(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* n = GetLayerNode(this_val);
    if (! n || argc < 1) return JS_UNDEFINED;
    double x = 0, y = 0, z = 0;
    if (! ReadXYZ(ctx, argv[0], x, y, z)) return JS_UNDEFINED;
    n->RotateObjectSpace({ float(x), float(y), float(z) });
    return JS_UNDEFINED;
}

JSValue BoneTransformTranslation(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    JSValue v = JS_GetPropertyStr(ctx, this_val, "__wwTranslation");
    if (! JS_IsUndefined(v)) return v;
    JS_FreeValue(ctx, v);
    return MakeVec3(ctx, 0, 0, 0);
}

JSValue MakeBoneTransform(JSContext* ctx, const Eigen::Vector3f& translation) {
    JSValue obj = JS_NewObject(ctx);
    JS_DefinePropertyValueStr(ctx,
                              obj,
                              "__wwTranslation",
                              MakeVec3(ctx, translation.x(), translation.y(), translation.z()),
                              JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx,
                              obj,
                              "translation",
                              JS_NewCFunction(ctx, BoneTransformTranslation, "translation", 0),
                              JS_PROP_C_W_E);
    return obj;
}

JSValue NodeGetBoneIndex(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    auto* n    = GetLayerNode(this_val);
    if (! n || argc < 1 || ! host->bone_index_resolver) return JS_NewInt32(ctx, 0);
    const char* name = JS_ToCString(ctx, argv[0]);
    if (! name) return JS_NewInt32(ctx, 0);
    const uint32_t index = host->bone_index_resolver(n, name);
    JS_FreeCString(ctx, name);
    return JS_NewInt32(ctx, static_cast<int32_t>(index));
}

JSValue NodeGetBoneTransform(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    auto* n    = GetLayerNode(this_val);
    if (! n || argc < 1 || ! host->bone_transform_resolver)
        return MakeBoneTransform(ctx, { 0.0f, 0.0f, 0.0f });

    int32_t index = 0;
    JS_ToInt32(ctx, &index, argv[0]);
    if (index <= 0) return MakeBoneTransform(ctx, { 0.0f, 0.0f, 0.0f });

    auto bone =
        host->bone_transform_resolver(n, static_cast<uint32_t>(index), host->inputs.runtime);
    if (! bone) return MakeBoneTransform(ctx, { 0.0f, 0.0f, 0.0f });
    return MakeBoneTransform(ctx, { bone->x, bone->y, bone->z });
}

JSValue NodeGetChildren(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto*   n   = GetLayerNode(this_val);
    JSValue arr = JS_NewArray(ctx);
    if (! n) return arr;
    uint32_t i = 0;
    for (const auto& child : n->GetChildren()) {
        JS_DefinePropertyValueUint32(
            ctx, arr, i++, WrapLayerNode(ctx, child.as_ptr()), JS_PROP_C_W_E);
    }
    return arr;
}

JSValue NodeGetNameValue(JSContext* ctx, JSValueConst this_val) {
    auto* n = GetLayerNode(this_val);
    if (! n) return JS_NewString(ctx, "");
    return JS_NewStringLen(ctx, n->Name().data(), n->Name().size());
}

JSValue NodeGetName(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    return NodeGetNameValue(ctx, this_val);
}

JSValue NodeGetLayer(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    auto* n    = GetLayerNode(this_val);
    if (! n || argc < 1) return JS_DupValue(ctx, host->default_layer);
    const char* name = JS_ToCString(ctx, argv[0]);
    if (! name) return JS_DupValue(ctx, host->default_layer);
    std::string     layer_name { name };
    owe::SceneNode* hit = n->FindByName(layer_name);
    JS_FreeCString(ctx, name);
    return hit ? WrapLayerNode(ctx, hit) : WrapLayerName(ctx, std::move(layer_name));
}

JSValue NodeGetEffect(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    auto* n    = GetLayerNode(this_val);
    if (! host || ! host->scene || ! n || argc < 1) return WrapEffect(ctx, None());

    if (JS_IsNumber(argv[0])) {
        int64_t index {};
        if (JS_ToInt64(ctx, &index, argv[0]) != 0 || index < 0) return WrapEffect(ctx, None());
        return WrapEffect(ctx, host->scene->FindNodeImageEffect(*n, usize(index)));
    }

    const char* name = JS_ToCString(ctx, argv[0]);
    if (! name) return WrapEffect(ctx, None());
    auto effect = host->scene->FindNodeImageEffect(*n, name);
    JS_FreeCString(ctx, name);
    return WrapEffect(ctx, std::move(effect));
}

JSValue NodeGetEffectCount(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    auto* node = GetLayerNode(this_val);
    if (! host || ! host->scene || ! node) return JS_NewInt32(ctx, 0);
    return JS_NewInt64(
        ctx, static_cast<int64_t>(host->scene->NodeImageEffectCount(*node).to_primitive()));
}

JSValue EffectGetName(JSContext* ctx, JSValueConst this_val) {
    auto* handle = GetEffectHandle(this_val);
    if (! handle || ! handle->host || ! handle->host->scene || ! handle->ref)
        return JS_NewString(ctx, "");
    auto name = handle->host->scene->ImageEffectName(*handle->ref);
    auto view = rstd::cppstd::as_string_view(name);
    return JS_NewStringLen(ctx, view.data(), view.size());
}

JSValue EffectGetMaterial(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* handle = GetEffectHandle(this_val);
    if (! handle || ! handle->host || ! handle->host->scene || ! handle->ref || argc < 1)
        return WrapMaterial(ctx, nullptr);
    int64_t index {};
    if (JS_ToInt64(ctx, &index, argv[0]) != 0 || index < 0) return WrapMaterial(ctx, nullptr);
    return WrapMaterial(ctx, handle->host->scene->ImageEffectMaterial(*handle->ref, usize(index)));
}

bool TreeContains(owe::SceneNode* root, owe::SceneNode* needle) {
    if (! root || ! needle) return false;
    if (root == needle) return true;
    for (const auto& child : root->GetChildren()) {
        if (TreeContains(child.as_ptr(), needle)) return true;
    }
    return false;
}

JSValue NodeSceneLayerListIncludes(JSContext* ctx, JSValueConst this_val, int argc,
                                   JSValueConst* argv) {
    if (argc < 1) return JS_NewBool(ctx, false);
    JSValue root_val = JS_GetPropertyStr(ctx, this_val, "__wwRoot");
    auto*   root     = GetLayerNode(root_val);
    auto*   needle   = GetLayerNode(argv[0]);
    bool    found    = TreeContains(root, needle);
    JS_FreeValue(ctx, root_val);
    return JS_NewBool(ctx, found);
}

JSValue NodeSceneEnumerateLayers(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    JSValue arr = JS_NewArray(ctx);
    auto*   n   = GetLayerNode(this_val);
    if (! n) return arr;

    uint32_t i      = 0;
    auto     append = [&](auto& self, owe::SceneNode* node) -> void {
        if (! node) return;
        JS_DefinePropertyValueUint32(ctx, arr, i++, WrapLayerNode(ctx, node), JS_PROP_C_W_E);
        for (const auto& child : node->GetChildren()) {
            self(self, child.as_ptr());
        }
    };
    for (const auto& child : n->GetChildren()) {
        append(append, child.as_ptr());
    }
    JS_DefinePropertyValueStr(ctx, arr, "__wwRoot", WrapLayerNode(ctx, n), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx,
                              arr,
                              "includes",
                              JS_NewCFunction(ctx, NodeSceneLayerListIncludes, "includes", 1),
                              JS_PROP_C_W_E);
    return arr;
}

JSValue NodeSceneGetInitialLayerConfig(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    if (! host || argc < 1) return JS_NewObject(ctx);
    auto* node = GetLayerNode(argv[0]);
    if (! node) return JS_NewObject(ctx);
    auto config = host->initial_layer_configs.get(node);
    if (config.is_some()) return JsonToJs(ctx, **config);
    return JS_NewObject(ctx);
}

JSValue NodeSceneGetCameraTransforms(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    if (! host || ! host->scene) return JS_ThrowTypeError(ctx, "scene camera is not available");
    auto transforms = host->scene->ActiveCameraTransforms();
    if (transforms.is_none()) return JS_ThrowTypeError(ctx, "active scene camera is not available");

    JSValue out = JS_NewObject(ctx);
    JS_DefinePropertyValueStr(
        ctx,
        out,
        "eye",
        MakeVec3(ctx, transforms->eye.x(), transforms->eye.y(), transforms->eye.z()),
        JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(
        ctx,
        out,
        "center",
        MakeVec3(ctx, transforms->center.x(), transforms->center.y(), transforms->center.z()),
        JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(
        ctx,
        out,
        "up",
        MakeVec3(ctx, transforms->up.x(), transforms->up.y(), transforms->up.z()),
        JS_PROP_C_W_E);
    return out;
}

bool ReadOptionalCameraVector(JSContext* ctx, JSValueConst object, const char* name,
                              Eigen::Vector3d& out) {
    JSValue value = JS_GetPropertyStr(ctx, object, name);
    if (JS_IsUndefined(value)) {
        JS_FreeValue(ctx, value);
        return true;
    }
    double     x {}, y {}, z {};
    const bool ok = ReadXYZ(ctx, value, x, y, z);
    JS_FreeValue(ctx, value);
    if (! ok) return false;
    out = Eigen::Vector3d { x, y, z };
    return true;
}

JSValue NodeSceneSetCameraTransforms(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    if (! host || ! host->scene) return JS_ThrowTypeError(ctx, "scene camera is not available");
    if (argc < 1 || ! JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "camera transforms must be an object");
    auto transforms = host->scene->ActiveCameraTransforms();
    if (transforms.is_none()) return JS_ThrowTypeError(ctx, "active scene camera is not available");
    if (! ReadOptionalCameraVector(ctx, argv[0], "eye", transforms->eye) ||
        ! ReadOptionalCameraVector(ctx, argv[0], "center", transforms->center) ||
        ! ReadOptionalCameraVector(ctx, argv[0], "up", transforms->up))
        return JS_ThrowTypeError(ctx, "camera transform vectors require x, y and z");
    if (! host->scene->SetActiveCameraTransforms(*transforms))
        return JS_ThrowRangeError(ctx, "camera transforms are degenerate");
    return JS_UNDEFINED;
}

JSValue NodeSceneCreateLayer(JSContext* ctx, JSValueConst /*this_val*/, int argc,
                             JSValueConst* argv) {
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    auto* fs   = host->active_field_script;
    if (! fs) return JS_ThrowReferenceError(ctx, "createLayer requires an active field script");

    owe::SceneNode* node = nullptr;
    if (argc > 0 && ! JS_IsObject(argv[0])) {
        const char* asset = JS_ToCString(ctx, argv[0]);
        if (asset) {
            auto it = fs->m_impl->asset_clone_queues.find(asset);
            if (it != fs->m_impl->asset_clone_queues.end() && ! it->second.empty()) {
                node = it->second.front();
                it->second.erase(it->second.begin());
            }
            if (! node && host->layer_factory.is_some()) {
                auto created = (**host->layer_factory)(
                    fs->m_impl->node,
                    LayerAssetReference { .path        = rstd::cppstd::as_str(asset).unwrap(),
                                          .workshop_id = fs->WorkshopId() });
                if (created.is_some()) {
                    node                               = (*created).as_ptr();
                    fs->m_impl->clone_asset_keys[node] = asset;
                }
            }
            JS_FreeCString(ctx, asset);
        }
    } else if (argc > 0 && JS_IsObject(argv[0]) && host->layer_config_factory.is_some()) {
        Option<Json> config;
        if (auto* source_node = GetLayerNode(argv[0]); source_node != nullptr) {
            auto initial = host->initial_layer_configs.get(source_node);
            if (initial.is_some()) config = Some((**initial).clone());
        } else {
            JSValue global    = JS_GetGlobalObject(ctx);
            JSValue serialize = JS_GetPropertyStr(ctx, global, "__wwSerializeLayerConfig");
            JSValue encoded   = JS_Call(ctx, serialize, global, 1, argv);
            JS_FreeValue(ctx, serialize);
            JS_FreeValue(ctx, global);
            if (JS_IsException(encoded)) {
                JS_FreeValue(ctx, encoded);
                return JS_EXCEPTION;
            }
            const char* source = JS_ToCString(ctx, encoded);
            if (source != nullptr) {
                auto parsed = ParseJson(source);
                JS_FreeCString(ctx, source);
                if (parsed.is_ok()) config = Some(rstd::move(parsed).unwrap_unchecked());
            }
            JS_FreeValue(ctx, encoded);
            if (config.is_none())
                return JS_ThrowTypeError(ctx, "createLayer configuration is not serializable");
        }
        if (config.is_some()) {
            auto initial = (*config).clone();
            auto created = (**host->layer_config_factory)(fs->m_impl->node, rstd::move(*config));
            if (created.is_some()) {
                node = (*created).as_ptr();
                (void)host->initial_layer_configs.insert(node, rstd::move(initial));
            }
        }
    }
    if (! node) return JS_ThrowReferenceError(ctx, "createLayer asset is unavailable");
    if (host->scene)
        (void)host->scene->SetNodeVisible(*node, true);
    else
        node->SetVisible(true);
    node->Play();
    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValue perspective = JS_GetPropertyStr(ctx, argv[0], "perspective");
        if (! JS_IsUndefined(perspective)) node->SetPerspective(JS_ToBool(ctx, perspective) != 0);
        JS_FreeValue(ctx, perspective);
    }
    return WrapLayerNode(ctx, node);
}

JSValue NodeSceneDestroyLayer(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    if (auto* n = GetLayerNode(argv[0])) {
        auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
        if (host && host->scene)
            (void)host->scene->SetNodeVisible(*n, false);
        else
            n->SetVisible(false);
        n->Stop();
        auto* fs = host ? host->active_field_script : nullptr;
        if (fs) {
            auto key_it = fs->m_impl->clone_asset_keys.find(n);
            if (key_it != fs->m_impl->clone_asset_keys.end()) {
                auto& queue = fs->m_impl->asset_clone_queues[key_it->second];
                if (std::find(queue.begin(), queue.end(), n) == queue.end()) queue.push_back(n);
            }
        }
    }
    return JS_UNDEFINED;
}

owe::SceneNode* ResolveSceneLayerArgument(JSContext* ctx, JSValueConst this_val,
                                          JSValueConst value) {
    if (auto* node = GetLayerNode(value)) return node;
    auto* root = GetLayerNode(this_val);
    if (root == nullptr) return nullptr;
    const char* name = JS_ToCString(ctx, value);
    if (name == nullptr) return nullptr;
    auto* node = root->FindByName(name);
    JS_FreeCString(ctx, name);
    return node;
}

JSValue NodeSceneGetLayerIndex(JSContext* ctx, JSValueConst this_val, int argc,
                               JSValueConst* argv) {
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    if (host == nullptr || host->scene == nullptr || argc < 1) return JS_NewInt32(ctx, -1);
    auto* node = ResolveSceneLayerArgument(ctx, this_val, argv[0]);
    if (node == nullptr) return JS_NewInt32(ctx, -1);
    auto index = host->scene->LayerIndex(*node);
    if (index.is_none()) return JS_NewInt32(ctx, -1);
    return JS_NewInt64(ctx, static_cast<int64_t>(index->to_primitive()));
}

JSValue NodeSceneSortLayer(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    if (host == nullptr || host->scene == nullptr || argc < 2) return JS_UNDEFINED;
    auto*   node = ResolveSceneLayerArgument(ctx, this_val, argv[0]);
    int64_t index {};
    if (node == nullptr || JS_ToInt64(ctx, &index, argv[1]) != 0 || index < 0) return JS_UNDEFINED;
    (void)host->scene->SortLayer(*node, usize(static_cast<std::size_t>(index)));
    return JS_UNDEFINED;
}

JSValue NodePlay(JSContext*, JSValueConst this_val, int, JSValueConst*) {
    if (auto* n = GetLayerNode(this_val)) n->Play();
    return JS_UNDEFINED;
}
JSValue NodeStop(JSContext*, JSValueConst this_val, int, JSValueConst*) {
    if (auto* n = GetLayerNode(this_val)) n->Stop();
    return JS_UNDEFINED;
}
JSValue NodePause(JSContext*, JSValueConst this_val, int, JSValueConst*) {
    if (auto* n = GetLayerNode(this_val)) n->Pause();
    return JS_UNDEFINED;
}
JSValue NodeIsPlaying(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* n = GetLayerNode(this_val);
    return JS_NewBool(ctx, n ? n->IsPlaying() : false);
}

// --- WWTextureAnimation -----------------------------------------------------
// Wraps a SceneNode*'s TextureAnimatorState. Slot 0 only — every workshop
// script that touches `getTextureAnimation()` in the corpus uses the primary
// (diffuse) texture animation; multi-slot would need a different API shape.

static JSClassID s_texanim_class_id = 0;

JSClassDef s_texanim_class_def {
    .class_name = "WWTextureAnimation",
    .finalizer  = nullptr, // SceneNode owns the state
};

inline owe::SceneNode* GetTexAnimNode(JSValueConst v) {
    return static_cast<owe::SceneNode*>(JS_GetOpaque(v, s_texanim_class_id));
}

JSValue TexAnimPlay(JSContext*, JSValueConst this_val, int, JSValueConst*) {
    if (auto* n = GetTexAnimNode(this_val)) {
        auto& a         = n->TexAnim();
        a.current_frame = -1;
        a.playing       = true;
    }
    return JS_UNDEFINED;
}
JSValue TexAnimStop(JSContext*, JSValueConst this_val, int, JSValueConst*) {
    if (auto* n = GetTexAnimNode(this_val)) n->TexAnim().playing = false;
    return JS_UNDEFINED;
}
JSValue TexAnimPause(JSContext*, JSValueConst this_val, int, JSValueConst*) {
    if (auto* n = GetTexAnimNode(this_val)) n->TexAnim().playing = false;
    return JS_UNDEFINED;
}
JSValue TexAnimSetFrame(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    auto* n = GetTexAnimNode(this_val);
    if (! n) return JS_UNDEFINED;
    int32_t f = 0;
    JS_ToInt32(ctx, &f, argv[0]);
    if (f < 0) f = 0;
    n->TexAnim().current_frame = f;
    n->TexAnim().playing       = false;
    return JS_UNDEFINED;
}
JSValue TexAnimGetFrame(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* n = GetTexAnimNode(this_val);
    if (! n) return JS_NewInt32(ctx, 0);
    const int f = n->TexAnim().current_frame;
    return JS_NewInt32(ctx, f < 0 ? 0 : f);
}
JSValue TexAnimIsPlaying(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* n = GetTexAnimNode(this_val);
    return JS_NewBool(ctx, n ? n->TexAnim().playing : false);
}

const JSCFunctionListEntry s_texanim_proto_funcs[] = {
    JS_CFUNC_DEF("play", 0, TexAnimPlay),         JS_CFUNC_DEF("stop", 0, TexAnimStop),
    JS_CFUNC_DEF("pause", 0, TexAnimPause),       JS_CFUNC_DEF("setFrame", 1, TexAnimSetFrame),
    JS_CFUNC_DEF("getFrame", 0, TexAnimGetFrame), JS_CFUNC_DEF("isPlaying", 0, TexAnimIsPlaying),
};

void InitTexAnimClass(JSContext* ctx, JSRuntime* rt) {
    if (s_texanim_class_id == 0) JS_NewClassID(rt, &s_texanim_class_id);
    JS_NewClass(rt, s_texanim_class_id, &s_texanim_class_def);
    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx,
                               proto,
                               s_texanim_proto_funcs,
                               sizeof(s_texanim_proto_funcs) / sizeof(s_texanim_proto_funcs[0]));
    JS_SetClassProto(ctx, s_texanim_class_id, proto);
}

JSValue NodeGetTextureAnimation(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* n = GetLayerNode(this_val);
    if (! n) {
        // Unbound (default) layer — fall back to the JS-side stub so reads
        // like .getFrame() don't TypeError.
        JSValue g = JS_GetGlobalObject(ctx);
        JSValue f = JS_GetPropertyStr(ctx, g, "__wwCreateTexAnimStub");
        JSValue r = JS_Call(ctx, f, JS_UNDEFINED, 0, nullptr);
        JS_FreeValue(ctx, f);
        JS_FreeValue(ctx, g);
        return r;
    }
    JSValue obj = JS_NewObjectClass(ctx, s_texanim_class_id);
    if (JS_IsException(obj)) return obj;
    JS_SetOpaque(obj, n);
    return obj;
}

// Sprite-image .getAnimation() and puppet-bone .getAnimationLayer(name).
// Renderer doesn't yet expose either through the C-class, so route both
// through the JS-side __wwCreateAnimationStub which gives scripts a
// no-op handle with `rate` / play / stop / isPlaying.
JSValue NodeGetAnimationStub(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue f = JS_GetPropertyStr(ctx, g, "__wwCreateAnimationStub");
    JSValue r = JS_Call(ctx, f, JS_UNDEFINED, 0, nullptr);
    JS_FreeValue(ctx, f);
    JS_FreeValue(ctx, g);
    return r;
}

static JSClassID s_video_texture_class_id = 0;

struct VideoTextureHandle {
    Arc<VideoPlaybackState> playback;
};

void VideoTextureFinalizer(JSRuntime*, JSValue value) {
    delete static_cast<VideoTextureHandle*>(JS_GetOpaque(value, s_video_texture_class_id));
}

JSClassDef s_video_texture_class_def {
    .class_name = "WWVideoTexture",
    .finalizer  = VideoTextureFinalizer,
};

VideoPlaybackState* GetVideoPlayback(JSValueConst value) {
    auto* handle = static_cast<VideoTextureHandle*>(JS_GetOpaque(value, s_video_texture_class_id));
    return handle != nullptr ? handle->playback.as_ptr().as_raw_ptr() : nullptr;
}

JSValue VideoTextureGetDuration(JSContext* ctx, JSValueConst this_val) {
    auto* playback = GetVideoPlayback(this_val);
    auto  duration = playback != nullptr ? playback->Duration() : None<f64>();
    return JS_NewFloat64(ctx, duration.unwrap_or(f64()).to_primitive());
}

JSValue VideoTextureGetRate(JSContext* ctx, JSValueConst this_val) {
    auto* playback = GetVideoPlayback(this_val);
    return JS_NewFloat64(ctx, playback != nullptr ? playback->Snapshot().rate.to_primitive() : 1.0);
}

JSValue VideoTextureSetRate(JSContext* ctx, JSValueConst this_val, JSValueConst value) {
    double rate = 1.0;
    if (JS_ToFloat64(ctx, &rate, value) != 0) return JS_UNDEFINED;
    auto parsed_rate = f64(rate);
    if (parsed_rate.is_finite() && parsed_rate > f64()) {
        if (auto* playback = GetVideoPlayback(this_val)) playback->SetRate(parsed_rate);
    }
    return JS_UNDEFINED;
}

JSValue VideoTextureGetVolume(JSContext* ctx, JSValueConst) { return JS_NewFloat64(ctx, 1.0); }
JSValue VideoTextureSetVolume(JSContext*, JSValueConst, JSValueConst) { return JS_UNDEFINED; }

JSValue VideoTexturePlay(JSContext*, JSValueConst this_val, int, JSValueConst*) {
    if (auto* playback = GetVideoPlayback(this_val)) playback->Play();
    return JS_UNDEFINED;
}

JSValue VideoTextureStop(JSContext*, JSValueConst this_val, int, JSValueConst*) {
    if (auto* playback = GetVideoPlayback(this_val)) playback->Stop();
    return JS_UNDEFINED;
}

JSValue VideoTexturePause(JSContext*, JSValueConst this_val, int, JSValueConst*) {
    if (auto* playback = GetVideoPlayback(this_val)) playback->Pause();
    return JS_UNDEFINED;
}

JSValue VideoTextureSetCurrentTime(JSContext* ctx, JSValueConst this_val, int argc,
                                   JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    double seconds = 0.0;
    if (JS_ToFloat64(ctx, &seconds, argv[0]) != 0) return JS_UNDEFINED;
    auto parsed_seconds = f64(seconds);
    if (parsed_seconds.is_finite() && parsed_seconds >= f64()) {
        if (auto* playback = GetVideoPlayback(this_val)) playback->Seek(parsed_seconds);
    }
    return JS_UNDEFINED;
}

JSValue VideoTextureGetCurrentTime(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* playback = GetVideoPlayback(this_val);
    return JS_NewFloat64(ctx, playback != nullptr ? playback->CurrentTime().to_primitive() : 0.0);
}

JSValue VideoTextureIsPlaying(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* playback = GetVideoPlayback(this_val);
    return JS_NewBool(ctx, playback != nullptr && playback->Snapshot().playing);
}

const JSCFunctionListEntry s_video_texture_proto_funcs[] = {
    JS_CGETSET_DEF("duration", VideoTextureGetDuration, NodeSetIgnore),
    JS_CGETSET_DEF("rate", VideoTextureGetRate, VideoTextureSetRate),
    JS_CGETSET_DEF("volume", VideoTextureGetVolume, VideoTextureSetVolume),
    JS_CFUNC_DEF("play", 0, VideoTexturePlay),
    JS_CFUNC_DEF("stop", 0, VideoTextureStop),
    JS_CFUNC_DEF("pause", 0, VideoTexturePause),
    JS_CFUNC_DEF("setCurrentTime", 1, VideoTextureSetCurrentTime),
    JS_CFUNC_DEF("getCurrentTime", 0, VideoTextureGetCurrentTime),
    JS_CFUNC_DEF("isPlaying", 0, VideoTextureIsPlaying),
};

void InitVideoTextureClass(JSContext* ctx, JSRuntime* rt) {
    if (s_video_texture_class_id == 0) JS_NewClassID(rt, &s_video_texture_class_id);
    JS_NewClass(rt, s_video_texture_class_id, &s_video_texture_class_def);
    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx,
                               proto,
                               s_video_texture_proto_funcs,
                               sizeof(s_video_texture_proto_funcs) /
                                   sizeof(s_video_texture_proto_funcs[0]));
    JS_SetClassProto(ctx, s_video_texture_class_id, proto);
}

JSValue NodeGetVideoTexture(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* node     = GetLayerNode(this_val);
    auto  playback = node != nullptr ? node->VideoControlHandle() : None<Arc<VideoPlaybackState>>();
    if (playback.is_none()) {
        JSValue global  = JS_GetGlobalObject(ctx);
        JSValue factory = JS_GetPropertyStr(ctx, global, "__wwCreateVideoTextureStub");
        JSValue result  = JS_Call(ctx, factory, JS_UNDEFINED, 0, nullptr);
        JS_FreeValue(ctx, factory);
        JS_FreeValue(ctx, global);
        return result;
    }
    JSValue object = JS_NewObjectClass(ctx, s_video_texture_class_id);
    if (JS_IsException(object)) return object;
    JS_SetOpaque(object, new VideoTextureHandle { .playback = rstd::move(*playback) });
    return object;
}

const JSCFunctionListEntry s_layer_proto_funcs[] = {
    JS_CGETSET_DEF("origin", NodeGetOrigin, NodeSetOrigin),
    JS_CGETSET_DEF("scale", NodeGetScale, NodeSetScale),
    JS_CGETSET_DEF("angles", NodeGetAngles, NodeSetAngles),
    JS_CGETSET_DEF("size", NodeGetSize, NodeSetIgnore),
    JS_CGETSET_DEF("visible", NodeGetVisible, NodeSetVisible),
    JS_CGETSET_DEF("alpha", NodeGetAlpha, NodeSetAlpha),
    JS_CGETSET_DEF("brightness", NodeGetBrightness, NodeSetBrightness),
    JS_CGETSET_DEF("color", NodeGetColor, NodeSetColor),
    JS_CGETSET_DEF("volume", NodeGetVolume, NodeSetVolume),
    JS_CGETSET_DEF("instance", NodeGetParticleInstance, NodeSetIgnore),
    JS_CGETSET_DEF("perspective", NodeGetPerspective, NodeSetPerspective),
    JS_CGETSET_DEF("alignment", NodeGetAlignment, NodeSetAlignment),
    JS_CGETSET_DEF("text", NodeGetText, NodeSetText),
    JS_CGETSET_DEF("name", NodeGetNameValue, NodeSetIgnore),
    JS_CGETSET_DEF("verticalalign", NodeGetVAlign, NodeSetVAlign),
    JS_CGETSET_DEF("horizontalalign", NodeGetHAlign, NodeSetHAlign),
    JS_CGETSET_DEF("pointsize", NodeGetPointSize, NodeSetPointSize),
    JS_CFUNC_DEF("getParent", 0, NodeGetParent),
    JS_CFUNC_DEF("getTransformMatrix", 0, NodeGetTransformMatrix),
    JS_CFUNC_DEF("rotateObjectSpace", 1, NodeRotateObjectSpace),
    JS_CFUNC_DEF("getChildren", 0, NodeGetChildren),
    JS_CFUNC_DEF("getName", 0, NodeGetName),
    JS_CFUNC_DEF("getLayer", 1, NodeGetLayer),
    JS_CFUNC_DEF("getEffect", 1, NodeGetEffect),
    JS_CFUNC_DEF("getEffectCount", 0, NodeGetEffectCount),
    JS_CFUNC_DEF("enumerateLayers", 0, NodeSceneEnumerateLayers),
    JS_CFUNC_DEF("getInitialLayerConfig", 1, NodeSceneGetInitialLayerConfig),
    JS_CFUNC_DEF("getCameraTransforms", 0, NodeSceneGetCameraTransforms),
    JS_CFUNC_DEF("setCameraTransforms", 1, NodeSceneSetCameraTransforms),
    JS_CFUNC_DEF("getBoneIndex", 1, NodeGetBoneIndex),
    JS_CFUNC_DEF("getBoneTransform", 1, NodeGetBoneTransform),
    JS_CFUNC_DEF("getTextureAnimation", 0, NodeGetTextureAnimation),
    JS_CFUNC_DEF("getVideoTexture", 0, NodeGetVideoTexture),
    JS_CFUNC_DEF("getAnimation", 0, NodeGetAnimationStub),
    JS_CFUNC_DEF("getAnimationLayer", 1, NodeGetAnimationStub),
    JS_CFUNC_DEF("createLayer", 1, NodeSceneCreateLayer),
    JS_CFUNC_DEF("destroyLayer", 1, NodeSceneDestroyLayer),
    JS_CFUNC_DEF("getLayerIndex", 1, NodeSceneGetLayerIndex),
    JS_CFUNC_DEF("sortLayer", 2, NodeSceneSortLayer),
    JS_CFUNC_DEF("play", 0, NodePlay),
    JS_CFUNC_DEF("stop", 0, NodeStop),
    JS_CFUNC_DEF("pause", 0, NodePause),
    JS_CFUNC_DEF("isPlaying", 0, NodeIsPlaying),
};

void InitLayerClass(JSContext* ctx, JSRuntime* rt) {
    if (s_layer_class_id == 0) JS_NewClassID(rt, &s_layer_class_id);
    JS_NewClass(rt, s_layer_class_id, &s_layer_class_def);
    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx,
                               proto,
                               s_layer_proto_funcs,
                               sizeof(s_layer_proto_funcs) / sizeof(s_layer_proto_funcs[0]));
    JS_SetClassProto(ctx, s_layer_class_id, proto);
}

const JSCFunctionListEntry s_particle_instance_proto_funcs[] = {
    JS_CGETSET_DEF("alpha", ParticleGetAlpha, ParticleSetAlpha),
    JS_CGETSET_DEF("size", ParticleGetSize, ParticleSetSize),
    JS_CGETSET_DEF("lifetime", ParticleGetLifetime, ParticleSetLifetime),
    JS_CGETSET_DEF("rate", ParticleGetRate, ParticleSetRate),
    JS_CGETSET_DEF("speed", ParticleGetSpeed, ParticleSetSpeed),
    JS_CGETSET_DEF("count", ParticleGetCount, ParticleSetCount),
    JS_CGETSET_DEF("brightness", ParticleGetBrightness, ParticleSetBrightness),
    JS_CGETSET_DEF("color", ParticleGetColor, ParticleSetColor),
    JS_CGETSET_DEF("colorn", ParticleGetColorn, ParticleSetColorn),
};

void InitParticleInstanceClass(JSContext* ctx, JSRuntime* rt) {
    if (s_particle_instance_class_id == 0) JS_NewClassID(rt, &s_particle_instance_class_id);
    JS_NewClass(rt, s_particle_instance_class_id, &s_particle_instance_class_def);
    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx,
                               proto,
                               s_particle_instance_proto_funcs,
                               sizeof(s_particle_instance_proto_funcs) /
                                   sizeof(s_particle_instance_proto_funcs[0]));
    JS_SetClassProto(ctx, s_particle_instance_class_id, proto);
}

const JSCFunctionListEntry s_effect_proto_funcs[] = {
    JS_CGETSET_DEF("visible", EffectGetVisible, EffectSetVisible),
    JS_CGETSET_DEF("name", EffectGetName, NodeSetIgnore),
    JS_CFUNC_DEF("getMaterial", 1, EffectGetMaterial),
};

void InitEffectClass(JSContext* ctx, JSRuntime* rt) {
    if (s_effect_class_id == 0) JS_NewClassID(rt, &s_effect_class_id);
    JS_NewClass(rt, s_effect_class_id, &s_effect_class_def);
    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx,
                               proto,
                               s_effect_proto_funcs,
                               sizeof(s_effect_proto_funcs) / sizeof(s_effect_proto_funcs[0]));
    JS_SetClassProto(ctx, s_effect_class_id, proto);
}

void InitMaterialClass(JSContext* ctx, JSRuntime* rt) {
    if (s_material_class_id == 0) JS_NewClassID(rt, &s_material_class_id);
    JS_NewClass(rt, s_material_class_id, &s_material_class_def);
    JS_SetClassProto(ctx, s_material_class_id, JS_NewObject(ctx));
}

// Stash the bootstrap's `thisLayer` / `thisScene` stubs for restore.
void CaptureDefaultBindings(JSContext* ctx) {
    auto*   host        = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    JSValue g           = JS_GetGlobalObject(ctx);
    host->default_layer = JS_GetPropertyStr(ctx, g, "thisLayer");
    host->default_scene = JS_GetPropertyStr(ctx, g, "thisScene");
    JS_FreeValue(ctx, g);
}

// Write `globalThis.thisLayer = val`. `val` is duplicated; ownership of
// the original ref stays with the caller.
void BindThisLayer(JSContext* ctx, JSValueConst val) {
    JSValue g = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, "thisLayer", JS_DupValue(ctx, val));
    JS_SetPropertyStr(ctx, g, "thisObject", JS_DupValue(ctx, val));
    JS_FreeValue(ctx, g);
}
void BindThisScene(JSContext* ctx, JSValueConst val) {
    JSValue g = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, "thisScene", JS_DupValue(ctx, val));
    JS_FreeValue(ctx, g);
}

JSValue MakeMediaPlaybackEvent(JSContext* ctx, const MediaStatus& status) {
    JSValue ev = JS_NewObject(ctx);
    JS_DefinePropertyValueStr(ctx, ev, "state", JS_NewUint32(ctx, status.state), JS_PROP_C_W_E);
    return ev;
}

JSValue MakeMediaPropertiesEvent(JSContext* ctx, const MediaStatus& status) {
    JSValue ev = JS_NewObject(ctx);
    JS_DefinePropertyValueStr(ctx,
                              ev,
                              "title",
                              JS_NewStringLen(ctx, status.title.data(), status.title.size()),
                              JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx,
                              ev,
                              "artist",
                              JS_NewStringLen(ctx, status.artist.data(), status.artist.size()),
                              JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx,
                              ev,
                              "album",
                              JS_NewStringLen(ctx, status.album.data(), status.album.size()),
                              JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx,
                              ev,
                              "albumTitle",
                              JS_NewStringLen(ctx, status.album.data(), status.album.size()),
                              JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(
        ctx,
        ev,
        "albumArtist",
        JS_NewStringLen(ctx, status.album_artist.data(), status.album_artist.size()),
        JS_PROP_C_W_E);
    return ev;
}

JSValue MakeMediaThumbnailEvent(JSContext* ctx, const MediaStatus& status) {
    JSValue ev = JS_NewObject(ctx);
    JS_DefinePropertyValueStr(
        ctx, ev, "hasThumbnail", JS_NewBool(ctx, ! status.art_url.empty()), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx,
                              ev,
                              "thumbnail",
                              JS_NewStringLen(ctx, status.art_url.data(), status.art_url.size()),
                              JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx,
                              ev,
                              "artUrl",
                              JS_NewStringLen(ctx, status.art_url.data(), status.art_url.size()),
                              JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(
        ctx,
        ev,
        "previousThumbnail",
        JS_NewStringLen(ctx, status.previous_art_url.data(), status.previous_art_url.size()),
        JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, ev, "primaryColor", MakeVec3(ctx, 1, 1, 1), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, ev, "secondaryColor", MakeVec3(ctx, 0, 0, 0), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, ev, "tertiaryColor", MakeVec3(ctx, 0, 0, 0), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, ev, "textColor", MakeVec3(ctx, 0, 0, 0), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, ev, "highContrastColor", MakeVec3(ctx, 0, 0, 0), JS_PROP_C_W_E);
    return ev;
}

} // namespace

// --- JsRuntime methods ------------------------------------------------------

JsRuntime::JsRuntime(): m_impl(std::make_unique<Impl>()) {
    m_impl->rt  = JS_NewRuntime();
    m_impl->ctx = JS_NewContext(m_impl->rt);
    if (! m_impl->rt || ! m_impl->ctx) {
        rstd_error("script: JS_NewRuntime/JS_NewContext failed");
        return;
    }
    // QuickJS's default stack-overflow check is conservative (relative to
    // the OS thread stack at runtime init). When the wallpaper renderer
    // runs scripts from a deep call site (Vulkan render thread, post-
    // particle emission), `new Date()` and similar built-ins hit the
    // stack-frame guard and throw "Maximum call stack size exceeded".
    // Disable the soft check; the OS stack is plenty for clock/audio-
    // response style scripts in the corpus.
    JS_SetMaxStackSize(m_impl->rt, 0);
    JS_SetContextOpaque(m_impl->ctx, &m_impl->host);
    // Built-in ES modules (WEMath, …). Resolves bare `import 'WEMath'`
    // against the kBuiltinModules table; unknown names raise
    // ReferenceError via the loader.
    JS_SetModuleLoaderFunc(
        m_impl->rt, /*normalize=*/nullptr, BuiltinModuleLoader, /*opaque=*/nullptr);
    InitLayerClass(m_impl->ctx, m_impl->rt);
    InitParticleInstanceClass(m_impl->ctx, m_impl->rt);
    InitEffectClass(m_impl->ctx, m_impl->rt);
    InitMaterialClass(m_impl->ctx, m_impl->rt);
    InitTexAnimClass(m_impl->ctx, m_impl->rt);
    InitVideoTextureClass(m_impl->ctx, m_impl->rt);
    InstallEngineGlobal(m_impl->ctx);
    // Bootstrap created stub `thisLayer` / `thisScene` on globalThis.
    // Capture them now so per-script binding can fall back to the stub
    // when no SceneNode is provided.
    CaptureDefaultBindings(m_impl->ctx);
}

JsRuntime::~JsRuntime() {
    if (! m_impl) return;
    // Drop FieldScripts before tearing down the runtime so their JSValues
    // go through JS_FreeValue while the context is still alive.
    for (auto& fs : m_impl->scripts) {
        if (fs && fs->m_impl) {
            JS_FreeValue(m_impl->ctx, fs->m_impl->update_fn);
            JS_FreeValue(m_impl->ctx, fs->m_impl->animation_event_fn);
            JS_FreeValue(m_impl->ctx, fs->m_impl->init_fn);
            JS_FreeValue(m_impl->ctx, fs->m_impl->module_ns);
            JS_FreeValue(m_impl->ctx, fs->m_impl->current_value);
            if (! JS_IsUndefined(fs->m_impl->wrapped_layer))
                JS_FreeValue(m_impl->ctx, fs->m_impl->wrapped_layer);
        }
    }
    m_impl->scripts.clear();
    for (auto& [_sha, ns] : m_impl->ns_by_sha) JS_FreeValue(m_impl->ctx, ns);
    m_impl->ns_by_sha.clear();
    if (! JS_IsUndefined(m_impl->wrapped_scene)) JS_FreeValue(m_impl->ctx, m_impl->wrapped_scene);
    if (! JS_IsUndefined(m_impl->host.vec3_ctor)) JS_FreeValue(m_impl->ctx, m_impl->host.vec3_ctor);
    if (! JS_IsUndefined(m_impl->host.default_layer))
        JS_FreeValue(m_impl->ctx, m_impl->host.default_layer);
    if (! JS_IsUndefined(m_impl->host.default_scene))
        JS_FreeValue(m_impl->ctx, m_impl->host.default_scene);
    for (auto& slot : m_impl->host.audio_buffers) {
        if (! JS_IsUndefined(slot.object)) {
            JS_FreeValue(m_impl->ctx, slot.object);
            slot.object = JS_UNDEFINED;
        }
    }
    for (auto& d : m_impl->host.deferred) {
        if (! JS_IsUndefined(d.fn)) JS_FreeValue(m_impl->ctx, d.fn);
    }
    m_impl->host.deferred.clear();
    if (m_impl->ctx) JS_FreeContext(m_impl->ctx);
    if (m_impl->rt) JS_FreeRuntime(m_impl->rt);
}

void JsRuntime::SetFrameInputs(const FrameInputs& fi) {
    m_impl->host.inputs = fi;
    UpdateInputObject(m_impl->ctx);
    RefreshAudioBuffers(m_impl->ctx, m_impl->host);
}

void JsRuntime::SetAudioResponseDemand(Option<Arc<AudioResponseDemand>> demand) {
    m_impl->host.audio_response_lease  = None();
    m_impl->host.audio_response_demand = rstd::move(demand);
    if (HasAudioBuffers(m_impl->host) && m_impl->host.audio_response_demand.is_some()) {
        m_impl->host.audio_response_lease = Some((*m_impl->host.audio_response_demand)->Acquire());
    }
}

void JsRuntime::SetUserProperty(std::string_view key, const Json& property) {
    if (! m_impl || ! m_impl->ctx) return;
    std::string key_str { key };
    JSContext*  ctx    = m_impl->ctx;
    JSValue     global = JS_GetGlobalObject(ctx);
    JSValue     engine = JS_GetPropertyStr(ctx, global, "engine");
    JSValue     props  = JS_GetPropertyStr(ctx, engine, "userProperties");
    if (! JS_IsObject(props)) {
        JS_FreeValue(ctx, props);
        props = JS_NewObject(ctx);
        JS_DefinePropertyValueStr(
            ctx, engine, "userProperties", JS_DupValue(ctx, props), JS_PROP_C_W_E);
    }
    JS_DefinePropertyValueStr(
        ctx, props, key_str.c_str(), UserPropertyValueToJs(ctx, property), JS_PROP_C_W_E);

    JSValue changed = JS_NewObject(ctx);
    JS_DefinePropertyValueStr(
        ctx, changed, key_str.c_str(), UserPropertyValueToJs(ctx, property), JS_PROP_C_W_E);
    for (auto& fs : m_impl->scripts) {
        auto* I = fs->m_impl.get();
        if (! I->alive) continue;
        JSValue fn = JS_GetPropertyStr(ctx, I->module_ns, "applyUserProperties");
        if (JS_IsFunction(ctx, fn)) {
            BindThisLayer(ctx,
                          JS_IsUndefined(I->wrapped_layer) ? m_impl->host.default_layer
                                                           : I->wrapped_layer);
            m_impl->host.active_field_script = fs.get();
            JSValue arg                      = JS_DupValue(ctx, changed);
            JSValue r                        = JS_Call(ctx, fn, JS_UNDEFINED, 1, &arg);
            JS_FreeValue(ctx, arg);
            if (JS_IsException(r)) m_impl->LogError(ctx, I->sha, "applyUserProperties threw");
            JS_FreeValue(ctx, r);
        }
        JS_FreeValue(ctx, fn);
    }
    m_impl->host.active_field_script = nullptr;
    JS_FreeValue(ctx, changed);

    JS_FreeValue(ctx, props);
    JS_FreeValue(ctx, engine);
    JS_FreeValue(ctx, global);
}

void JsRuntime::SetMediaStatus(const MediaStatus& status) {
    if (! m_impl || ! m_impl->ctx) return;
    JSContext* ctx         = m_impl->ctx;
    auto&      host        = m_impl->host;
    const bool first       = ! host.media_initialized;
    const auto prev        = host.media;
    host.media             = status;
    host.media_initialized = true;

    const bool playback_changed   = first || prev.state != status.state;
    const bool properties_changed = first || prev.title != status.title ||
                                    prev.artist != status.artist || prev.album != status.album ||
                                    prev.album_artist != status.album_artist;
    const bool thumbnail_changed =
        first || prev.art_url != status.art_url || prev.previous_art_url != status.previous_art_url;
    if (! playback_changed && ! properties_changed && ! thumbnail_changed) return;

    for (auto& fs : m_impl->scripts) {
        auto* I = fs->m_impl.get();
        if (! I->alive) continue;
        BindThisLayer(
            ctx, JS_IsUndefined(I->wrapped_layer) ? m_impl->host.default_layer : I->wrapped_layer);
        m_impl->host.active_field_script = fs.get();
        if (playback_changed) {
            JSValue ev = MakeMediaPlaybackEvent(ctx, status);
            InvokeEventCallback(
                ctx, I->module_ns, "mediaPlaybackChanged", ev, m_impl.get(), I->sha);
            JS_FreeValue(ctx, ev);
        }
        if (properties_changed) {
            JSValue ev = MakeMediaPropertiesEvent(ctx, status);
            InvokeEventCallback(
                ctx, I->module_ns, "mediaPropertiesChanged", ev, m_impl.get(), I->sha);
            JS_FreeValue(ctx, ev);
        }
        if (thumbnail_changed) {
            JSValue ev = MakeMediaThumbnailEvent(ctx, status);
            InvokeEventCallback(
                ctx, I->module_ns, "mediaThumbnailChanged", ev, m_impl.get(), I->sha);
            JS_FreeValue(ctx, ev);
        }
    }
    m_impl->host.active_field_script = nullptr;
}

void JsRuntime::SetBoneResolvers(BoneIndexResolver     index_resolver,
                                 BoneTransformResolver transform_resolver) {
    m_impl->host.bone_index_resolver     = std::move(index_resolver);
    m_impl->host.bone_transform_resolver = std::move(transform_resolver);
}

void JsRuntime::SetPersistence(std::string path) {
    m_impl->host.ls_path = std::move(path);
    LoadLocalStorage(&m_impl->host);
}

namespace
{
void RunFieldScriptInit(JSContext* ctx, JsRuntime::Impl* rt, FieldScript* fs);
}

void JsRuntime::SetScene(owe::Scene* scene) {
    if (! m_impl) return;
    m_impl->host.scene = scene;
}

void JsRuntime::SetInitializationOrder(FieldScript& script, std::uint64_t order) {
    if (! script.m_impl || script.m_impl->rt != m_impl.get() || script.m_impl->init_done) return;
    script.m_impl->initialization_order = order;
}

void JsRuntime::AddAnimationEventSource(FieldScript&                    script,
                                        const owe::SceneAnimationCurve& curve) {
    if (! m_impl || ! script.m_impl || script.m_impl->rt != m_impl.get()) return;
    if (JS_IsUndefined(script.m_impl->animation_event_fn)) return;
    owe::SceneAnimationEventCursor cursor(curve);
    if (cursor.Empty()) return;
    script.m_impl->animation_cursors.push_back(rstd::move(cursor));
}

void JsRuntime::RegisterInitialLayerConfig(owe::SceneNode* node, Json config) {
    if (! m_impl || node == nullptr) return;
    (void)m_impl->host.initial_layer_configs.insert(node, rstd::move(config));
}

void JsRuntime::SetSceneRoot(owe::SceneNode* root) {
    if (! m_impl || ! m_impl->ctx) return;
    if (! JS_IsUndefined(m_impl->wrapped_scene)) JS_FreeValue(m_impl->ctx, m_impl->wrapped_scene);
    m_impl->scene_root      = root;
    m_impl->host.scene_root = root;
    m_impl->wrapped_scene   = root ? WrapLayerNode(m_impl->ctx, root) : JS_UNDEFINED;
    if (! JS_IsUndefined(m_impl->wrapped_scene)) BindThisScene(m_impl->ctx, m_impl->wrapped_scene);
    std::vector<FieldScript*> pending;
    pending.reserve(m_impl->scripts.size());
    for (auto& fs : m_impl->scripts)
        if (fs && fs->m_impl && ! fs->m_impl->init_done) pending.push_back(fs.get());
    std::stable_sort(pending.begin(), pending.end(), [](const auto* left, const auto* right) {
        return left->m_impl->initialization_order < right->m_impl->initialization_order;
    });
    for (auto* fs : pending) RunFieldScriptInit(m_impl->ctx, m_impl.get(), fs);
}

void JsRuntime::TickAll() {
    JSContext* ctx = m_impl->ctx;
    SweepDeferred(ctx, &m_impl->host);

    // Cursor event dispatch. For every script bound to a SceneNode, hit-
    // test the cursor against the node's world AABB and fire any of
    // cursorEnter/Leave/Move/Down/Up/Click that the script's module
    // exports. Runs before update() so update can react to state writes
    // the callbacks made this frame.
    const CursorWorld cursor      = CursorToWorld(m_impl->host.inputs);
    const bool        in_window   = m_impl->host.inputs.cursor_in_window;
    const uint32_t    btn_pressed = m_impl->host.inputs.mouse_buttons_pressed;
    const uint32_t    btn_release = m_impl->host.inputs.mouse_buttons_released;
    JSValue           ev_shared   = JS_UNDEFINED;
    auto              ensure_ev   = [&](int button) -> JSValue {
        if (! JS_IsUndefined(ev_shared)) JS_FreeValue(ctx, ev_shared);
        ev_shared = MakeCursorEvent(ctx, cursor, button);
        return ev_shared;
    };
    for (auto& fs : m_impl->scripts) {
        auto* I = fs->m_impl.get();
        if (! I->alive || ! I->node) continue;
        const bool now_inside = in_window && HitTestNode(I->node, cursor);
        BindThisLayer(
            ctx, JS_IsUndefined(I->wrapped_layer) ? m_impl->host.default_layer : I->wrapped_layer);
        m_impl->host.active_field_script = fs.get();
        if (now_inside != I->cursor_inside) {
            InvokeEventCallback(ctx,
                                I->module_ns,
                                now_inside ? "cursorEnter" : "cursorLeave",
                                ensure_ev(-1),
                                m_impl.get(),
                                I->sha);
            I->cursor_inside = now_inside;
        }
        if (now_inside) {
            InvokeEventCallback(
                ctx, I->module_ns, "cursorMove", ensure_ev(-1), m_impl.get(), I->sha);
        }
        if (btn_pressed && now_inside) {
            for (int b = 0; b < 3; ++b) {
                if (btn_pressed & (1u << b)) {
                    InvokeEventCallback(
                        ctx, I->module_ns, "cursorDown", ensure_ev(b), m_impl.get(), I->sha);
                    InvokeEventCallback(
                        ctx, I->module_ns, "cursorClick", ensure_ev(b), m_impl.get(), I->sha);
                }
            }
        }
        if (btn_release && now_inside) {
            for (int b = 0; b < 3; ++b) {
                if (btn_release & (1u << b)) {
                    InvokeEventCallback(
                        ctx, I->module_ns, "cursorUp", ensure_ev(b), m_impl.get(), I->sha);
                }
            }
        }
    }
    if (! JS_IsUndefined(ev_shared)) JS_FreeValue(ctx, ev_shared);

    // Timeline marker dispatch. Each bound curve advances on the clock the
    // renderer evaluates it on; the markers it passed since the previous
    // tick fire `animationEvent(event, value)`. Runs before update() so
    // update sees the state the callbacks wrote this frame.
    Vec<ref<str>> passed_markers;
    for (auto& fs : m_impl->scripts) {
        auto* I = fs->m_impl.get();
        if (! I->alive || I->animation_cursors.empty()) continue;
        passed_markers.clear();
        for (auto& cursor : I->animation_cursors)
            cursor.Advance(m_impl->host.inputs.runtime, passed_markers);
        if (passed_markers.is_empty()) continue;
        BindThisLayer(
            ctx, JS_IsUndefined(I->wrapped_layer) ? m_impl->host.default_layer : I->wrapped_layer);
        m_impl->host.active_field_script = fs.get();
        for (usize index {}; index < passed_markers.len(); ++index) {
            JSValue args[2] = { MakeAnimationEvent(ctx, passed_markers[index]),
                                JS_DupValue(ctx, I->current_value) };
            JSValue ret     = JS_Call(ctx, I->animation_event_fn, JS_UNDEFINED, 2, args);
            JS_FreeValue(ctx, args[0]);
            JS_FreeValue(ctx, args[1]);
            if (JS_IsException(ret)) {
                m_impl->LogError(ctx, I->sha, "animationEvent threw");
                JS_FreeValue(ctx, ret);
                continue;
            }
            // The callback may hand back a replacement property value, the
            // same way update() does; keep it for the update() that follows.
            auto value = CoerceReturn(ctx, ret, I->kind);
            if (! std::holds_alternative<std::monostate>(value)) {
                JSValue next_value = ScriptValueToJs(ctx, value);
                JS_FreeValue(ctx, I->current_value);
                I->current_value = next_value;
                I->last_value    = std::move(value);
            }
            JS_FreeValue(ctx, ret);
        }
    }
    m_impl->host.active_field_script = nullptr;

    for (auto& fs : m_impl->scripts) {
        auto* I = fs->m_impl.get();
        if (! I->alive) continue;
        if (JS_IsUndefined(I->update_fn)) continue;
        // Swap `thisLayer` to this script's bound node before update. When
        // unbound, restore the original stub captured at bootstrap.
        BindThisLayer(
            ctx, JS_IsUndefined(I->wrapped_layer) ? m_impl->host.default_layer : I->wrapped_layer);
        m_impl->host.active_field_script = fs.get();
        JSValue ret;
        if (I->update_takes_arg) {
            JSValue args[1] = { JS_DupValue(ctx, I->current_value) };
            ret             = JS_Call(ctx, I->update_fn, JS_UNDEFINED, 1, args);
            JS_FreeValue(ctx, args[0]);
        } else {
            ret = JS_Call(ctx, I->update_fn, JS_UNDEFINED, 0, nullptr);
        }
        if (JS_IsException(ret)) {
            m_impl->LogError(ctx, I->sha, "update threw");
            JS_FreeValue(ctx, ret);
            continue;
        }
        I->last_value = CoerceReturn(ctx, ret, I->kind);
        // Keep the next argument in the field's coerced shape. Vec3 scripts
        // often return a scalar for scale, but still read value.x next frame.
        if (I->update_takes_arg && ! std::holds_alternative<std::monostate>(I->last_value)) {
            JSValue next_value = ScriptValueToJs(ctx, I->last_value);
            JS_FreeValue(ctx, I->current_value);
            I->current_value = next_value;
        }
        JS_FreeValue(ctx, ret);
    }
    m_impl->host.active_field_script = nullptr;
}

void JsRuntime::ForEachScript(EachFn fn, void* user) {
    for (auto& fs : m_impl->scripts) fn(fs.get(), user);
}

void JsRuntime::RegisterTextSetter(owe::SceneNode*                       node,
                                   std::function<void(std::string_view)> setter) {
    if (node == nullptr) return;
    m_impl->host.text_setters[node] = std::move(setter);
}

void JsRuntime::RegisterTextAlignSetters(owe::SceneNode* node, std::string horizontal,
                                         std::string vertical, double point_size,
                                         std::function<void(std::string_view)> set_horizontal,
                                         std::function<void(std::string_view)> set_vertical,
                                         std::function<double()>               get_point_size,
                                         std::function<void(double)>           set_point_size) {
    if (node == nullptr) return;
    m_impl->host.text_align_hooks[node] = EngineHostState::TextAlignHooks {
        .horizontal     = std::move(horizontal),
        .vertical       = std::move(vertical),
        .point_size     = point_size,
        .set_horizontal = std::move(set_horizontal),
        .set_vertical   = std::move(set_vertical),
        .get_point_size = std::move(get_point_size),
        .set_point_size = std::move(set_point_size),
    };
}

void JsRuntime::RegisterNodeOriginAccessors(owe::SceneNode* node, NodeOriginGetter getter,
                                            NodeOriginSetter setter) {
    if (node == nullptr) return;
    (void)m_impl->host.node_origin_hooks.insert(
        node,
        EngineHostState::NodeOriginHooks { .getter = rstd::move(getter),
                                           .setter = rstd::move(setter) });
}

void JsRuntime::RegisterImageAlignmentSetter(owe::SceneNode* node, ref<str> alignment,
                                             ImageAlignmentSetter setter) {
    if (node == nullptr) return;
    (void)m_impl->host.image_alignment_hooks.insert(node,
                                                    EngineHostState::ImageAlignmentHook {
                                                        .alignment = String::make(alignment),
                                                        .setter    = rstd::move(setter),
                                                    });
}

void JsRuntime::CloneImageAlignmentBinding(owe::SceneNode* source, owe::SceneNode* clone) {
    if (source == nullptr || clone == nullptr) return;
    auto hook = m_impl->host.image_alignment_hooks.get(source);
    if (hook.is_none()) return;
    (void)m_impl->host.image_alignment_hooks.insert(
        clone,
        EngineHostState::ImageAlignmentHook { .alignment = (**hook).alignment.clone(),
                                              .setter    = (**hook).setter.clone() });
}

void JsRuntime::SetLayerFactory(LayerFactory factory) {
    m_impl->host.layer_factory = Some(rstd::move(factory));
}

void JsRuntime::SetLayerConfigFactory(LayerConfigFactory factory) {
    m_impl->host.layer_config_factory = Some(rstd::move(factory));
}

void JsRuntime::ClearLayerFactory() { m_impl->host.layer_factory = None(); }

void JsRuntime::ClearLayerConfigFactory() { m_impl->host.layer_config_factory = None(); }

// --- Module load + FieldScript construction ---------------------------------

namespace
{

// Discover whether `update` takes an argument by inspecting `length`.
// JS function objects have a `length` property = formal parameter count.
bool FunctionTakesArg(JSContext* ctx, JSValue fn) {
    JSValue len = JS_GetPropertyStr(ctx, fn, "length");
    int32_t n   = 0;
    JS_ToInt32(ctx, &n, len);
    JS_FreeValue(ctx, len);
    return n >= 1;
}

void RunFieldScriptInit(JSContext* ctx, JsRuntime::Impl* rt, FieldScript* fs) {
    if (! fs || ! fs->m_impl || fs->m_impl->init_done) return;
    auto* I = fs->m_impl.get();
    if (! JS_IsFunction(ctx, I->init_fn)) {
        I->init_done = true;
        return;
    }

    BindThisLayer(ctx,
                  JS_IsUndefined(I->wrapped_layer) ? rt->host.default_layer : I->wrapped_layer);
    if (! JS_IsUndefined(rt->wrapped_scene)) BindThisScene(ctx, rt->wrapped_scene);
    rt->host.active_field_script = fs;
    JSValue arg                  = JS_DupValue(ctx, I->current_value);
    JSValue r                    = JS_Call(ctx, I->init_fn, JS_UNDEFINED, 1, &arg);
    JS_FreeValue(ctx, arg);
    if (JS_IsException(r)) rt->LogError(ctx, I->sha, "init threw");
    JS_FreeValue(ctx, r);
    rt->host.active_field_script = nullptr;
    I->init_done                 = true;
}

JSValue AwaitModuleEvaluation(JSContext* ctx, JSValue value) {
    for (;;) {
        switch (JS_PromiseState(ctx, value)) {
        case JS_PROMISE_FULFILLED: {
            JSValue result = JS_PromiseResult(ctx, value);
            JS_FreeValue(ctx, value);
            return result;
        }
        case JS_PROMISE_REJECTED: {
            JSValue reason = JS_PromiseResult(ctx, value);
            JS_FreeValue(ctx, value);
            return JS_Throw(ctx, reason);
        }
        case JS_PROMISE_PENDING: {
            JSContext* job_ctx = nullptr;
            int        status  = JS_ExecutePendingJob(JS_GetRuntime(ctx), &job_ctx);
            if (status > 0) continue;
            JS_FreeValue(ctx, value);
            if (status == 0) {
                return JS_ThrowInternalError(ctx, "module evaluation is pending without a job");
            }
            JSContext* exception_ctx = job_ctx != nullptr ? job_ctx : ctx;
            return JS_Throw(ctx, JS_GetException(exception_ctx));
        }
        case JS_PROMISE_NOT_A_PROMISE: return value;
        }
    }
}

} // namespace

FieldScript* JsRuntime::MakeFieldScript(std::string_view source, std::string_view script_sha,
                                        FieldKind field_kind_in, const Json& properties_config,
                                        const Json& initial_value, owe::SceneNode* node) {
    JSContext* ctx = m_impl->ctx;
    if (! ctx) return nullptr;
    m_impl->host.pending_registered_assets.clear();

    // Wrap `node` (if any) up front. Bind it as `thisLayer` for the
    // duration of module eval + init so module-body top-level statements
    // like `let parent = thisLayer.getParent()` see the real node.
    JSValue wrapped = node ? WrapLayerNode(ctx, node) : JS_UNDEFINED;
    BindThisLayer(ctx, JS_IsUndefined(wrapped) ? m_impl->host.default_layer : wrapped);

    // 1. Compile + evaluate the module fresh per FieldScript. Caching by
    //    source-sha would share `scriptProperties._hostValues` across all
    //    instances using the same source — workshop wallpapers commonly
    //    reuse the position-template script across many layers (each with
    //    distinct {user, value} bindings), so a shared _hostValues makes
    //    every instance read whichever binding was wired last.
    JSValue       ns;
    auto          sha_str = std::string(script_sha);
    std::uint64_t uniq    = m_impl->next_module_serial++;
    std::string   fname   = "scripts/" + sha_str + "-" + std::to_string(uniq) + ".js";
    {
        JSValue compiled = JS_Eval(ctx,
                                   source.data(),
                                   source.size(),
                                   fname.c_str(),
                                   JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
        if (JS_IsException(compiled)) {
            m_impl->LogError(ctx, script_sha, "compile failed");
            JS_FreeValue(ctx, compiled);
            if (! JS_IsUndefined(wrapped)) JS_FreeValue(ctx, wrapped);
            return nullptr;
        }
        JSModuleDef* m  = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(compiled));
        JSValue      ev = AwaitModuleEvaluation(ctx, JS_EvalFunction(ctx, compiled));
        if (JS_IsException(ev)) {
            m_impl->LogError(ctx, script_sha, "module eval failed");
            JS_FreeValue(ctx, ev);
            if (! JS_IsUndefined(wrapped)) JS_FreeValue(ctx, wrapped);
            return nullptr;
        }
        JS_FreeValue(ctx, ev);
        ns = JS_GetModuleNamespace(ctx, m);
    }

    // 2. Build the FieldScript handle.
    auto  fs         = std::make_unique<FieldScript>();
    auto* I          = fs->m_impl.get();
    I->rt            = m_impl.get();
    I->ctx           = ctx;
    I->sha           = sha_str;
    I->kind          = (field_kind_in == FieldKind::Unknown) ? FieldKind::Scalar : field_kind_in;
    I->module_ns     = ns; // owns one ref now
    I->node          = node;
    I->wrapped_layer = wrapped; // takes ownership; freed in JsRuntime dtor
    I->registered_assets = rstd::move(m_impl->host.pending_registered_assets);
    JSValue workshop_id  = JS_GetPropertyStr(ctx, ns, "__workshopId");
    if (JS_IsString(workshop_id)) {
        const char* value = JS_ToCString(ctx, workshop_id);
        if (value != nullptr) {
            I->workshop_id = String::make(rstd::cppstd::as_str(value).unwrap());
            JS_FreeCString(ctx, value);
        }
    }
    JS_FreeValue(ctx, workshop_id);
    // 3. Wire scriptProperties._hostValues from the per-binding config so
    //    `scriptProperties.foo` returns the configured value (resolving
    //    {user, value} to value) instead of the JS-default.
    JSValue sp = JS_GetPropertyStr(ctx, ns, "scriptProperties");
    if (! JS_IsUndefined(sp)) {
        JSValue hv = JS_GetPropertyStr(ctx, sp, "__hostValues");
        if (JS_IsObject(hv) && properties_config.is_object()) {
            auto object = properties_config.as_object();
            (*object)->iter().for_each([&](auto entry) {
                auto [entry_key, entry_value] = entry;
                auto        owned_key         = rstd::cppstd::to_string(entry_key->as_str());
                const auto& value             = *entry_value;
                JS_DefinePropertyValueStr(
                    ctx, hv, owned_key.c_str(), ResolveConfigValue(ctx, value), JS_PROP_C_W_E);
            });
        }
        JS_FreeValue(ctx, hv);
    }
    JS_FreeValue(ctx, sp);

    // `init` runs after SetSceneRoot so thisScene queries see the complete tree.
    JSValue init_fn  = JS_GetPropertyStr(ctx, ns, "init");
    JSValue init_arg = CoerceInitialValue(ctx, initial_value, I->kind);
    if (JS_IsFunction(ctx, init_fn)) {
        I->init_fn = init_fn;
    } else {
        JS_FreeValue(ctx, init_fn);
    }

    // Cache `update` for the per-frame tick.
    JSValue update_fn = JS_GetPropertyStr(ctx, ns, "update");
    if (JS_IsFunction(ctx, update_fn)) {
        I->update_fn        = update_fn;
        I->update_takes_arg = FunctionTakesArg(ctx, update_fn);
    } else {
        JS_FreeValue(ctx, update_fn);
        I->update_fn = JS_UNDEFINED;
    }
    // Cache `animationEvent` the same way; timeline markers dispatch into it.
    JSValue animation_event_fn = JS_GetPropertyStr(ctx, ns, "animationEvent");
    if (JS_IsFunction(ctx, animation_event_fn)) {
        I->animation_event_fn = animation_event_fn;
    } else {
        JS_FreeValue(ctx, animation_event_fn);
        I->animation_event_fn = JS_UNDEFINED;
    }
    // Reuse the coerced initial value as the seed for (value)-form
    // updates so the first frame's `update(value)` sees a Vec3, not a
    // raw string.
    I->current_value = init_arg;

    auto* raw = fs.get();
    m_impl->scripts.push_back(std::move(fs));
    if (m_impl->scene_root) RunFieldScriptInit(ctx, m_impl.get(), raw);
    return raw;
}

// ---------------------------------------------------------------------------
// ScriptScene — per-Scene runtime + actuator drain.
// ---------------------------------------------------------------------------

struct ScriptScene::Impl {
    JsRuntime             rt;
    std::vector<Actuator> actuators;
};

ScriptScene::ScriptScene(Option<Arc<AudioResponseDemand>> demand)
    : m_impl(std::make_unique<Impl>()) {
    m_impl->rt.SetAudioResponseDemand(rstd::move(demand));
}
ScriptScene::~ScriptScene() = default;

JsRuntime& ScriptScene::runtime() noexcept { return m_impl->rt; }
void       ScriptScene::AddActuator(Actuator a) { m_impl->actuators.push_back(a); }
// Empty = no scripts AND no actuators. Visibility-bound side-effect-only
// scripts (audio bar fanout) don't register an actuator but still need
// their TickAll to run, so emptiness must also consult the runtime.
bool ScriptScene::empty() const noexcept {
    if (! m_impl->actuators.empty()) return false;
    bool has_script = false;
    m_impl->rt.ForEachScript(
        [](script::FieldScript*, void* u) {
            *static_cast<bool*>(u) = true;
        },
        &has_script);
    return ! has_script;
}

struct SceneNodeArcCapture {
    rstd::sync::Arc<owe::SceneNode> node;

    explicit SceneNodeArcCapture(rstd::sync::Arc<owe::SceneNode> n): node(rstd::move(n)) {}
    SceneNodeArcCapture(const SceneNodeArcCapture& other): node(other.node.clone()) {}
    SceneNodeArcCapture(SceneNodeArcCapture&&) noexcept            = default;
    SceneNodeArcCapture& operator=(SceneNodeArcCapture&&) noexcept = default;
    SceneNodeArcCapture& operator=(const SceneNodeArcCapture&)     = delete;
};

std::function<void(const ScriptValue&)> MakeNodeTransformApply(rstd::sync::Arc<owe::SceneNode> node,
                                                               NodeTransformTarget target) {
    return [hold = SceneNodeArcCapture(rstd::move(node)), target](const ScriptValue& v) {
        if (std::holds_alternative<std::monostate>(v)) return;
        auto& node = hold.node;

        // Script angle values are degrees; node rotation is radians. Read the
        // current rotation back as degrees so partial (Vec2 / scalar) updates
        // compose in the same unit the script works in.
        Eigen::Vector3f current = [&] {
            switch (target) {
            case NodeTransformTarget::Translate: return node->Translate();
            case NodeTransformTarget::Scale: return node->Scale();
            case NodeTransformTarget::Rotation:
                return Eigen::Vector3f { node->Rotation() * float(kRadToDeg) };
            }
            return Eigen::Vector3f { 0.0f, 0.0f, 0.0f };
        }();

        Eigen::Vector3f next = current;
        if (auto* p = std::get_if<Vec3Value>(&v)) {
            next = Eigen::Vector3f { static_cast<float>(p->x),
                                     static_cast<float>(p->y),
                                     static_cast<float>(p->z) };
        } else if (auto* p = std::get_if<Vec2Value>(&v)) {
            next =
                Eigen::Vector3f { static_cast<float>(p->x), static_cast<float>(p->y), current.z() };
        } else if (auto* p = std::get_if<ScalarValue>(&v)) {
            // Scalar splats across all three axes for scale; falls back to
            // current.x for translate/rotation (rare but seen in the corpus
            // when scripts mistakenly bind to the wrong field kind).
            if (target == NodeTransformTarget::Scale) {
                float s = static_cast<float>(p->v);
                next    = Eigen::Vector3f { s, s, s };
            } else {
                next.x() = static_cast<float>(p->v);
            }
        } else {
            return;
        }

        switch (target) {
        case NodeTransformTarget::Translate: node->SetTranslate(next); break;
        case NodeTransformTarget::Scale: node->SetScale(next); break;
        case NodeTransformTarget::Rotation: node->SetRotation(next * float(kDegToRad)); break;
        }
    };
}

std::function<void(const ScriptValue&)> MakeNodeAlphaApply(rstd::sync::Arc<owe::SceneNode> node) {
    return [hold = SceneNodeArcCapture(rstd::move(node))](const ScriptValue& v) {
        if (std::holds_alternative<std::monostate>(v)) return;
        auto& node = hold.node;

        if (auto* p = std::get_if<ScalarValue>(&v)) {
            node->SetUserAlpha(static_cast<float>(p->v));
        } else if (auto* p = std::get_if<BoolValue>(&v)) {
            node->SetUserAlpha(p->v ? 1.0f : 0.0f);
        } else if (auto* p = std::get_if<Vec2Value>(&v)) {
            node->SetUserAlpha(static_cast<float>(p->x));
        } else if (auto* p = std::get_if<Vec3Value>(&v)) {
            node->SetUserAlpha(static_cast<float>(p->x));
        }
    };
}

std::function<void(const ScriptValue&)> MakeNodeVolumeApply(rstd::sync::Arc<owe::SceneNode> node) {
    return [hold = SceneNodeArcCapture(rstd::move(node))](const ScriptValue& value) {
        if (auto* volume = std::get_if<ScalarValue>(&value)) {
            hold.node->SetVolume(static_cast<float>(volume->v));
        } else if (auto* volume = std::get_if<BoolValue>(&value)) {
            hold.node->SetVolume(volume->v ? 1.0f : 0.0f);
        } else if (auto* volume = std::get_if<Vec2Value>(&value)) {
            hold.node->SetVolume(static_cast<float>(volume->x));
        } else if (auto* volume = std::get_if<Vec3Value>(&value)) {
            hold.node->SetVolume(static_cast<float>(volume->x));
        }
    };
}

std::function<void(const ScriptValue&)> MakeNodeColorApply(rstd::sync::Arc<owe::SceneNode> node) {
    return [hold = SceneNodeArcCapture(rstd::move(node))](const ScriptValue& value) {
        auto& current = hold.node->Color();
        if (auto* color = std::get_if<Vec3Value>(&value)) {
            hold.node->SetColor({ static_cast<float>(color->x),
                                  static_cast<float>(color->y),
                                  static_cast<float>(color->z) });
        } else if (auto* color = std::get_if<Vec2Value>(&value)) {
            hold.node->SetColor(
                { static_cast<float>(color->x), static_cast<float>(color->y), current.z() });
        } else if (auto* color = std::get_if<ScalarValue>(&value)) {
            const auto component = static_cast<float>(color->v);
            hold.node->SetColor({ component, component, component });
        }
    };
}

void ScriptScene::Tick(const FrameInputs& fi) {
    m_impl->rt.SetFrameInputs(fi);
    m_impl->rt.TickAll();
    for (const auto& a : m_impl->actuators) {
        if (! a.script || ! a.apply) continue;
        a.apply(a.script->last_value());
    }
}

void InstallScriptScene(owe::Scene& scene, Box<ScriptScene> script_scene) {
    scene.InstallExtension(rstd::move(script_scene));
}

void TickSceneScripts(owe::Scene& scene, const FrameInputs& fi) {
    auto script_scene = scene.ExtensionMut<ScriptScene>();
    if (script_scene.is_none()) return;
    (**script_scene).Tick(fi);
}

void SetSceneUserProperty(owe::Scene& scene, std::string_view key, const Json& property) {
    auto script_scene = scene.ExtensionMut<ScriptScene>();
    if (script_scene.is_some()) (**script_scene).runtime().SetUserProperty(key, property);
    scene.ApplyUserLightVisibilityBindings(key, property);
}

void SetSceneMediaStatus(owe::Scene& scene, const MediaStatus& status) {
    auto script_scene = scene.ExtensionMut<ScriptScene>();
    if (script_scene.is_some()) (**script_scene).runtime().SetMediaStatus(status);
}

void SetScenePersistence(owe::Scene& scene, std::string path) {
    auto script_scene = scene.ExtensionMut<ScriptScene>();
    if (script_scene.is_none()) return;
    (**script_scene).runtime().SetPersistence(std::move(path));
}

} // namespace owe::script
