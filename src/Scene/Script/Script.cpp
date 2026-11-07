module;

#include <rstd/macro.hpp>
#include <rstd/enum.hpp>
#include "quickjs.h"

module wescene.script;
import :programs;
import :conversion;
import eigen;
import owe.scene_audio_response;
import rstd;
import rstd.log;
import rstd.cppstd;
import wescene.types;
import wescene.scene;

using rstd::ffi::CStr;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::collections::HashMap;
using rstd::collections::HashSet;
using rstd::ffi::CString;
using rstd::path::PathBuf;
using rstd::slice_::sort_unstable_by;
using rstd::sync::Arc;

namespace owe::script
{

using conversion::CoerceInitialValue;
using conversion::CoerceReturn;
using conversion::JsonToJs;
using conversion::ResolveConfigValue;
using conversion::ScriptValueToJs;
using conversion::UserPropertyValueToJs;

// ---------------------------------------------------------------------------
// FrameInputs storage. JSRuntime opaque data: a per-context FrameInputs
// snapshot the engine.* getters consult on each call.
// ---------------------------------------------------------------------------

// One scheduled engine.setTimeout / setInterval entry. fn is an owned ref;
// dead entries are tombstoned during sweep and compacted afterwards so
// callbacks that schedule more callbacks don't iterate over invalid storage.
struct DeferredCb {
    rstd::uint32_t handle;
    double         fire_at;    // engine.runtime seconds when due
    double         interval_s; // for setInterval; 0 for setTimeout
    JSValue        fn;         // owned
    FieldScript*   owner { nullptr };
    bool           repeating;
    bool           dead;
};

struct AudioBufferSlot {
    rstd::uint32_t resolution;
    JSValue        object { JS_UNDEFINED };
};

struct ScriptLayerState {
    owe::SceneNode* node { nullptr };
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
    // Keep layer identity stable for thisLayer/getLayer/enumerateLayers comparisons.
    HashMap<owe::SceneNode*, JSValue>               layer_objects;
    HashMap<owe::SceneNode*, Arc<ScriptLayerState>> layer_states;
    HashSet<owe::SceneNode*>                        script_layers;
    // Keep registered queries enabled even when the scene has no wallpaper layers.
    bool registered_layer_queries { false };
    // engine.setTimeout / setInterval queue. Swept once per frame in
    // JsRuntime::TickAll before the script update loop runs.
    Vec<DeferredCb> deferred;
    rstd::uint32_t  next_handle { 1 };
    // localStorage backing. Values are JSON-serialised strings so we can
    // round-trip arbitrary script values through the persistence file.
    // Empty `ls_path` means in-memory only (the legacy bootstrap shape).
    HashMap<String, String> ls_data;
    // Keep the last unpublished operation per key; None represents deletion.
    HashMap<String, Option<String>> ls_pending;
    PathBuf                         ls_path;
    // Set around every init/update/cursor invocation so host callbacks can
    // resolve the owning field binding.
    FieldScript*                             active_field_script { nullptr };
    Option<Arc<owe::SceneAnimationPlayback>> active_animation;
    Vec<String>                              pending_registered_assets;
    Option<JsRuntime::LayerFactory>          layer_factory;
    Option<JsRuntime::LayerConfigFactory>    layer_config_factory;
    // SceneNode -> text-content setter. Populated by text layers in the
    // parser; consulted by NodeSetText so `thisLayer.text = "..."` reaches
    // TextLayouter::SetText. Missing entry means the layer is not text-
    // capable; writes silently no-op.
    HashMap<owe::SceneNode*, JsRuntime::TextSetter> text_setters;
    HashMap<owe::SceneNode*, String>                current_text_strings;
    struct TextAlignHooks {
        String                             horizontal { "center"_Str };
        String                             vertical { "center"_Str };
        double                             point_size { 1.0 };
        JsRuntime::TextAlignSetter         set_horizontal;
        JsRuntime::TextAlignSetter         set_vertical;
        Option<JsRuntime::PointSizeGetter> get_point_size;
        Option<JsRuntime::PointSizeSetter> set_point_size;
    };
    HashMap<owe::SceneNode*, TextAlignHooks> text_align_hooks;
    struct NodeOriginHooks {
        JsRuntime::NodeOriginGetter getter;
        JsRuntime::NodeOriginSetter setter;
    };
    HashMap<owe::SceneNode*, NodeOriginHooks>  node_origin_hooks;
    Option<JsRuntime::NodeParallaxDepthGetter> node_parallax_depth_getter;
    Option<JsRuntime::NodeParallaxDepthSetter> node_parallax_depth_setter;
    struct ImageAlignmentHook {
        String                          alignment;
        JsRuntime::ImageAlignmentSetter setter;
    };
    HashMap<owe::SceneNode*, ImageAlignmentHook> image_alignment_hooks;
    HashMap<owe::SceneNode*, Json>               initial_layer_configs;
    Option<JsRuntime::BoneIndexResolver>         bone_index_resolver;
    Option<JsRuntime::BoneCountResolver>         bone_count_resolver;
    Option<JsRuntime::BoneTransformResolver>     bone_transform_resolver;
    Option<JsRuntime::BoneSetTransformResolver>  bone_set_transform_resolver;
    owe::SceneNode*                              scene_root { nullptr };
};

class EngineBindingGuard : NoCopy, NoMove {
public:
    EngineBindingGuard(JSContext* context, EngineHostState& state)
        : ctx(context), host(state), active_field_script(state.active_field_script) {
        global      = JS_GetGlobalObject(ctx);
        this_layer  = JS_GetPropertyStr(ctx, global, "thisLayer");
        this_object = JS_GetPropertyStr(ctx, global, "thisObject");
        this_scene  = JS_GetPropertyStr(ctx, global, "thisScene");
        if (state.active_animation.is_some())
            active_animation = Some((*state.active_animation).clone());
    }

    ~EngineBindingGuard() {
        JS_SetPropertyStr(ctx, global, "thisLayer", this_layer);
        JS_SetPropertyStr(ctx, global, "thisObject", this_object);
        JS_SetPropertyStr(ctx, global, "thisScene", this_scene);
        JS_FreeValue(ctx, global);
        host.active_field_script = active_field_script;
        host.active_animation    = rstd::move(active_animation);
    }

private:
    JSContext*                               ctx { nullptr };
    EngineHostState&                         host;
    FieldScript*                             active_field_script { nullptr };
    Option<Arc<owe::SceneAnimationPlayback>> active_animation;
    JSValue                                  global { JS_UNDEFINED };
    JSValue                                  this_layer { JS_UNDEFINED };
    JSValue                                  this_object { JS_UNDEFINED };
    JSValue                                  this_scene { JS_UNDEFINED };
};

rstd::uint32_t NormalizeAudioResolution(int32_t requested) {
    if (requested <= 16) return 16;
    if (requested <= 32) return 32;
    return 64;
}

AudioBufferSlot& AudioBufferForResolution(EngineHostState& host, rstd::uint32_t resolution) {
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
    String           sha;
    FieldKind        kind { FieldKind::Unknown };
    JSValue          module_ns { JS_UNDEFINED };
    JSValue          init_fn { JS_UNDEFINED };
    JSValue          update_fn { JS_UNDEFINED };
    JSValue          animation_event_fn { JS_UNDEFINED };
    JSValue          deferred_module { JS_UNDEFINED };
    Option<Json>     deferred_properties;
    Option<Json>     deferred_initial_value;
    bool             update_takes_arg { false };
    bool             init_done { false };
    bool             init_in_progress { false };
    bool             init_deferred { false };
    rstd::uint64_t   initialization_order { 0 };
    JSValue          current_value {
        JS_UNDEFINED
    }; // last `value` returned, kept as JSValue for the (value)-arg form
    ScriptValue last_value;
    bool        alive { true };
    bool        error_logged { false };
    // Layer-B: the SceneNode this script's `thisLayer` resolves to. Null →
    // fall back to the generic JS stub. `wrapped_layer` caches the JSValue
    // wrapper so per-frame swap doesn't reallocate.
    owe::SceneNode*          node { nullptr };
    JSValue                  wrapped_layer { JS_UNDEFINED };
    JSValue                  wrapped_object { JS_UNDEFINED };
    String                   property;
    ScriptPropertyObjectKind object_kind { ScriptPropertyObjectKind::Layer };
    // Per-script cursor-inside-bbox state used to edge-detect
    // cursorEnter / cursorLeave between frames.
    bool                                     cursor_inside { false };
    Option<Arc<owe::SceneAnimationPlayback>> animation;
    Option<Arc<owe::SceneAnimationLayer>>    animation_layer;
    Vec<String>                              registered_assets;
    String                                   workshop_id;
};

auto ScriptBindingContext::ForLayer(owe::SceneNode* layer, ref<str> property,
                                    Option<Arc<owe::SceneAnimationPlayback>> animation)
    -> ScriptBindingContext {
    ScriptBindingContext context(layer);
    context.property  = String::make(property);
    context.animation = rstd::move(animation);
    return context;
}

auto ScriptBindingContext::ForAnimationLayer(owe::SceneNode* layer, ref<str> property,
                                             Arc<owe::SceneAnimationPlayback> animation)
    -> ScriptBindingContext {
    auto context        = ForLayer(layer, property, Some(rstd::move(animation)));
    context.object_kind = ScriptPropertyObjectKind::AnimationLayer;
    return context;
}

auto ScriptBindingContext::ForAnimationLayer(owe::SceneNode* layer, ref<str> property,
                                             Arc<owe::SceneAnimationLayer> animation_layer,
                                             Option<Arc<owe::SceneAnimationPlayback>> animation)
    -> ScriptBindingContext {
    auto context            = ForLayer(layer, property, rstd::move(animation));
    context.object_kind     = ScriptPropertyObjectKind::AnimationLayer;
    context.animation_layer = Some(rstd::move(animation_layer));
    return context;
}

auto ScriptBindingContext::ForEffect(owe::SceneNode* layer, owe::SceneImageEffectRef effect,
                                     ref<str>                                 property,
                                     Option<Arc<owe::SceneAnimationPlayback>> animation)
    -> ScriptBindingContext {
    auto context        = ForLayer(layer, property, rstd::move(animation));
    context.object_kind = ScriptPropertyObjectKind::Effect;
    context.effect      = Some(effect);
    return context;
}

auto ScriptBindingContext::ForMaterial(owe::SceneNode* layer, owe::SceneMaterial* material,
                                       ref<str>                                 property,
                                       Option<Arc<owe::SceneAnimationPlayback>> animation)
    -> ScriptBindingContext {
    auto context        = ForLayer(layer, property, rstd::move(animation));
    context.object_kind = ScriptPropertyObjectKind::Material;
    context.material    = material;
    return context;
}

FieldScript::FieldScript(): m_impl(Box<Impl>::make()) {}
FieldScript::~FieldScript() = default;
FieldKind          FieldScript::field_kind() const noexcept { return m_impl->kind; }
const ScriptValue& FieldScript::last_value() const noexcept { return m_impl->last_value; }
bool               FieldScript::alive() const noexcept { return m_impl->alive; }
bool     FieldScript::initialization_deferred() const noexcept { return m_impl->init_deferred; }
ref<str> FieldScript::script_sha() const noexcept { return m_impl->sha.as_str(); }
slice<String> FieldScript::RegisteredAssets() const noexcept {
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
    // Last pointer sample consumed by TickAll; frame ticks are not motion.
    float previous_cursor_x { 0.0f }, previous_cursor_y { 0.0f };
    bool  cursor_was_in_window { false };
    // Compiled-module dedup: same script source under the same sha is
    // imported once per runtime, exposing one shared namespace. A
    // FieldScript holds a JS_DupValue of the namespace.
    HashMap<String, JSValue> ns_by_sha;
    rstd::uint64_t           next_module_serial { 0 };
    Vec<Box<FieldScript>>    scripts;
    Vec<Box<FieldScript>>    deferred_scripts;
    // Set of error-logged shas to log once.
    HashSet<String> errored;
    // Scene root for `thisScene`. Wrapped lazily; freed in dtor.
    owe::SceneNode* scene_root { nullptr };
    JSValue         wrapped_scene { JS_UNDEFINED };

    void LogError(JSContext* c, ref<str> sha, const char* what) {
        if (errored.contains(sha)) return;
        (void)errored.insert(rstd::into(sha));
        JSValue     exc = JS_GetException(c);
        const char* msg = JS_ToCString(c, exc);
        rstd_error("script[{}] {}: {}", sha, what, msg ? msg : "<no message>");
        if (msg) JS_FreeCString(c, msg);
        JSValue stack = JS_GetPropertyStr(c, exc, "stack");
        if (! JS_IsUndefined(stack) && ! JS_IsNull(stack)) {
            const char* stack_msg = JS_ToCString(c, stack);
            if (stack_msg && stack_msg[0] != '\0') {
                rstd_error("script[{}] stack:\n{}", sha, stack_msg);
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

void SetAudioArrayValue(JSContext* ctx, JSValueConst arr, rstd::uint32_t index, float value) {
    JS_DefinePropertyValueUint32(ctx, arr, index, JS_NewFloat64(ctx, value), JS_PROP_C_W_E);
}

JSValue MakeAudioBuffer(JSContext* ctx, const FrameInputs& inputs, rstd::uint32_t resolution) {
    JSValue object  = JS_NewObject(ctx);
    JSValue left    = JS_NewArray(ctx);
    JSValue right   = JS_NewArray(ctx);
    JSValue average = JS_NewArray(ctx);
    JSValue buffer  = JS_NewArray(ctx);
    for (rstd::uint32_t index = 0; index < resolution; ++index) {
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
    for (rstd::uint32_t index = 0; index < slot.resolution; ++index) {
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
        if (d.handle == rstd::uint32_t(h)) {
            d.dead = true;
            break;
        }
    }
    return JS_UNDEFINED;
}

JSValue MakeCancelFn(JSContext* ctx, rstd::uint32_t handle) {
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
    double         interval_s = ms / 1000.0;
    rstd::uint32_t h          = host->next_handle++;
    host->deferred.push(DeferredCb {
        .handle     = h,
        .fire_at    = host->inputs.runtime + interval_s,
        .interval_s = interval_s,
        .fn         = JS_DupValue(ctx, argv[0]),
        .owner      = host->active_field_script,
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
PathBuf LocalStorageSidecar(const PathBuf& path, ref<str> suffix) {
    auto name = path.as_path().as_os_str().to_os_string();
    name.push(suffix);
    return PathBuf::from(rstd::move(name));
}

Option<PathBuf> ResolveLocalStoragePath(const PathBuf& path) {
    auto target = path.clone();
    // Resolve aliases before selecting the lock or replacing the data file.
    for (int depth = 0; depth < 40; ++depth) {
        auto canonical = rstd::fs::canonicalize(target.as_path());
        if (canonical.is_ok()) return Some(canonical.unwrap());
        if (canonical.unwrap_err().kind().code != rstd::io::ErrorKind::NotFound) return None();
        auto parent = target.as_path().parent();
        auto name   = target.as_path().file_name();
        if (parent.is_none() || name.is_none()) return None();
        auto directory = parent->is_empty() ? PathBuf::from("."_str) : PathBuf::from(*parent);
        auto link      = rstd::fs::read_link(target.as_path());
        if (link.is_ok()) {
            target = directory.join(link.unwrap().as_path());
            continue;
        }
        auto metadata = rstd::fs::symlink_metadata(target.as_path());
        if (metadata.is_ok() || metadata.unwrap_err().kind().code != rstd::io::ErrorKind::NotFound)
            return None();
        auto resolved_parent = rstd::fs::canonicalize(directory.as_path());
        if (resolved_parent.is_err()) return None();
        return Some(resolved_parent.unwrap().join(ref<rstd::path::Path>(*name)));
    }
    return None();
}

auto LockLocalStorage(const PathBuf& path) -> rstd::io::Result<rstd::fs::FileLock> {
    // Atomic replacement changes the data inode, so lock a stable sidecar instead.
    auto file = rstd_try(
        rstd::fs::OpenOptions::make().read(true).write(true).create(true).mode(u32(0600)).open(
            LocalStorageSidecar(path, ".lock"_str).as_path()));
    return rstd::fs::FileLock::acquire(rstd::move(file), rstd::fs::FileLockMode::Exclusive);
}
} // namespace

bool FlushLocalStorage(EngineHostState* host, const PathBuf& path) {
    auto object = rstd::json::Map::make();
    for (auto [k, v] : host->ls_data.iter()) object.insert(k->clone(), Json::String(v->clone()));
    auto out     = Json::Object(rstd::move(object));
    auto source  = rstd::json::to_string(out);
    auto written = rstd::fs::write_atomic(path.as_path(), source.as_str().as_bytes());
    if (written.is_err()) {
        rstd_warn("localStorage flush: cannot publish {}: {}",
                  path.as_path().to_string_lossy(),
                  written.unwrap_err());
        return false;
    }
    return true;
}

bool LoadLocalStorage(EngineHostState* host, const PathBuf& path) {
    if (path.is_empty()) {
        host->ls_data.clear();
        return true;
    }
    auto source = rstd::fs::read_to_string(path.as_path());
    if (source.is_err()) {
        if (source.unwrap_err().kind().code == rstd::io::ErrorKind::NotFound) {
            host->ls_data.clear();
            return true;
        }
        rstd_warn("localStorage load: cannot open {}: {}",
                  path.as_path().to_string_lossy(),
                  source.unwrap_err());
        return false;
    }
    auto parsed = rstd::json::from_str(source.unwrap().as_str());
    if (parsed.is_err()) {
        rstd_warn("localStorage parse failed: {}", parsed.unwrap_err());
        return false;
    }
    auto doc    = parsed.unwrap();
    auto object = doc.as_object();
    if (object.is_none()) return false;
    host->ls_data.clear();
    (*object)->iter().for_each([&](auto entry) {
        auto [entry_key, entry_value] = entry;
        const auto& value             = *entry_value;
        if (auto stored = value.as_str(); stored.is_some())
            (void)host->ls_data.insert(entry_key->clone(), rstd::into(*stored));
    });
    return true;
}

void ApplyLocalStorageMutation(EngineHostState* host, ref<str> key, Option<ref<str>> value) {
    if (value) {
        (void)host->ls_data.insert(String::make(key), String::make(*value));
    } else {
        (void)host->ls_data.remove(key);
    }
}

void UpdateLocalStorage(EngineHostState* host, ref<str> key, Option<ref<str>> value) {
    ApplyLocalStorageMutation(host, key, value);
    if (host->ls_path.is_empty()) return;
    Option<String> pending;
    if (value) pending = Some(String::make(*value));
    (void)host->ls_pending.insert(String::make(key), rstd::move(pending));
    auto path = ResolveLocalStoragePath(host->ls_path);
    if (path.is_none()) {
        rstd_warn("localStorage update: cannot resolve {}",
                  host->ls_path.as_path().to_string_lossy());
        return;
    }
    auto lock = LockLocalStorage(*path);
    if (lock.is_err()) {
        rstd_warn("localStorage update: cannot lock {}: {}",
                  path->as_path().to_string_lossy(),
                  lock.unwrap_err());
        return;
    }
    if (! LoadLocalStorage(host, *path)) return;
    for (auto [pending_key, pending_value] : host->ls_pending.iter()) {
        Option<ref<str>> mutation;
        if (pending_value->is_some()) mutation = Some((**pending_value).as_str());
        ApplyLocalStorageMutation(host, pending_key->as_str(), mutation);
    }
    if (FlushLocalStorage(host, *path)) host->ls_pending.clear();
}

JSValue LocalStorageGet(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    const char* key = JS_ToCString(ctx, argv[0]);
    if (! key) return JS_UNDEFINED;
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    auto  it   = host->ls_data.get(rstd::cppstd::as_str(key).unwrap());
    JS_FreeCString(ctx, key);
    if (it.is_none()) return JS_UNDEFINED;
    auto source = CString::make(Vec<u8>::from((*it)->as_str().as_bytes())).unwrap();
    return JS_ParseJSON(ctx, source.as_ptr(), (*it)->len().to_primitive(), "<localStorage>");
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
    if (s) {
        UpdateLocalStorage(
            host, rstd::cppstd::as_str(key).unwrap(), Some(rstd::cppstd::as_str(s).unwrap()));
    }
    if (s) JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, jv);
    JS_FreeCString(ctx, key);
    return JS_UNDEFINED;
}

JSValue LocalStorageRemove(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    const char* key = JS_ToCString(ctx, argv[0]);
    if (! key) return JS_UNDEFINED;
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    UpdateLocalStorage(host, rstd::cppstd::as_str(key).unwrap(), None());
    JS_FreeCString(ctx, key);
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

CursorWorld CursorToWorld(const FrameInputs& fi, owe::Scene* scene) {
    if (scene != nullptr) {
        const auto world = scene->ScreenToWorld({ fi.cursor_x, fi.cursor_y });
        return CursorWorld { .x = world.x(), .y = world.y() };
    }
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

    const CursorWorld world = CursorToWorld(fi, host->scene);
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
        minx              = rstd::cmp::min(w.x(), minx);
        maxx              = rstd::cmp::max(w.x(), maxx);
        miny              = rstd::cmp::min(w.y(), miny);
        maxy              = rstd::cmp::max(w.y(), maxy);
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

JSValue MakeAnimationEvent(JSContext* ctx, const owe::SceneAnimationEvent& event) {
    JSValue ev   = JS_NewObject(ctx);
    auto    text = rstd::cppstd::as_string_view(event.name.as_str());
    JS_DefinePropertyValueStr(
        ctx, ev, "name", JS_NewStringLen(ctx, text.data(), text.size()), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(
        ctx, ev, "frame", JS_NewInt32(ctx, event.frame.to_primitive()), JS_PROP_C_W_E);
    return ev;
}

// Invoke `name` on the script's module namespace if exported, passing one
// event arg. `thisLayer` should already be bound to the script's node by
// the caller. Exceptions are caught and logged once per sha.
void BindFieldScriptContext(JSContext*, const FieldScript::Impl&, JSValueConst);

void InvokeEventCallback(JSContext* ctx, JSValue ns, const char* name, JSValue ev,
                         JsRuntime::Impl* rt, ref<str> sha) {
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
    for (usize i {}; i < host->deferred.len(); ++i) {
        if (host->deferred[i].dead) continue;
        auto* pending_owner = host->deferred[i].owner;
        if (pending_owner != nullptr && pending_owner->m_impl->init_deferred) continue;
        while (! host->deferred[i].dead && host->deferred[i].fire_at <= now) {
            auto* previous = host->active_field_script;
            auto* owner    = host->deferred[i].owner;
            if (owner != nullptr && owner->m_impl->alive) {
                BindFieldScriptContext(ctx, *owner->m_impl, host->default_layer);
                host->active_field_script = owner;
            }
            JSValue fn  = JS_DupValue(ctx, host->deferred[i].fn);
            JSValue ret = JS_Call(ctx, fn, JS_UNDEFINED, 0, nullptr);
            JS_FreeValue(ctx, fn);
            host->active_field_script = previous;
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
    host->deferred.retain([](const DeferredCb& d) {
        return ! d.dead;
    });
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

void InstallEngineGlobal(JSContext* ctx) {
    // Run the bootstrap to create createScriptProperties + skeleton engine.
    const char* bootstrap = programs::BootstrapSource();
    JSValue     r         = JS_Eval(ctx,
                                    bootstrap,
                                    CStr::from_ptr(bootstrap).to_bytes().len().to_primitive(),
                                    "<wescene-bootstrap>",
                                    JS_EVAL_TYPE_GLOBAL);
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

// --- Layer-B: SceneNode wrapper class ---------------------------------------
// WWLayer instances share an invalidatable handle to their SceneNode.

static JSClassID s_layer_class_id             = 0;
static JSClassID s_effect_class_id            = 0;
static JSClassID s_material_class_id          = 0;
static JSClassID s_particle_instance_class_id = 0;

struct LayerHandle {
    EngineHostState*              host { nullptr };
    Option<Arc<ScriptLayerState>> state;
    String                        name;
    bool                          property_object { false };
};

struct ParticleInstanceHandle {
    Option<Arc<ScriptLayerState>> state;
};

owe::SceneNode* ResolveLayerNode(LayerHandle* h) {
    if (! h) return nullptr;
    if (h->state.is_some()) return (*h->state)->node;
    if (! h->host || ! h->host->scene_root || h->name.is_empty()) return nullptr;
    return h->host->scene_root->FindByName(h->name.as_str());
}

void LayerFinalizer(JSRuntime*, JSValue v) {
    delete static_cast<LayerHandle*>(JS_GetOpaque(v, s_layer_class_id));
}

JSClassDef s_layer_class_def {
    .class_name = "WWLayer",
    .finalizer  = LayerFinalizer,
};

struct EffectHandle {
    EngineHostState*              host { nullptr };
    Option<SceneImageEffectRef>   ref;
    Option<Arc<ScriptLayerState>> owner;
    bool                          fallback_visible { true };
};

struct MaterialHandle {
    EngineHostState*              host { nullptr };
    SceneMaterial*                material { nullptr };
    Option<Arc<ScriptLayerState>> owner;
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

void ParticleInstanceFinalizer(JSRuntime*, JSValue v) {
    delete static_cast<ParticleInstanceHandle*>(JS_GetOpaque(v, s_particle_instance_class_id));
}

JSClassDef s_material_class_def {
    .class_name = "WWMaterial",
    .finalizer  = MaterialFinalizer,
    .exotic     = &s_material_exotic,
};

JSClassDef s_particle_instance_class_def {
    .class_name = "WWParticleInstance",
    .finalizer  = ParticleInstanceFinalizer,
};

inline LayerHandle* GetLayerHandle(JSValueConst value) {
    return static_cast<LayerHandle*>(JS_GetOpaque(value, s_layer_class_id));
}

inline owe::SceneNode* GetLayerNode(JSValueConst value) {
    return ResolveLayerNode(GetLayerHandle(value));
}

auto LayerStateForNode(EngineHostState* host, owe::SceneNode* node)
    -> Option<Arc<ScriptLayerState>> {
    Option<Arc<ScriptLayerState>> state;
    if (host && node) {
        auto existing = host->layer_states.get(node);
        if (existing.is_some()) {
            state = Some((**existing).clone());
        } else {
            auto created = Arc<ScriptLayerState>::make(ScriptLayerState { .node = node });
            state        = Some(created.clone());
            (void)host->layer_states.insert(node, rstd::move(created));
        }
    }
    return state;
}

JSValue WrapLayerNode(JSContext* ctx, owe::SceneNode* node, bool property_object = false) {
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    if (host && ! property_object) {
        auto object = host->layer_objects.get(node);
        if (object.is_some()) return JS_DupValue(ctx, **object);
    }
    JSValue obj = JS_NewObjectClass(ctx, s_layer_class_id);
    if (JS_IsException(obj)) return obj;
    JS_SetOpaque(obj,
                 new LayerHandle { .host            = host,
                                   .state           = LayerStateForNode(host, node),
                                   .property_object = property_object });
    if (host && ! property_object) (void)host->layer_objects.insert(node, JS_DupValue(ctx, obj));
    return obj;
}

JSValue WrapLayerName(JSContext* ctx, String name) {
    JSValue obj = JS_NewObjectClass(ctx, s_layer_class_id);
    if (JS_IsException(obj)) return obj;
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    JS_SetOpaque(obj, new LayerHandle { .host = host, .name = rstd::move(name) });
    return obj;
}

JSValue WrapParticleInstance(JSContext* ctx, owe::SceneNode* node) {
    JSValue obj = JS_NewObjectClass(ctx, s_particle_instance_class_id);
    if (JS_IsException(obj)) return obj;
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    JS_SetOpaque(obj, new ParticleInstanceHandle { LayerStateForNode(host, node) });
    return obj;
}

EffectHandle* GetEffectHandle(JSValueConst v) {
    return static_cast<EffectHandle*>(JS_GetOpaque(v, s_effect_class_id));
}

JSValue WrapEffect(JSContext* ctx, Option<SceneImageEffectRef> ref,
                   owe::SceneNode* owner = nullptr) {
    JSValue obj = JS_NewObjectClass(ctx, s_effect_class_id);
    if (JS_IsException(obj)) return obj;
    auto* host   = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    bool visible = host && host->scene && ref ? host->scene->ImageEffectRuntimeVisible(*ref) : true;
    JS_SetOpaque(obj,
                 new EffectHandle { .host             = host,
                                    .ref              = rstd::move(ref),
                                    .owner            = LayerStateForNode(host, owner),
                                    .fallback_visible = visible });
    return obj;
}

JSValue WrapMaterial(JSContext* ctx, SceneMaterial* material, owe::SceneNode* owner = nullptr) {
    JSValue obj = JS_NewObjectClass(ctx, s_material_class_id);
    if (JS_IsException(obj)) return obj;
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    JS_SetOpaque(obj,
                 new MaterialHandle {
                     .host = host, .material = material, .owner = LayerStateForNode(host, owner) });
    return obj;
}

JSValue EffectGetVisible(JSContext* ctx, JSValueConst this_val) {
    auto* h = GetEffectHandle(this_val);
    if (! h) return JS_NewBool(ctx, true);
    if (h->host && h->host->scene && h->ref && (h->owner.is_none() || (*h->owner)->node))
        return JS_NewBool(ctx, h->host->scene->ImageEffectRuntimeVisible(*h->ref));
    return JS_NewBool(ctx, h->fallback_visible);
}

JSValue EffectSetVisible(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* h       = GetEffectHandle(this_val);
    bool  visible = JS_ToBool(ctx, val) != 0;
    if (! h) return JS_UNDEFINED;
    h->fallback_visible = visible;
    if (h->host && h->host->scene && h->ref && (h->owner.is_none() || (*h->owner)->node)) {
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

inline bool ReadXY(JSContext* ctx, JSValueConst value, double& x, double& y) {
    if (! JS_IsObject(value)) return false;
    JSValue    js_x = JS_GetPropertyStr(ctx, value, "x");
    JSValue    js_y = JS_GetPropertyStr(ctx, value, "y");
    const bool ok   = JS_ToFloat64(ctx, &x, js_x) == 0 && JS_ToFloat64(ctx, &y, js_y) == 0;
    JS_FreeValue(ctx, js_x);
    JS_FreeValue(ctx, js_y);
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
    if (! handle || ! handle->host || ! handle->host->scene || ! handle->material ||
        (handle->owner.is_some() && ! (*handle->owner)->node))
        return 1;

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
constexpr double kRadToDeg = 180.0 / f64::consts::PI.to_primitive();
constexpr double kDegToRad = f64::consts::PI.to_primitive() / 180.0;
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

JSValue NodeGetParallaxDepth(JSContext* ctx, JSValueConst this_val) {
    auto* node = GetLayerNode(this_val);
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    if (node == nullptr || host == nullptr || host->node_parallax_depth_getter.is_none())
        return MakeVec2Value(ctx, 0.0, 0.0);
    auto depth = (**host->node_parallax_depth_getter)(node);
    if (depth.is_none()) return MakeVec2Value(ctx, 0.0, 0.0);
    return MakeVec2Value(ctx, depth->x, depth->y);
}

JSValue NodeSetParallaxDepth(JSContext* ctx, JSValueConst this_val, JSValueConst value) {
    auto* node = GetLayerNode(this_val);
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    if (node == nullptr || host == nullptr || host->node_parallax_depth_setter.is_none())
        return JS_UNDEFINED;
    double x {};
    double y {};
    if (! ReadXY(ctx, value, x, y)) return JS_UNDEFINED;
    (**host->node_parallax_depth_setter)(node, Vec2Value { .x = x, .y = y });
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
    auto* handle =
        static_cast<ParticleInstanceHandle*>(JS_GetOpaque(this_val, s_particle_instance_class_id));
    auto* node = handle && handle->state.is_some() ? (*handle->state)->node : nullptr;
    if (! node) return JS_UNDEFINED;
    auto control = node->ParticleControlHandle();
    if (control.is_none()) return JS_UNDEFINED;
    auto value = (*control)->Get(field);
    if (vector) {
        if (value.len() < usize(3)) return JS_UNDEFINED;
        return MakeVec3(ctx, value[usize()], value[usize(1)], value[usize(2)]);
    }
    return value.is_empty() ? JS_UNDEFINED : JS_NewFloat64(ctx, value.first().unwrap().get());
}

JSValue ParticleSet(JSContext* ctx, JSValueConst this_val, JSValueConst value, ref<str> field,
                    bool vector) {
    auto* handle =
        static_cast<ParticleInstanceHandle*>(JS_GetOpaque(this_val, s_particle_instance_class_id));
    auto* node = handle && handle->state.is_some() ? (*handle->state)->node : nullptr;
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
JSValue NodeEmitParticles(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* node = GetLayerNode(this_val);
    if (! node) return JS_UNDEFINED;
    auto control = node->ParticleControl();
    if (control.is_none()) return JS_UNDEFINED;
    rstd::int64_t count {};
    if (argc < 1 || JS_ToInt64(ctx, &count, argv[0]) != 0 || count <= 0) return JS_UNDEFINED;
    auto emit_count = count > static_cast<rstd::int64_t>(u32::MAX.to_primitive())
                          ? u32::MAX
                          : u32(static_cast<rstd::uint32_t>(count));
    (**control).Emit(emit_count);
    return JS_UNDEFINED;
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
    auto*      host  = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    auto       it    = host->text_align_hooks.get_mut(n);
    const auto value = it.is_none() ? "center"_str : (**it).vertical.as_str();
    const auto bytes = rstd::cppstd::as_string_view(value);
    return JS_NewStringLen(ctx, bytes.data(), bytes.size());
}
JSValue NodeGetHAlign(JSContext* ctx, JSValueConst this_val) {
    auto* n = GetLayerNode(this_val);
    if (! n) return JS_NewString(ctx, "center");
    auto*      host  = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    auto       it    = host->text_align_hooks.get_mut(n);
    const auto value = it.is_none() ? "center"_str : (**it).horizontal.as_str();
    const auto bytes = rstd::cppstd::as_string_view(value);
    return JS_NewStringLen(ctx, bytes.data(), bytes.size());
}
JSValue NodeSetVAlign(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* n = GetLayerNode(this_val);
    if (! n) return JS_UNDEFINED;
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    auto  it   = host->text_align_hooks.get_mut(n);
    if (it.is_none()) return JS_UNDEFINED;
    const char* s = JS_ToCString(ctx, val);
    if (s == nullptr) return JS_UNDEFINED;
    auto next = rstd::cppstd::as_str(s).unwrap();
    if (! (**it).set_vertical || (**it).set_vertical->operator()(next))
        (**it).vertical = String::make(next);
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}
JSValue NodeSetHAlign(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* n = GetLayerNode(this_val);
    if (! n) return JS_UNDEFINED;
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    auto  it   = host->text_align_hooks.get_mut(n);
    if (it.is_none()) return JS_UNDEFINED;
    const char* s = JS_ToCString(ctx, val);
    if (s == nullptr) return JS_UNDEFINED;
    auto next = rstd::cppstd::as_str(s).unwrap();
    if (! (**it).set_horizontal || (**it).set_horizontal->operator()(next))
        (**it).horizontal = String::make(next);
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}
JSValue NodeSetIgnore(JSContext*, JSValueConst, JSValueConst) { return JS_UNDEFINED; }

// `text` is the string-valued property on WWLayer.
JSValue NodeGetText(JSContext* ctx, JSValueConst this_val) {
    auto* n = GetLayerNode(this_val);
    if (! n) return JS_NewString(ctx, "");
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    if (! host) return JS_NewString(ctx, "");
    if (auto it = host->current_text_strings.get(n); it.is_some()) {
        const auto view = rstd::cppstd::as_string_view((*it)->as_str());
        return JS_NewStringLen(ctx, view.data(), view.size());
    }
    return JS_NewString(ctx, "");
}
JSValue NodeSetText(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* n = GetLayerNode(this_val);
    if (! n) return JS_UNDEFINED;
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    auto  it   = host->text_setters.get_mut(n);
    if (it.is_none()) return JS_UNDEFINED;
    const char* s = JS_ToCString(ctx, val);
    if (s == nullptr) return JS_UNDEFINED;
    auto str_ref = rstd::cppstd::as_str(s).unwrap();
    (void)host->current_text_strings.insert(n, String::make(str_ref));
    (**it)->operator()(str_ref);
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}
JSValue NodeGetPointSize(JSContext* ctx, JSValueConst this_val) {
    auto* n = GetLayerNode(this_val);
    if (! n) return JS_NewFloat64(ctx, 1.0);
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    auto  it   = host->text_align_hooks.get_mut(n);
    if (it.is_some()) {
        if ((**it).get_point_size)
            return JS_NewFloat64(ctx, (*(**it).get_point_size)->operator()());
        return JS_NewFloat64(ctx, (**it).point_size);
    }
    auto* mesh = n->Mesh();
    return JS_NewFloat64(ctx, mesh == nullptr ? 1.0 : mesh->PointSize().to_primitive());
}
JSValue NodeSetPointSize(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* n = GetLayerNode(this_val);
    if (! n) return JS_UNDEFINED;
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    auto  it   = host->text_align_hooks.get_mut(n);
    if (it.is_none()) return JS_UNDEFINED;
    double point_size = (**it).point_size;
    if (JS_ToFloat64(ctx, &point_size, val) < 0 || ! f64(point_size).is_finite())
        return JS_UNDEFINED;
    if ((**it).set_point_size.is_none() || (*(**it).set_point_size)->operator()(point_size))
        (**it).point_size = point_size;
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

JSValue MakeMat4(JSContext* ctx, const BoneTransformData& data) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ctor   = JS_GetPropertyStr(ctx, global, "Mat4");
    JSValue arr    = JS_NewArray(ctx);
    for (int i = 0; i < 16; ++i) {
        JS_SetPropertyUint32(
            ctx, arr, static_cast<uint32_t>(i), JS_NewFloat64(ctx, static_cast<double>(data.m[i])));
    }
    JSValue argv[1] = { arr };
    JSValue matObj  = JS_CallConstructor(ctx, ctor, 1, argv);
    JS_FreeValue(ctx, arr);
    JS_FreeValue(ctx, ctor);
    JS_FreeValue(ctx, global);
    return matObj;
}

static uint32_t ResolveBoneIndex(JSContext* ctx, owe::SceneNode* n, EngineHostState* host,
                                 JSValueConst arg) {
    if (! n || ! host) return 0;
    if (JS_IsString(arg)) {
        if (! host->bone_index_resolver) return 0;
        const char* name = JS_ToCString(ctx, arg);
        if (! name) return 0;
        uint32_t idx =
            (*host->bone_index_resolver)->operator()(n, rstd::cppstd::as_str(name).unwrap());
        JS_FreeCString(ctx, name);
        return idx;
    }
    int32_t idx = 0;
    JS_ToInt32(ctx, &idx, arg);
    return idx > 0 ? static_cast<uint32_t>(idx) : 0;
}

JSValue NodeGetBoneIndex(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    auto* n    = GetLayerNode(this_val);
    if (! n || argc < 1 || ! host) return JS_NewInt32(ctx, 0);
    uint32_t index = ResolveBoneIndex(ctx, n, host, argv[0]);
    return JS_NewInt32(ctx, static_cast<int32_t>(index));
}

JSValue NodeGetBoneCount(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    auto* n    = GetLayerNode(this_val);
    if (! n || ! host || ! host->bone_count_resolver) return JS_NewInt32(ctx, 0);
    uint32_t count = (*host->bone_count_resolver)->operator()(n);
    return JS_NewInt32(ctx, static_cast<int32_t>(count));
}

JSValue NodeGetBoneTransform(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto*             host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    auto*             n    = GetLayerNode(this_val);
    BoneTransformData identity;
    if (! n || argc < 1 || ! host || ! host->bone_transform_resolver)
        return MakeMat4(ctx, identity);

    uint32_t index = ResolveBoneIndex(ctx, n, host, argv[0]);
    if (index == 0) return MakeMat4(ctx, identity);

    auto bone = (*host->bone_transform_resolver)->operator()(n, index, host->inputs.runtime);
    if (! bone) return MakeMat4(ctx, identity);
    return MakeMat4(ctx, *bone);
}

JSValue NodeSetBoneTransform(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_UNDEFINED;
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    auto* n    = GetLayerNode(this_val);
    if (! n || ! host || ! host->bone_set_transform_resolver) return JS_UNDEFINED;

    uint32_t index = ResolveBoneIndex(ctx, n, host, argv[0]);
    if (index == 0) return JS_UNDEFINED;

    BoneTransformData data;
    JSValueConst      val    = argv[1];
    JSValue           m_prop = JS_GetPropertyStr(ctx, val, "m");
    JSValue           arr    = JS_IsUndefined(m_prop) ? JS_DupValue(ctx, val) : m_prop;

    if (JS_IsArray(arr)) {
        for (int i = 0; i < 16; ++i) {
            JSValue elem = JS_GetPropertyUint32(ctx, arr, static_cast<uint32_t>(i));
            double  dval = 0.0;
            JS_ToFloat64(ctx, &dval, elem);
            data.m[i] = static_cast<float>(dval);
            JS_FreeValue(ctx, elem);
        }
    }
    JS_FreeValue(ctx, arr);

    (*host->bone_set_transform_resolver)->operator()(n, index, data);
    return JS_UNDEFINED;
}

JSValue NodeGetChildren(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto*   n   = GetLayerNode(this_val);
    JSValue arr = JS_NewArray(ctx);
    if (! n) return arr;
    rstd::uint32_t i = 0;
    for (const auto& child : n->GetChildren()) {
        JS_DefinePropertyValueUint32(
            ctx, arr, i++, WrapLayerNode(ctx, child.as_ptr()), JS_PROP_C_W_E);
    }
    return arr;
}

JSValue NodeGetNameValue(JSContext* ctx, JSValueConst this_val) {
    auto* n = GetLayerNode(this_val);
    if (! n) return JS_NewString(ctx, "");
    return JS_NewStringLen(
        ctx, rstd::cppstd::as_string_view(n->Name()).data(), n->Name().len().to_primitive());
}

JSValue NodeGetName(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    return NodeGetNameValue(ctx, this_val);
}

// Parsed scenes query registered wallpaper layers, excluding internal nodes.
// Runtimes without authored scene metadata query all tree nodes.
bool IsScriptSceneLayer(EngineHostState* host, owe::SceneNode* node) {
    return ! host || ! host->registered_layer_queries || host->script_layers.contains(node);
}

// Walk wallpaper layers in the same depth-first order for every scene query.
// A visitor returning false ends traversal.
template<typename Visitor>
bool VisitScriptSceneLayers(EngineHostState* host, owe::SceneNode* root, Visitor& visit) {
    if (! root) return true;
    for (const auto& child : root->GetChildren()) {
        if (IsScriptSceneLayer(host, child.as_ptr()) && ! visit(child.as_ptr())) return false;
        if (! VisitScriptSceneLayers(host, child.as_ptr(), visit)) return false;
    }
    return true;
}

owe::SceneNode* SceneLayerAt(JSContext* ctx, owe::SceneNode* root, JSValueConst value) {
    double        numeric_index {};
    rstd::int64_t index {};
    if (JS_ToFloat64(ctx, &numeric_index, value) != 0 || JS_ToInt64(ctx, &index, value) != 0 ||
        index < 0 || numeric_index != static_cast<double>(index))
        return nullptr;
    auto*           host   = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    owe::SceneNode* result = nullptr;
    auto            find   = [&](owe::SceneNode* node) {
        if (index-- != 0) return true;
        result = node;
        return false;
    };
    VisitScriptSceneLayers(host, root, find);
    return result;
}

JSValue NodeSceneGetLayerCount(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto*         host  = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    rstd::int64_t total = 0;
    auto          count = [&](owe::SceneNode*) {
        ++total;
        return true;
    };
    VisitScriptSceneLayers(host, GetLayerNode(this_val), count);
    return JS_NewInt64(ctx, total);
}

JSValue NodeGetLayer(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    auto* n    = GetLayerNode(this_val);
    if (! n || argc < 1) return JS_DupValue(ctx, host->default_layer);
    if (JS_IsNumber(argv[0])) {
        auto* hit = SceneLayerAt(ctx, n, argv[0]);
        return hit ? WrapLayerNode(ctx, hit) : JS_NULL;
    }
    const char* name = JS_ToCString(ctx, argv[0]);
    if (! name) return JS_DupValue(ctx, host->default_layer);
    String          layer_name = rstd::into(rstd::cppstd::as_str(name).unwrap());
    owe::SceneNode* hit        = n->FindByName(layer_name.as_str());
    JS_FreeCString(ctx, name);
    return hit ? WrapLayerNode(ctx, hit) : WrapLayerName(ctx, rstd::move(layer_name));
}

JSValue NodeGetEffect(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    auto* n    = GetLayerNode(this_val);
    if (! host || ! host->scene || ! n || argc < 1) return WrapEffect(ctx, None());

    if (JS_IsNumber(argv[0])) {
        rstd::int64_t index {};
        if (JS_ToInt64(ctx, &index, argv[0]) != 0 || index < 0) return WrapEffect(ctx, None());
        return WrapEffect(ctx, host->scene->FindNodeImageEffect(*n, usize(index)), n);
    }

    const char* name = JS_ToCString(ctx, argv[0]);
    if (! name) return WrapEffect(ctx, None());
    auto effect = host->scene->FindNodeImageEffect(*n, rstd::cppstd::as_str(name).unwrap());
    JS_FreeCString(ctx, name);
    return WrapEffect(ctx, rstd::move(effect), n);
}

JSValue NodeGetEffectCount(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    auto* node = GetLayerNode(this_val);
    if (! host || ! host->scene || ! node) return JS_NewInt32(ctx, 0);
    return JS_NewInt64(
        ctx, static_cast<rstd::int64_t>(host->scene->NodeImageEffectCount(*node).to_primitive()));
}

JSValue EffectGetName(JSContext* ctx, JSValueConst this_val) {
    auto* handle = GetEffectHandle(this_val);
    if (! handle || ! handle->host || ! handle->host->scene || ! handle->ref ||
        (handle->owner.is_some() && ! (*handle->owner)->node))
        return JS_NewString(ctx, "");
    auto name = handle->host->scene->ImageEffectName(*handle->ref);
    auto view = rstd::cppstd::as_string_view(name);
    return JS_NewStringLen(ctx, view.data(), view.size());
}

JSValue EffectGetMaterial(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* handle = GetEffectHandle(this_val);
    if (! handle || ! handle->host || ! handle->host->scene || ! handle->ref || argc < 1 ||
        (handle->owner.is_some() && ! (*handle->owner)->node))
        return WrapMaterial(ctx, nullptr);
    rstd::int64_t index {};
    if (JS_ToInt64(ctx, &index, argv[0]) != 0 || index < 0) return WrapMaterial(ctx, nullptr);
    auto* owner = handle->owner.is_some() ? (*handle->owner)->node : nullptr;
    return WrapMaterial(
        ctx, handle->host->scene->ImageEffectMaterial(*handle->ref, usize(index)), owner);
}

bool TreeContains(owe::SceneNode* root, owe::SceneNode* needle) {
    if (! root || ! needle) return false;
    if (root == needle) return true;
    for (const auto& child : root->GetChildren()) {
        if (TreeContains(child.as_ptr(), needle)) return true;
    }
    return false;
}

void PruneSceneLayerBindings(JSContext* ctx, EngineHostState& host, owe::SceneNode* root) {
    host.layer_states.retain([&](owe::SceneNode* node, Arc<ScriptLayerState>& state) {
        if (TreeContains(root, node)) return true;
        state->node = nullptr;
        return false;
    });
    host.layer_objects.retain([&](owe::SceneNode* node, JSValue& object) {
        if (TreeContains(root, node)) return true;
        JS_FreeValue(ctx, object);
        return false;
    });
    host.script_layers.retain([&](owe::SceneNode* node) {
        return TreeContains(root, node);
    });
    host.text_setters.retain([&](owe::SceneNode* node, auto&) {
        return TreeContains(root, node);
    });
    host.current_text_strings.retain([&](owe::SceneNode* node, auto&) {
        return TreeContains(root, node);
    });
    host.text_align_hooks.retain([&](owe::SceneNode* node, auto&) {
        return TreeContains(root, node);
    });
    host.node_origin_hooks.retain([&](owe::SceneNode* node, auto&) {
        return TreeContains(root, node);
    });
    host.image_alignment_hooks.retain([&](owe::SceneNode* node, auto&) {
        return TreeContains(root, node);
    });
    host.initial_layer_configs.retain([&](owe::SceneNode* node, auto&) {
        return TreeContains(root, node);
    });
}

JSValue NodeSceneLayerListIncludes(JSContext* ctx, JSValueConst this_val, int argc,
                                   JSValueConst* argv) {
    if (argc < 1) return JS_NewBool(ctx, false);
    JSValue root_val = JS_GetPropertyStr(ctx, this_val, "__wwRoot");
    auto*   root     = GetLayerNode(root_val);
    auto*   needle   = GetLayerNode(argv[0]);
    auto*   host     = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    bool    found    = false;
    auto    find     = [&](owe::SceneNode* node) {
        found = node == needle;
        return ! found;
    };
    VisitScriptSceneLayers(host, root, find);
    JS_FreeValue(ctx, root_val);
    return JS_NewBool(ctx, found);
}

JSValue NodeSceneEnumerateLayers(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    JSValue arr = JS_NewArray(ctx);
    auto*   n   = GetLayerNode(this_val);
    if (! n) return arr;

    auto*          host   = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    rstd::uint32_t i      = 0;
    auto           append = [&](owe::SceneNode* node) {
        JS_DefinePropertyValueUint32(ctx, arr, i++, WrapLayerNode(ctx, node), JS_PROP_C_W_E);
        return true;
    };
    VisitScriptSceneLayers(host, n, append);
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
    // A factory may initialize nested field scripts and replace the caller's JS globals.
    EngineBindingGuard binding_guard(ctx, *host);

    owe::SceneNode* node = nullptr;
    if (argc > 0 && ! JS_IsObject(argv[0])) {
        const char* asset = JS_ToCString(ctx, argv[0]);
        if (asset) {
            if (host->layer_factory.is_some()) {
                auto created = (**host->layer_factory)(
                    fs->m_impl->node,
                    LayerAssetReference { .path        = rstd::cppstd::as_str(asset).unwrap(),
                                          .workshop_id = fs->WorkshopId() });
                if (created.is_some()) node = (*created).as_ptr();
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
                auto parsed = ParseJson(rstd::cppstd::as_str(source).unwrap());
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
    (void)host->script_layers.insert(node);
    if (argc == 0 || ! JS_IsObject(argv[0])) {
        if (host->scene)
            (void)host->scene->SetNodeVisible(*node, true);
        else
            node->SetVisible(true);
    }
    node->Play();
    return WrapLayerNode(ctx, node);
}

JSValue NodeSceneDestroyLayer(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    if (! host || ! host->scene || ! host->scene_root || argc < 1) return JS_NewBool(ctx, false);
    owe::SceneNode* node = nullptr;
    if (JS_IsNumber(argv[0])) {
        node = SceneLayerAt(ctx, host->scene_root, argv[0]);
    } else if (JS_IsString(argv[0])) {
        const char* name = JS_ToCString(ctx, argv[0]);
        if (! name) return JS_EXCEPTION;
        node = host->scene_root->FindByName(rstd::cppstd::as_str(name).unwrap());
        JS_FreeCString(ctx, name);
    } else if (JS_IsObject(argv[0])) {
        node = GetLayerNode(argv[0]);
    }
    return JS_NewBool(
        ctx, node && IsScriptSceneLayer(host, node) && host->scene->RequestLayerDeletion(*node));
}

owe::SceneNode* ResolveSceneLayerArgument(JSContext* ctx, JSValueConst this_val,
                                          JSValueConst value) {
    if (auto* node = GetLayerNode(value)) return node;
    auto* root = GetLayerNode(this_val);
    if (root == nullptr) return nullptr;
    const char* name = JS_ToCString(ctx, value);
    if (name == nullptr) return nullptr;
    auto* node = root->FindByName(rstd::cppstd::as_str(name).unwrap());
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
    return JS_NewInt64(ctx, static_cast<rstd::int64_t>(index->to_primitive()));
}

JSValue NodeSceneSortLayer(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    if (host == nullptr || host->scene == nullptr || argc < 2) return JS_UNDEFINED;
    auto*         node = ResolveSceneLayerArgument(ctx, this_val, argv[0]);
    rstd::int64_t index {};
    if (node == nullptr || JS_ToInt64(ctx, &index, argv[1]) != 0 || index < 0) return JS_UNDEFINED;
    (void)host->scene->SortLayer(*node, usize(static_cast<rstd::size_t>(index)));
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

JSValue MakeAnimationStub(JSContext* ctx) {
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue f = JS_GetPropertyStr(ctx, g, "__wwCreateAnimationStub");
    JSValue r = JS_Call(ctx, f, JS_UNDEFINED, 0, nullptr);
    JS_FreeValue(ctx, f);
    JS_FreeValue(ctx, g);
    return r;
}

static JSClassID s_animation_class_id = 0;

struct AnimationHandle {
    Arc<owe::SceneAnimationPlayback> playback;
};

void AnimationFinalizer(JSRuntime*, JSValue value) {
    delete static_cast<AnimationHandle*>(JS_GetOpaque(value, s_animation_class_id));
}

JSClassDef s_animation_class_def {
    .class_name = "WWAnimation",
    .finalizer  = AnimationFinalizer,
};

owe::SceneAnimationPlayback* GetAnimationPlayback(JSValueConst value) {
    auto* handle = static_cast<AnimationHandle*>(JS_GetOpaque(value, s_animation_class_id));
    return handle != nullptr ? handle->playback.as_ptr().as_raw_ptr() : nullptr;
}

JSValue AnimationGetFps(JSContext* ctx, JSValueConst value) {
    auto* playback = GetAnimationPlayback(value);
    return JS_NewFloat64(ctx, playback != nullptr ? playback->Fps() : 0.0);
}

JSValue AnimationGetFrameCount(JSContext* ctx, JSValueConst value) {
    auto* playback = GetAnimationPlayback(value);
    return JS_NewInt32(ctx, playback != nullptr ? playback->FrameCount().to_primitive() : 0);
}

JSValue AnimationGetDuration(JSContext* ctx, JSValueConst value) {
    auto* playback = GetAnimationPlayback(value);
    return JS_NewFloat64(ctx, playback != nullptr ? playback->Duration() : 0.0);
}

JSValue AnimationGetName(JSContext* ctx, JSValueConst value) {
    auto* playback = GetAnimationPlayback(value);
    if (playback == nullptr) return JS_NewStringLen(ctx, "", 0);
    auto name = rstd::cppstd::as_string_view(playback->Name());
    return JS_NewStringLen(ctx, name.data(), name.size());
}

JSValue AnimationGetRate(JSContext* ctx, JSValueConst value) {
    auto* playback = GetAnimationPlayback(value);
    return JS_NewFloat64(ctx, playback != nullptr ? playback->Rate() : 1.0);
}

JSValue AnimationSetRate(JSContext* ctx, JSValueConst value, JSValueConst next) {
    double rate = 1.0;
    if (JS_ToFloat64(ctx, &rate, next) == 0) {
        if (auto* playback = GetAnimationPlayback(value))
            playback->SetRate(static_cast<float>(rate));
    }
    return JS_UNDEFINED;
}

JSValue AnimationPlay(JSContext*, JSValueConst value, int, JSValueConst*) {
    if (auto* playback = GetAnimationPlayback(value)) playback->Play();
    return JS_UNDEFINED;
}

JSValue AnimationStop(JSContext*, JSValueConst value, int, JSValueConst*) {
    if (auto* playback = GetAnimationPlayback(value)) playback->Stop();
    return JS_UNDEFINED;
}

JSValue AnimationPause(JSContext*, JSValueConst value, int, JSValueConst*) {
    if (auto* playback = GetAnimationPlayback(value)) playback->Pause();
    return JS_UNDEFINED;
}

JSValue AnimationSetFrame(JSContext* ctx, JSValueConst value, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    double frame {};
    if (JS_ToFloat64(ctx, &frame, argv[0]) == 0) {
        if (auto* playback = GetAnimationPlayback(value))
            playback->SetFrame(static_cast<float>(frame));
    }
    return JS_UNDEFINED;
}

JSValue AnimationGetFrame(JSContext* ctx, JSValueConst value, int, JSValueConst*) {
    auto* playback = GetAnimationPlayback(value);
    return JS_NewInt32(ctx, playback != nullptr ? playback->Frame().to_primitive() : 0);
}

JSValue AnimationIsPlaying(JSContext* ctx, JSValueConst value, int, JSValueConst*) {
    auto* playback = GetAnimationPlayback(value);
    return JS_NewBool(ctx, playback != nullptr && playback->IsPlaying());
}

const JSCFunctionListEntry s_animation_proto_funcs[] = {
    JS_CGETSET_DEF("fps", AnimationGetFps, NodeSetIgnore),
    JS_CGETSET_DEF("frameCount", AnimationGetFrameCount, NodeSetIgnore),
    JS_CGETSET_DEF("duration", AnimationGetDuration, NodeSetIgnore),
    JS_CGETSET_DEF("name", AnimationGetName, NodeSetIgnore),
    JS_CGETSET_DEF("rate", AnimationGetRate, AnimationSetRate),
    JS_CFUNC_DEF("play", 0, AnimationPlay),
    JS_CFUNC_DEF("stop", 0, AnimationStop),
    JS_CFUNC_DEF("pause", 0, AnimationPause),
    JS_CFUNC_DEF("setFrame", 1, AnimationSetFrame),
    JS_CFUNC_DEF("getFrame", 0, AnimationGetFrame),
    JS_CFUNC_DEF("isPlaying", 0, AnimationIsPlaying),
};

void InitAnimationClass(JSContext* ctx, JSRuntime* rt) {
    if (s_animation_class_id == 0) JS_NewClassID(rt, &s_animation_class_id);
    JS_NewClass(rt, s_animation_class_id, &s_animation_class_def);
    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx,
                               proto,
                               s_animation_proto_funcs,
                               sizeof(s_animation_proto_funcs) /
                                   sizeof(s_animation_proto_funcs[0]));
    JS_SetClassProto(ctx, s_animation_class_id, proto);
}

auto ActivePropertyAnimation(JSContext* ctx, int argc, JSValueConst* argv)
    -> Option<Arc<owe::SceneAnimationPlayback>> {
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    Option<Arc<owe::SceneAnimationPlayback>> playback;
    if (host != nullptr && host->active_field_script != nullptr) {
        auto* script = host->active_field_script->m_impl.get();
        if (script->animation.is_some()) playback = Some((*script->animation).clone());
    } else if (host != nullptr && host->active_animation.is_some()) {
        playback = Some((*host->active_animation).clone());
    }
    if (playback.is_none()) return None();
    if (argc > 0 && ! JS_IsUndefined(argv[0]) && ! JS_IsNull(argv[0])) {
        const char* name = JS_ToCString(ctx, argv[0]);
        if (name == nullptr) return None();
        const bool matches =
            *name == '\0' || (**playback).Name() == rstd::cppstd::as_str(name).unwrap();
        JS_FreeCString(ctx, name);
        if (! matches) return None();
    }
    return playback;
}

JSValue WrapAnimation(JSContext* ctx, Option<Arc<owe::SceneAnimationPlayback>> playback) {
    if (playback.is_none()) return MakeAnimationStub(ctx);
    JSValue object = JS_NewObjectClass(ctx, s_animation_class_id);
    if (JS_IsException(object)) return object;
    JS_SetOpaque(object, new AnimationHandle { .playback = rstd::move(*playback) });
    return object;
}

JSValue PropertyObjectGetAnimation(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    return WrapAnimation(ctx, ActivePropertyAnimation(ctx, argc, argv));
}

JSValue WrapAnimationLayer(JSContext* ctx, Option<Arc<owe::SceneAnimationPlayback>> playback) {
    JSValue object    = JS_NewObject(ctx);
    JSValue animation = WrapAnimation(ctx, rstd::move(playback));
    JSValue getter    = JS_NewCFunctionData(
        ctx,
        [](JSContext* context, JSValueConst, int, JSValueConst*, int, JSValue* data) -> JSValue {
            return JS_DupValue(context, data[0]);
        },
        0,
        0,
        1,
        &animation);
    JS_SetPropertyStr(ctx, object, "getAnimation", getter);
    JS_FreeValue(ctx, animation);
    return object;
}

static JSClassID s_named_animation_layer_class_id = 0;

void NamedAnimationLayerFinalizer(JSRuntime*, JSValue value) {
    delete static_cast<Arc<owe::SceneAnimationLayer>*>(
        JS_GetOpaque(value, s_named_animation_layer_class_id));
}

owe::SceneAnimationLayer* GetNamedAnimationLayer(JSValueConst value) {
    auto* layer = static_cast<Arc<owe::SceneAnimationLayer>*>(
        JS_GetOpaque(value, s_named_animation_layer_class_id));
    return layer ? layer->as_ptr().as_raw_ptr() : nullptr;
}

JSValue NamedLayerGetBlend(JSContext* ctx, JSValueConst value) {
    auto* layer = GetNamedAnimationLayer(value);
    return layer ? JS_NewFloat64(ctx, layer->Blend()) : JS_UNDEFINED;
}
JSValue NamedLayerSetBlend(JSContext* ctx, JSValueConst value, JSValueConst next) {
    double number;
    if (auto* layer = GetNamedAnimationLayer(value); layer && JS_ToFloat64(ctx, &number, next) == 0)
        layer->SetBlend(static_cast<float>(number));
    return JS_UNDEFINED;
}
JSValue NamedLayerGetRate(JSContext* ctx, JSValueConst value) {
    auto* layer = GetNamedAnimationLayer(value);
    return layer ? JS_NewFloat64(ctx, layer->Rate()) : JS_UNDEFINED;
}
JSValue NamedLayerSetRate(JSContext* ctx, JSValueConst value, JSValueConst next) {
    double number;
    if (auto* layer = GetNamedAnimationLayer(value); layer && JS_ToFloat64(ctx, &number, next) == 0)
        layer->SetRate(static_cast<float>(number));
    return JS_UNDEFINED;
}
JSValue NamedLayerGetVisible(JSContext* ctx, JSValueConst value) {
    auto* layer = GetNamedAnimationLayer(value);
    return layer ? JS_NewBool(ctx, layer->Visible()) : JS_UNDEFINED;
}
JSValue NamedLayerSetVisible(JSContext* ctx, JSValueConst value, JSValueConst next) {
    if (auto* layer = GetNamedAnimationLayer(value)) layer->SetVisible(JS_ToBool(ctx, next) > 0);
    return JS_UNDEFINED;
}
JSValue NamedLayerPlay(JSContext*, JSValueConst value, int, JSValueConst*) {
    if (auto* layer = GetNamedAnimationLayer(value))
        if (auto playback = layer->Playback(); playback.is_some()) (*playback)->Play();
    return JS_UNDEFINED;
}
JSValue NamedLayerPause(JSContext*, JSValueConst value, int, JSValueConst*) {
    if (auto* layer = GetNamedAnimationLayer(value))
        if (auto playback = layer->Playback(); playback.is_some()) (*playback)->Pause();
    return JS_UNDEFINED;
}
JSValue NamedLayerStop(JSContext*, JSValueConst value, int, JSValueConst*) {
    if (auto* layer = GetNamedAnimationLayer(value))
        if (auto playback = layer->Playback(); playback.is_some()) (*playback)->Stop();
    return JS_UNDEFINED;
}
JSValue NamedLayerIsPlaying(JSContext* ctx, JSValueConst value, int, JSValueConst*) {
    if (auto* layer = GetNamedAnimationLayer(value))
        if (auto playback = layer->Playback(); playback.is_some())
            return JS_NewBool(ctx, (*playback)->IsPlaying());
    return JS_NewBool(ctx, false);
}
JSValue NamedLayerGetName(JSContext* ctx, JSValueConst value) {
    auto* layer = GetNamedAnimationLayer(value);
    if (! layer) return JS_UNDEFINED;
    auto name = rstd::cppstd::as_string_view(layer->Name());
    return JS_NewStringLen(ctx, name.data(), name.size());
}
JSValue NamedLayerGetFps(JSContext* ctx, JSValueConst value) {
    if (auto* layer = GetNamedAnimationLayer(value))
        if (auto playback = layer->Playback(); playback.is_some())
            return JS_NewFloat64(ctx, (*playback)->Fps());
    return JS_UNDEFINED;
}
JSValue NamedLayerGetFrameCount(JSContext* ctx, JSValueConst value) {
    if (auto* layer = GetNamedAnimationLayer(value))
        if (auto playback = layer->Playback(); playback.is_some())
            return JS_NewInt32(ctx, (*playback)->FrameCount().to_primitive());
    return JS_UNDEFINED;
}
JSValue NamedLayerGetDuration(JSContext* ctx, JSValueConst value) {
    if (auto* layer = GetNamedAnimationLayer(value))
        if (auto playback = layer->Playback(); playback.is_some())
            return JS_NewFloat64(ctx, (*playback)->Duration());
    return JS_UNDEFINED;
}
JSValue NamedLayerGetFrame(JSContext* ctx, JSValueConst value, int, JSValueConst*) {
    if (auto* layer = GetNamedAnimationLayer(value))
        if (auto playback = layer->Playback(); playback.is_some())
            return JS_NewInt32(ctx, (*playback)->Frame().to_primitive());
    return JS_UNDEFINED;
}
JSValue NamedLayerSetFrame(JSContext* ctx, JSValueConst value, int argc, JSValueConst* argv) {
    double frame;
    if (argc && JS_ToFloat64(ctx, &frame, argv[0]) == 0)
        if (auto* layer = GetNamedAnimationLayer(value))
            if (auto playback = layer->Playback(); playback.is_some())
                (*playback)->SetFrame(static_cast<float>(frame));
    return JS_UNDEFINED;
}

JSValue NamedAnimationLayerGetAnimation(JSContext* ctx, JSValueConst value, int argc,
                                        JSValueConst* argv) {
    auto* layer = static_cast<Arc<owe::SceneAnimationLayer>*>(
        JS_GetOpaque(value, s_named_animation_layer_class_id));
    if (! layer) return JS_UNDEFINED;
    if (argc == 0 || JS_IsUndefined(argv[0]))
        return WrapAnimation(ctx, ActivePropertyAnimation(ctx, argc, argv));
    const char* name = JS_ToCString(ctx, argv[0]);
    if (! name) return JS_EXCEPTION;
    auto animation = (*layer)->NamedAnimation(rstd::cppstd::as_str(name).unwrap());
    JS_FreeCString(ctx, name);
    if (animation.is_none()) return JS_UNDEFINED;
    return WrapAnimation(ctx, rstd::move(animation));
}

JSValue WrapNamedAnimationLayer(JSContext* ctx, Arc<owe::SceneAnimationLayer> layer) {
    JSValue object = JS_NewObjectClass(ctx, s_named_animation_layer_class_id);
    if (JS_IsException(object)) return object;
    JS_SetOpaque(object, new Arc<owe::SceneAnimationLayer>(rstd::move(layer)));
    return object;
}

JSValue NodeGetAnimationLayer(JSContext* ctx, JSValueConst value, int argc, JSValueConst* argv) {
    auto* node = GetLayerNode(value);
    if (! node || argc == 0) return JS_UNDEFINED;
    if (JS_IsNumber(argv[0])) {
        int32_t index;
        if (JS_ToInt32(ctx, &index, argv[0]) < 0 || index < 0) return JS_UNDEFINED;
        auto layer = node->AnimationLayer(usize(index));
        return layer.is_some() ? WrapNamedAnimationLayer(ctx, rstd::move(*layer)) : JS_UNDEFINED;
    }
    const char* name = JS_ToCString(ctx, argv[0]);
    if (! name) return JS_EXCEPTION;
    auto layer = node->AnimationLayer(rstd::cppstd::as_str(name).unwrap());
    JS_FreeCString(ctx, name);
    if (layer.is_none()) return JS_UNDEFINED;
    return WrapNamedAnimationLayer(ctx, rstd::move(*layer));
}

void InitNamedAnimationLayerClass(JSContext* ctx, JSRuntime* rt) {
    if (s_named_animation_layer_class_id == 0) JS_NewClassID(rt, &s_named_animation_layer_class_id);
    JSClassDef def {};
    def.class_name = "WWNamedAnimationLayer";
    def.finalizer  = NamedAnimationLayerFinalizer;
    JS_NewClass(rt, s_named_animation_layer_class_id, &def);
    JSValue                           proto   = JS_NewObject(ctx);
    static const JSCFunctionListEntry funcs[] = {
        JS_CFUNC_DEF("getAnimation", 1, NamedAnimationLayerGetAnimation),
        JS_CFUNC_DEF("play", 0, NamedLayerPlay),
        JS_CFUNC_DEF("pause", 0, NamedLayerPause),
        JS_CFUNC_DEF("stop", 0, NamedLayerStop),
        JS_CFUNC_DEF("isPlaying", 0, NamedLayerIsPlaying),
        JS_CFUNC_DEF("getFrame", 0, NamedLayerGetFrame),
        JS_CFUNC_DEF("setFrame", 1, NamedLayerSetFrame),
        JS_CGETSET_DEF("name", NamedLayerGetName, NodeSetIgnore),
        JS_CGETSET_DEF("fps", NamedLayerGetFps, NodeSetIgnore),
        JS_CGETSET_DEF("frameCount", NamedLayerGetFrameCount, NodeSetIgnore),
        JS_CGETSET_DEF("duration", NamedLayerGetDuration, NodeSetIgnore),
        JS_CGETSET_DEF("blend", NamedLayerGetBlend, NamedLayerSetBlend),
        JS_CGETSET_DEF("rate", NamedLayerGetRate, NamedLayerSetRate),
        JS_CGETSET_DEF("visible", NamedLayerGetVisible, NamedLayerSetVisible),
    };
    JS_SetPropertyFunctionList(ctx, proto, funcs, sizeof(funcs) / sizeof(funcs[0]));
    JS_SetClassProto(ctx, s_named_animation_layer_class_id, proto);
}

JSValue NodeGetAnimation(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* handle = GetLayerHandle(this_val);
    if (handle == nullptr) return MakeAnimationStub(ctx);
    if (handle->property_object)
        return WrapAnimation(ctx, ActivePropertyAnimation(ctx, argc, argv));

    auto* node = ResolveLayerNode(handle);
    if (node == nullptr || argc == 0 || JS_IsUndefined(argv[0]) || JS_IsNull(argv[0]))
        return MakeAnimationStub(ctx);
    const char* name = JS_ToCString(ctx, argv[0]);
    if (name == nullptr || *name == '\0') {
        if (name != nullptr) JS_FreeCString(ctx, name);
        return MakeAnimationStub(ctx);
    }
    auto playback = node->NamedAnimation(rstd::cppstd::as_str(name).unwrap());
    JS_FreeCString(ctx, name);
    return WrapAnimation(ctx, rstd::move(playback));
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
    JS_CGETSET_DEF("parallaxDepth", NodeGetParallaxDepth, NodeSetParallaxDepth),
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
    JS_CFUNC_DEF("getLayerCount", 0, NodeSceneGetLayerCount),
    JS_CFUNC_DEF("getEffect", 1, NodeGetEffect),
    JS_CFUNC_DEF("getEffectCount", 0, NodeGetEffectCount),
    JS_CFUNC_DEF("enumerateLayers", 0, NodeSceneEnumerateLayers),
    JS_CFUNC_DEF("getInitialLayerConfig", 1, NodeSceneGetInitialLayerConfig),
    JS_CFUNC_DEF("getCameraTransforms", 0, NodeSceneGetCameraTransforms),
    JS_CFUNC_DEF("setCameraTransforms", 1, NodeSceneSetCameraTransforms),
    JS_CFUNC_DEF("getBoneCount", 0, NodeGetBoneCount),
    JS_CFUNC_DEF("getBoneIndex", 1, NodeGetBoneIndex),
    JS_CFUNC_DEF("getBoneTransform", 1, NodeGetBoneTransform),
    JS_CFUNC_DEF("setBoneTransform", 2, NodeSetBoneTransform),
    JS_CFUNC_DEF("getTextureAnimation", 0, NodeGetTextureAnimation),
    JS_CFUNC_DEF("getVideoTexture", 0, NodeGetVideoTexture),
    JS_CFUNC_DEF("emitParticles", 1, NodeEmitParticles),
    JS_CFUNC_DEF("getAnimation", 1, NodeGetAnimation),
    JS_CFUNC_DEF("getAnimationLayer", 1, NodeGetAnimationLayer),
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
    JS_CFUNC_DEF("getAnimation", 1, PropertyObjectGetAnimation),
};

const JSCFunctionListEntry s_material_proto_funcs[] = {
    JS_CFUNC_DEF("getAnimation", 1, PropertyObjectGetAnimation),
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
    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx,
                               proto,
                               s_material_proto_funcs,
                               sizeof(s_material_proto_funcs) / sizeof(s_material_proto_funcs[0]));
    JS_SetClassProto(ctx, s_material_class_id, proto);
}

// Stash the bootstrap's `thisLayer` / `thisScene` stubs for restore.
void CaptureDefaultBindings(JSContext* ctx) {
    auto*   host        = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    JSValue g           = JS_GetGlobalObject(ctx);
    host->default_layer = JS_GetPropertyStr(ctx, g, "thisLayer");
    host->default_scene = JS_GetPropertyStr(ctx, g, "thisScene");
    JS_FreeValue(ctx, g);
}

void BindThisContext(JSContext* ctx, JSValueConst layer, JSValueConst object) {
    JSValue g = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, "thisLayer", JS_DupValue(ctx, layer));
    JS_SetPropertyStr(ctx, g, "thisObject", JS_DupValue(ctx, object));
    JS_FreeValue(ctx, g);
}

void BindFieldScriptContext(JSContext* ctx, const FieldScript::Impl& script,
                            JSValueConst default_layer) {
    JSValueConst layer =
        JS_IsUndefined(script.wrapped_layer) ? default_layer : script.wrapped_layer;
    JSValueConst object = JS_IsUndefined(script.wrapped_object) ? layer : script.wrapped_object;
    BindThisContext(ctx, layer, object);
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
    JS_DefinePropertyValueStr(
        ctx,
        ev,
        "title",
        JS_NewStringLen(ctx,
                        rstd::cppstd::as_string_view(status.title.as_str()).data(),
                        status.title.len().to_primitive()),
        JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(
        ctx,
        ev,
        "artist",
        JS_NewStringLen(ctx,
                        rstd::cppstd::as_string_view(status.artist.as_str()).data(),
                        status.artist.len().to_primitive()),
        JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(
        ctx,
        ev,
        "album",
        JS_NewStringLen(ctx,
                        rstd::cppstd::as_string_view(status.album.as_str()).data(),
                        status.album.len().to_primitive()),
        JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(
        ctx,
        ev,
        "albumTitle",
        JS_NewStringLen(ctx,
                        rstd::cppstd::as_string_view(status.album.as_str()).data(),
                        status.album.len().to_primitive()),
        JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(
        ctx,
        ev,
        "albumArtist",
        JS_NewStringLen(ctx,
                        rstd::cppstd::as_string_view(status.album_artist.as_str()).data(),
                        status.album_artist.len().to_primitive()),
        JS_PROP_C_W_E);
    return ev;
}

JSValue MakeMediaThumbnailEvent(JSContext* ctx, const MediaStatus& status) {
    JSValue ev = JS_NewObject(ctx);
    JS_DefinePropertyValueStr(
        ctx, ev, "hasThumbnail", JS_NewBool(ctx, ! status.art_url.is_empty()), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(
        ctx,
        ev,
        "thumbnail",
        JS_NewStringLen(ctx,
                        rstd::cppstd::as_string_view(status.art_url.as_str()).data(),
                        status.art_url.len().to_primitive()),
        JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(
        ctx,
        ev,
        "artUrl",
        JS_NewStringLen(ctx,
                        rstd::cppstd::as_string_view(status.art_url.as_str()).data(),
                        status.art_url.len().to_primitive()),
        JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(
        ctx,
        ev,
        "previousThumbnail",
        JS_NewStringLen(ctx,
                        rstd::cppstd::as_string_view(status.previous_art_url.as_str()).data(),
                        status.previous_art_url.len().to_primitive()),
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

JsRuntime::JsRuntime(): m_impl(Box<Impl>::make()) {
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
        m_impl->rt, /*normalize=*/nullptr, programs::LoadBuiltinModule, /*opaque=*/nullptr);
    InitLayerClass(m_impl->ctx, m_impl->rt);
    InitParticleInstanceClass(m_impl->ctx, m_impl->rt);
    InitEffectClass(m_impl->ctx, m_impl->rt);
    InitMaterialClass(m_impl->ctx, m_impl->rt);
    InitTexAnimClass(m_impl->ctx, m_impl->rt);
    InitAnimationClass(m_impl->ctx, m_impl->rt);
    InitNamedAnimationLayerClass(m_impl->ctx, m_impl->rt);
    InitVideoTextureClass(m_impl->ctx, m_impl->rt);
    InstallEngineGlobal(m_impl->ctx);
    // Bootstrap created stub `thisLayer` / `thisScene` on globalThis.
    // Capture them now so per-script binding can fall back to the stub
    // when no SceneNode is provided.
    CaptureDefaultBindings(m_impl->ctx);
}

void ReleaseFieldScriptValues(JSContext* ctx, FieldScript& script) {
    auto& field = *script.m_impl;
    JS_FreeValue(ctx, field.update_fn);
    JS_FreeValue(ctx, field.animation_event_fn);
    JS_FreeValue(ctx, field.init_fn);
    JS_FreeValue(ctx, field.module_ns);
    JS_FreeValue(ctx, field.deferred_module);
    JS_FreeValue(ctx, field.current_value);
    if (! JS_IsUndefined(field.wrapped_layer)) JS_FreeValue(ctx, field.wrapped_layer);
    if (! JS_IsUndefined(field.wrapped_object)) JS_FreeValue(ctx, field.wrapped_object);
}

JsRuntime::~JsRuntime() {
    for (auto& fs : m_impl->scripts) ReleaseFieldScriptValues(m_impl->ctx, *fs);
    for (auto& fs : m_impl->deferred_scripts) ReleaseFieldScriptValues(m_impl->ctx, *fs);
    m_impl->scripts.clear();
    m_impl->deferred_scripts.clear();
    for (auto ns : m_impl->ns_by_sha.values()) JS_FreeValue(m_impl->ctx, *ns);
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
    PruneSceneLayerBindings(m_impl->ctx, m_impl->host, nullptr);
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

void JsRuntime::SetUserProperty(ref<str> key, const Json& property) {
    if (! m_impl->ctx) return;
    auto key_bytes = Vec<u8>::from(key.as_bytes());
    key_bytes.push(u8());
    const char* key_str =
        reinterpret_cast<const char*>(rstd::as_bytes(key_bytes.as_slice()).as_raw_ptr());
    JSContext* ctx    = m_impl->ctx;
    JSValue    global = JS_GetGlobalObject(ctx);
    JSValue    engine = JS_GetPropertyStr(ctx, global, "engine");
    JSValue    props  = JS_GetPropertyStr(ctx, engine, "userProperties");
    if (! JS_IsObject(props)) {
        JS_FreeValue(ctx, props);
        props = JS_NewObject(ctx);
        JS_DefinePropertyValueStr(
            ctx, engine, "userProperties", JS_DupValue(ctx, props), JS_PROP_C_W_E);
    }
    JS_DefinePropertyValueStr(
        ctx, props, key_str, UserPropertyValueToJs(ctx, property), JS_PROP_C_W_E);

    JSValue changed = JS_NewObject(ctx);
    JS_DefinePropertyValueStr(
        ctx, changed, key_str, UserPropertyValueToJs(ctx, property), JS_PROP_C_W_E);
    const auto script_count = m_impl->scripts.len();
    for (usize index {}; index < script_count; ++index) {
        auto* fs = m_impl->scripts[index].get();
        auto* I  = fs->m_impl.get();
        if (! I->alive || I->init_deferred) continue;
        JSValue fn = JS_GetPropertyStr(ctx, I->module_ns, "applyUserProperties");
        if (JS_IsFunction(ctx, fn)) {
            BindFieldScriptContext(ctx, *I, m_impl->host.default_layer);
            m_impl->host.active_field_script = fs;
            JSValue arg                      = JS_DupValue(ctx, changed);
            JSValue r                        = JS_Call(ctx, fn, JS_UNDEFINED, 1, &arg);
            JS_FreeValue(ctx, arg);
            if (JS_IsException(r))
                m_impl->LogError(ctx, I->sha.as_str(), "applyUserProperties threw");
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
    if (! m_impl->ctx) return;
    JSContext* ctx         = m_impl->ctx;
    auto&      host        = m_impl->host;
    const bool first       = ! host.media_initialized;
    const auto prev        = rstd::move(host.media);
    host.media             = status.clone();
    host.media_initialized = true;

    const bool playback_changed   = first || prev.state != status.state;
    const bool properties_changed = first || prev.title != status.title ||
                                    prev.artist != status.artist || prev.album != status.album ||
                                    prev.album_artist != status.album_artist;
    const bool thumbnail_changed =
        first || prev.art_url != status.art_url || prev.previous_art_url != status.previous_art_url;
    if (! playback_changed && ! properties_changed && ! thumbnail_changed) return;

    const auto script_count = m_impl->scripts.len();
    for (usize index {}; index < script_count; ++index) {
        auto* fs = m_impl->scripts[index].get();
        auto* I  = fs->m_impl.get();
        if (! I->alive || I->init_deferred) continue;
        BindFieldScriptContext(ctx, *I, m_impl->host.default_layer);
        m_impl->host.active_field_script = fs;
        if (playback_changed) {
            JSValue ev = MakeMediaPlaybackEvent(ctx, status);
            InvokeEventCallback(
                ctx, I->module_ns, "mediaPlaybackChanged", ev, m_impl.get(), I->sha.as_str());
            JS_FreeValue(ctx, ev);
        }
        if (properties_changed) {
            JSValue ev = MakeMediaPropertiesEvent(ctx, status);
            InvokeEventCallback(
                ctx, I->module_ns, "mediaPropertiesChanged", ev, m_impl.get(), I->sha.as_str());
            JS_FreeValue(ctx, ev);
        }
        if (thumbnail_changed) {
            JSValue ev = MakeMediaThumbnailEvent(ctx, status);
            InvokeEventCallback(
                ctx, I->module_ns, "mediaThumbnailChanged", ev, m_impl.get(), I->sha.as_str());
            JS_FreeValue(ctx, ev);
        }
    }
    m_impl->host.active_field_script = nullptr;
}

void JsRuntime::SetBoneResolvers(BoneIndexResolver index_resolver, BoneCountResolver count_resolver,
                                 BoneTransformResolver    transform_resolver,
                                 BoneSetTransformResolver set_transform_resolver) {
    m_impl->host.bone_index_resolver         = Some(rstd::move(index_resolver));
    m_impl->host.bone_count_resolver         = Some(rstd::move(count_resolver));
    m_impl->host.bone_transform_resolver     = Some(rstd::move(transform_resolver));
    m_impl->host.bone_set_transform_resolver = Some(rstd::move(set_transform_resolver));
}

void JsRuntime::SetPersistence(PathBuf path) {
    m_impl->host.ls_path = rstd::move(path);
    m_impl->host.ls_data.clear();
    m_impl->host.ls_pending.clear();
    (void)LoadLocalStorage(&m_impl->host, m_impl->host.ls_path);
}

namespace
{
void RunFieldScriptInit(JSContext* ctx, JsRuntime::Impl* rt, FieldScript* fs);
}

void JsRuntime::SetScene(owe::Scene* scene) {
    m_impl->host.scene = scene;
    if (scene) m_impl->host.registered_layer_queries = true;
}

void JsRuntime::InvalidateLayer(owe::SceneNode& node) {
    InvalidateLayerPointer(rstd::addressof(node));
}

void JsRuntime::InvalidateLayerPointer(owe::SceneNode* pointer) {
    auto& host = m_impl->host;
    for (auto& timer : host.deferred) {
        if (timer.dead || ! timer.owner || timer.owner->m_impl->node != pointer) continue;
        timer.dead  = true;
        timer.owner = nullptr;
    }
    auto invalidate_scripts = [&](Vec<Box<FieldScript>>& scripts) {
        for (auto& script : scripts) {
            auto& field = *script->m_impl;
            if (field.node == pointer) {
                field.node  = nullptr;
                field.alive = false;
            }
        }
    };
    invalidate_scripts(m_impl->scripts);
    invalidate_scripts(m_impl->deferred_scripts);
    auto state = host.layer_states.remove(pointer);
    if (state.is_some()) (*state)->node = nullptr;
    auto object = host.layer_objects.remove(pointer);
    if (object.is_some()) JS_FreeValue(m_impl->ctx, *object);
    (void)host.script_layers.remove(pointer);
    (void)host.text_setters.remove(pointer);
    (void)host.current_text_strings.remove(pointer);
    (void)host.text_align_hooks.remove(pointer);
    (void)host.node_origin_hooks.remove(pointer);
    (void)host.image_alignment_hooks.remove(pointer);
    (void)host.initial_layer_configs.remove(pointer);
}

void JsRuntime::SetInitializationOrder(FieldScript& script, rstd::uint64_t order) {
    if (script.m_impl->rt != m_impl.get() || script.m_impl->init_done) return;
    script.m_impl->initialization_order = order;
}

void JsRuntime::RegisterInitialLayerConfig(owe::SceneNode* node, Json config) {
    if (node == nullptr) return;
    (void)m_impl->host.initial_layer_configs.insert(node, rstd::move(config));
    (void)m_impl->host.script_layers.insert(node);
    m_impl->host.registered_layer_queries = true;
}

void JsRuntime::SetSceneRoot(owe::SceneNode* root) {
    if (! m_impl->ctx) return;
    if (m_impl->scene_root && m_impl->scene_root != root) {
        auto invalidate_removed = [&](Vec<Box<FieldScript>>& scripts) {
            for (auto& script : scripts) {
                auto* node = script->m_impl->node;
                if (node && ! TreeContains(root, node)) InvalidateLayerPointer(node);
            }
        };
        invalidate_removed(m_impl->scripts);
        invalidate_removed(m_impl->deferred_scripts);
        PruneSceneLayerBindings(m_impl->ctx, m_impl->host, root);
    }
    if (! JS_IsUndefined(m_impl->wrapped_scene)) JS_FreeValue(m_impl->ctx, m_impl->wrapped_scene);
    m_impl->scene_root      = root;
    m_impl->host.scene_root = root;
    m_impl->wrapped_scene   = root ? WrapLayerNode(m_impl->ctx, root) : JS_UNDEFINED;
    if (! JS_IsUndefined(m_impl->wrapped_scene)) BindThisScene(m_impl->ctx, m_impl->wrapped_scene);
    struct PendingScript {
        FieldScript* script;
        usize        registration;
    };
    Vec<PendingScript> pending;
    pending.reserve(m_impl->scripts.len());
    for (usize i; i < m_impl->scripts.len(); ++i) {
        auto* script = m_impl->scripts[i].get();
        if (! script->m_impl->init_done && ! script->m_impl->init_deferred)
            pending.push({ script, i });
    }
    sort_unstable_by(pending.deref_mut(), [](const auto& left, const auto& right) {
        const auto a = left.script->m_impl->initialization_order;
        const auto b = right.script->m_impl->initialization_order;
        return a < b || (a == b && left.registration < right.registration);
    });
    for (const auto& entry : pending) RunFieldScriptInit(m_impl->ctx, m_impl.get(), entry.script);
}

void JsRuntime::TickAll(slice<owe::SceneAnimationEventDispatch> animation_events) {
    JSContext* ctx = m_impl->ctx;
    SweepDeferred(ctx, &m_impl->host);
    const auto script_count = usize(m_impl->scripts.len().to_primitive());

    // Node animations run before scripts; cached script returns must not erase their values.
    for (usize index {}; index < script_count; ++index) {
        auto* script = m_impl->scripts[index]->m_impl.get();
        if (script->alive && ! script->init_deferred &&
            script->object_kind == ScriptPropertyObjectKind::AnimationLayer &&
            script->animation_layer.is_some() &&
            (*script->animation_layer)->FieldAnimation(script->property.as_str()).is_some()) {
            auto&   layer   = **script->animation_layer;
            JSValue current = script->property == "blend"_str  ? JS_NewFloat64(ctx, layer.Blend())
                              : script->property == "rate"_str ? JS_NewFloat64(ctx, layer.Rate())
                              : script->property == "visible"_str ? JS_NewBool(ctx, layer.Visible())
                                                                  : JS_UNDEFINED;
            auto    value   = CoerceReturn(ctx, current, script->kind);
            JS_FreeValue(ctx, current);
            if (! value.is_Empty()) {
                JS_FreeValue(ctx, script->current_value);
                script->current_value = ScriptValueToJs(ctx, value);
                script->last_value    = rstd::move(value);
            }
            continue;
        }
        if (! script->alive || script->init_deferred || script->node == nullptr ||
            script->object_kind != ScriptPropertyObjectKind::Layer ||
            ! script->node->HasFieldAnimationTrack(script->property.as_str()))
            continue;
        auto property = CString::make(Vec<u8>::from(script->property.as_str().as_bytes())).unwrap();
        JSValue current = JS_GetPropertyStr(ctx, script->wrapped_object, property.as_ptr());
        if (JS_IsException(current)) {
            m_impl->LogError(ctx, script->sha.as_str(), "animated property read failed");
            JS_FreeValue(ctx, current);
            continue;
        }
        auto value = CoerceReturn(ctx, current, script->kind);
        JS_FreeValue(ctx, current);
        if (value.is_Empty()) continue;
        JSValue next = ScriptValueToJs(ctx, value);
        JS_FreeValue(ctx, script->current_value);
        script->current_value = next;
        script->last_value    = rstd::move(value);
    }

    // Cursor event dispatch. For every script bound to a SceneNode, hit-
    // test the cursor against the node's world AABB and fire any of
    // cursorEnter/Leave/Move/Down/Up/Click that the script's module
    // exports. Runs before update() so update can react to state writes
    // the callbacks made this frame.
    const CursorWorld cursor    = CursorToWorld(m_impl->host.inputs, m_impl->host.scene);
    const bool        in_window = m_impl->host.inputs.cursor_in_window;
    const bool        cursor_moved =
        in_window && (! m_impl->cursor_was_in_window ||
                      m_impl->host.inputs.cursor_x != m_impl->previous_cursor_x ||
                      m_impl->host.inputs.cursor_y != m_impl->previous_cursor_y);
    m_impl->previous_cursor_x        = m_impl->host.inputs.cursor_x;
    m_impl->previous_cursor_y        = m_impl->host.inputs.cursor_y;
    m_impl->cursor_was_in_window     = in_window;
    const rstd::uint32_t btn_pressed = m_impl->host.inputs.mouse_buttons_pressed;
    const rstd::uint32_t btn_release = m_impl->host.inputs.mouse_buttons_released;
    JSValue              ev_shared   = JS_UNDEFINED;
    auto                 ensure_ev   = [&](int button) -> JSValue {
        if (! JS_IsUndefined(ev_shared)) JS_FreeValue(ctx, ev_shared);
        ev_shared = MakeCursorEvent(ctx, cursor, button);
        return ev_shared;
    };
    for (usize script_index {}; script_index < script_count; ++script_index) {
        auto* fs = m_impl->scripts[script_index].get();
        auto* I  = fs->m_impl.get();
        if (! I->alive || I->init_deferred || ! I->node) continue;
        const bool now_inside = in_window && HitTestNode(I->node, cursor);
        BindFieldScriptContext(ctx, *I, m_impl->host.default_layer);
        m_impl->host.active_field_script = fs;
        if (now_inside != I->cursor_inside) {
            InvokeEventCallback(ctx,
                                I->module_ns,
                                now_inside ? "cursorEnter" : "cursorLeave",
                                ensure_ev(-1),
                                m_impl.get(),
                                I->sha.as_str());
            I->cursor_inside = now_inside;
        }
        if (now_inside && cursor_moved) {
            InvokeEventCallback(
                ctx, I->module_ns, "cursorMove", ensure_ev(-1), m_impl.get(), I->sha.as_str());
        }
        if (btn_pressed && now_inside) {
            for (int b = 0; b < 3; ++b) {
                if (btn_pressed & (1u << b)) {
                    InvokeEventCallback(ctx,
                                        I->module_ns,
                                        "cursorDown",
                                        ensure_ev(b),
                                        m_impl.get(),
                                        I->sha.as_str());
                    InvokeEventCallback(ctx,
                                        I->module_ns,
                                        "cursorClick",
                                        ensure_ev(b),
                                        m_impl.get(),
                                        I->sha.as_str());
                }
            }
        }
        if (btn_release && now_inside) {
            for (int b = 0; b < 3; ++b) {
                if (btn_release & (1u << b)) {
                    InvokeEventCallback(
                        ctx, I->module_ns, "cursorUp", ensure_ev(b), m_impl.get(), I->sha.as_str());
                }
            }
        }
    }
    if (! JS_IsUndefined(ev_shared)) JS_FreeValue(ctx, ev_shared);

    for (const auto& dispatch : animation_events) {
        if (dispatch.node == nullptr) continue;
        for (usize script_index {}; script_index < script_count; ++script_index) {
            auto* fs = m_impl->scripts[script_index].get();
            auto* I  = fs->m_impl.get();
            if (! I->alive || I->init_deferred || I->node != dispatch.node ||
                JS_IsUndefined(I->animation_event_fn))
                continue;
            BindFieldScriptContext(ctx, *I, m_impl->host.default_layer);
            m_impl->host.active_field_script = fs;
            JSValue args[2]                  = { MakeAnimationEvent(ctx, dispatch.event),
                                                 JS_DupValue(ctx, I->current_value) };
            JSValue ret = JS_Call(ctx, I->animation_event_fn, JS_UNDEFINED, 2, args);
            JS_FreeValue(ctx, args[0]);
            JS_FreeValue(ctx, args[1]);
            if (JS_IsException(ret)) {
                m_impl->LogError(ctx, I->sha.as_str(), "animationEvent threw");
                JS_FreeValue(ctx, ret);
                continue;
            }
            // The callback may hand back a replacement property value, the
            // same way update() does; keep it for the update() that follows.
            auto value = CoerceReturn(ctx, ret, I->kind);
            if (! value.is_Empty()) {
                JSValue next_value = ScriptValueToJs(ctx, value);
                JS_FreeValue(ctx, I->current_value);
                I->current_value = next_value;
                I->last_value    = rstd::move(value);
            }
            JS_FreeValue(ctx, ret);
        }
    }
    m_impl->host.active_field_script = nullptr;

    for (usize script_index {}; script_index < script_count; ++script_index) {
        auto* fs = m_impl->scripts[script_index].get();
        auto* I  = fs->m_impl.get();
        if (! I->alive || I->init_deferred) continue;
        if (JS_IsUndefined(I->update_fn)) continue;
        // Swap `thisLayer` to this script's bound node before update. When
        // unbound, restore the original stub captured at bootstrap.
        BindFieldScriptContext(ctx, *I, m_impl->host.default_layer);
        m_impl->host.active_field_script = fs;
        JSValue ret;
        if (I->update_takes_arg) {
            JSValue args[1] = { JS_DupValue(ctx, I->current_value) };
            ret             = JS_Call(ctx, I->update_fn, JS_UNDEFINED, 1, args);
            JS_FreeValue(ctx, args[0]);
        } else {
            ret = JS_Call(ctx, I->update_fn, JS_UNDEFINED, 0, nullptr);
        }
        if (JS_IsException(ret)) {
            m_impl->LogError(ctx, I->sha.as_str(), "update threw");
            JS_FreeValue(ctx, ret);
            continue;
        }
        I->last_value = CoerceReturn(ctx, ret, I->kind);
        // Keep the next argument in the field's coerced shape. Vec3 scripts
        // often return a scalar for scale, but still read value.x next frame.
        if (I->update_takes_arg && ! I->last_value.is_Empty()) {
            JSValue next_value = ScriptValueToJs(ctx, I->last_value);
            JS_FreeValue(ctx, I->current_value);
            I->current_value = next_value;
        }
        JS_FreeValue(ctx, ret);
    }
    m_impl->host.active_field_script = nullptr;
}

bool JsRuntime::Empty() const noexcept {
    return m_impl->scripts.is_empty() && m_impl->deferred_scripts.is_empty();
}

void JsRuntime::ForEachScript(EachFn fn, void* user) {
    const auto count = m_impl->scripts.len();
    for (usize i; i < count; ++i) fn(m_impl->scripts[i].get(), user);
}

void JsRuntime::RegisterTextSetter(owe::SceneNode* node, JsRuntime::TextSetter setter,
                                   ref<str> initial_text) {
    if (node == nullptr) return;
    (void)m_impl->host.text_setters.insert(node, rstd::move(setter));
    (void)m_impl->host.current_text_strings.insert(node, String::make(initial_text));
}

void JsRuntime::RegisterTextAlignSetters(owe::SceneNode* node, String horizontal, String vertical,
                                         double                             point_size,
                                         JsRuntime::TextAlignSetter         set_horizontal,
                                         JsRuntime::TextAlignSetter         set_vertical,
                                         Option<JsRuntime::PointSizeGetter> get_point_size,
                                         Option<JsRuntime::PointSizeSetter> set_point_size) {
    if (node == nullptr) return;
    (void)m_impl->host.text_align_hooks.insert(node,
                                               EngineHostState::TextAlignHooks {
                                                   .horizontal     = rstd::move(horizontal),
                                                   .vertical       = rstd::move(vertical),
                                                   .point_size     = point_size,
                                                   .set_horizontal = rstd::move(set_horizontal),
                                                   .set_vertical   = rstd::move(set_vertical),
                                                   .get_point_size = rstd::move(get_point_size),
                                                   .set_point_size = rstd::move(set_point_size),
                                               });
}

void JsRuntime::RegisterNodeOriginAccessors(owe::SceneNode* node, NodeOriginGetter getter,
                                            NodeOriginSetter setter) {
    if (node == nullptr) return;
    (void)m_impl->host.node_origin_hooks.insert(
        node,
        EngineHostState::NodeOriginHooks { .getter = rstd::move(getter),
                                           .setter = rstd::move(setter) });
}

void JsRuntime::SetNodeParallaxDepthAccessors(NodeParallaxDepthGetter getter,
                                              NodeParallaxDepthSetter setter) {
    m_impl->host.node_parallax_depth_getter = Some(rstd::move(getter));
    m_impl->host.node_parallax_depth_setter = Some(rstd::move(setter));
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
    if (! fs || ! fs->m_impl->alive || fs->m_impl->init_done || fs->m_impl->init_in_progress)
        return;
    auto* I = fs->m_impl.get();
    if (! JS_IsFunction(ctx, I->init_fn)) {
        I->init_done = true;
        return;
    }

    EngineBindingGuard binding_guard(ctx, rt->host);
    I->init_in_progress = true;
    BindFieldScriptContext(ctx, *I, rt->host.default_layer);
    if (! JS_IsUndefined(rt->wrapped_scene)) BindThisScene(ctx, rt->wrapped_scene);
    rt->host.active_field_script = fs;
    JSValue arg                  = JS_DupValue(ctx, I->current_value);
    JSValue r                    = JS_Call(ctx, I->init_fn, JS_UNDEFINED, 1, &arg);
    JS_FreeValue(ctx, arg);
    if (JS_IsException(r)) rt->LogError(ctx, I->sha.as_str(), "init threw");
    JS_FreeValue(ctx, r);
    I->init_in_progress = false;
    I->init_done        = true;
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

bool EvaluateFieldScriptModule(JSContext* ctx, JsRuntime::Impl* rt, FieldScript& script,
                               JSValue compiled, const Json& properties_config,
                               const Json& initial_value) {
    auto*              I = script.m_impl.get();
    EngineBindingGuard binding_guard(ctx, rt->host);
    BindFieldScriptContext(ctx, *I, rt->host.default_layer);
    if (! JS_IsUndefined(rt->wrapped_scene)) BindThisScene(ctx, rt->wrapped_scene);
    rt->host.active_field_script = &script;
    rt->host.active_animation = I->animation.is_some() ? Some((*I->animation).clone())
                                                       : None<Arc<owe::SceneAnimationPlayback>>();
    JSModuleDef* module       = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(compiled));
    JSValue      result       = AwaitModuleEvaluation(ctx, JS_EvalFunction(ctx, compiled));
    if (JS_IsException(result)) {
        rt->LogError(ctx, I->sha.as_str(), "module eval failed");
        JS_FreeValue(ctx, result);
        for (auto& timer : rt->host.deferred) {
            if (timer.owner != &script) continue;
            timer.dead  = true;
            timer.owner = nullptr;
        }
        return false;
    }
    JS_FreeValue(ctx, result);
    JSValue ns          = JS_GetModuleNamespace(ctx, module);
    I->module_ns        = ns;
    JSValue workshop_id = JS_GetPropertyStr(ctx, ns, "__workshopId");
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
                auto owned_key                = Vec<u8>::from(entry_key->as_str().as_bytes());
                owned_key.push(u8());
                const auto& value = *entry_value;
                JS_DefinePropertyValueStr(ctx,
                                          hv,
                                          reinterpret_cast<const char*>(
                                              rstd::as_bytes(owned_key.as_slice()).as_raw_ptr()),
                                          ResolveConfigValue(ctx, value),
                                          JS_PROP_C_W_E);
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
    return true;
}

} // namespace

FieldScript* JsRuntime::MakeFieldScript(ref<str> source, ref<str> script_sha,
                                        FieldKind field_kind_in, const Json& properties_config,
                                        const Json& initial_value, ScriptBindingContext context,
                                        FieldScriptInitialization initialization) {
    JSContext* ctx = m_impl->ctx;
    if (! ctx) return nullptr;
    EngineBindingGuard binding_guard(ctx, m_impl->host);

    auto* node       = context.layer;
    auto  fs         = Box<FieldScript>::make();
    auto* I          = fs->m_impl.get();
    I->rt            = m_impl.get();
    I->ctx           = ctx;
    I->kind          = (field_kind_in == FieldKind::Unknown) ? FieldKind::Scalar : field_kind_in;
    I->node          = node;
    I->init_deferred = initialization == FieldScriptInitialization::Deferred;
    m_impl->host.active_field_script = fs.get();
    auto discard_pending_timers      = [&] {
        for (auto& timer : m_impl->host.deferred)
            if (timer.owner == fs.get()) timer.dead = true;
    };
    // Factories can initialize scripts before createLayer returns. Register the
    // owning layer before its module or init function can query the scene.
    if (node) (void)m_impl->host.script_layers.insert(node);
    JSValue wrapped_layer = node ? WrapLayerNode(ctx, node) : JS_UNDEFINED;
    JSValue wrapped_object { JS_UNDEFINED };
    switch (context.object_kind) {
    case ScriptPropertyObjectKind::Layer: wrapped_object = WrapLayerNode(ctx, node, true); break;
    case ScriptPropertyObjectKind::Effect:
        wrapped_object = WrapEffect(ctx, rstd::move(context.effect), node);
        break;
    case ScriptPropertyObjectKind::Material:
        wrapped_object = WrapMaterial(ctx, context.material, node);
        break;
    case ScriptPropertyObjectKind::AnimationLayer:
        wrapped_object = context.animation_layer.is_some()
                             ? WrapNamedAnimationLayer(ctx, (*context.animation_layer).clone())
                             : WrapAnimationLayer(ctx,
                                                  context.animation.is_some()
                                                      ? Some((*context.animation).clone())
                                                      : None<Arc<owe::SceneAnimationPlayback>>());
        break;
    }
    JSValueConst layer_value =
        JS_IsUndefined(wrapped_layer) ? m_impl->host.default_layer : wrapped_layer;
    JSValueConst object_value = JS_IsUndefined(wrapped_object) ? layer_value : wrapped_object;
    BindThisContext(ctx, layer_value, object_value);
    m_impl->host.active_animation = context.animation.is_some()
                                        ? Some((*context.animation).clone())
                                        : None<Arc<owe::SceneAnimationPlayback>>();
    I->property                   = rstd::move(context.property);
    I->object_kind                = context.object_kind;
    I->animation                  = rstd::move(context.animation);
    I->animation_layer            = rstd::move(context.animation_layer);
    I->wrapped_layer              = wrapped_layer;
    I->wrapped_object             = wrapped_object;

    // Each field script needs its own module and scriptProperties state.
    I->sha        = rstd::into(script_sha);
    auto filename = rstd::format("scripts/{}-{}.js", I->sha.as_str(), m_impl->next_module_serial++);
    auto fname    = CString::make(Vec<u8>::from(filename.as_str().as_bytes())).unwrap();
    auto bytes    = Vec<u8>::from(source.as_bytes());
    bytes.push(u8());
    JSValue compiled =
        JS_Eval(ctx,
                reinterpret_cast<const char*>(rstd::as_bytes(bytes.as_slice()).as_raw_ptr()),
                source.len().to_primitive(),
                fname.as_ptr(),
                JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(compiled)) {
        m_impl->LogError(ctx, script_sha, "compile failed");
        JS_FreeValue(ctx, compiled);
        ReleaseFieldScriptValues(ctx, *fs);
        discard_pending_timers();
        return nullptr;
    }
    if (I->init_deferred) {
        I->deferred_module        = compiled;
        I->deferred_properties    = Some(properties_config.clone());
        I->deferred_initial_value = Some(initial_value.clone());
    } else if (! EvaluateFieldScriptModule(
                   ctx, m_impl.get(), *fs, compiled, properties_config, initial_value)) {
        ReleaseFieldScriptValues(ctx, *fs);
        return nullptr;
    }

    auto* raw = fs.get();
    if (I->init_deferred) {
        m_impl->deferred_scripts.push(rstd::move(fs));
    } else {
        m_impl->scripts.push(rstd::move(fs));
        if (m_impl->scene_root) RunFieldScriptInit(ctx, m_impl.get(), raw);
    }
    return raw;
}

bool JsRuntime::InitializeFieldScript(FieldScript& script) {
    if (! m_impl->ctx || ! m_impl->scene_root || script.m_impl->rt != m_impl.get() ||
        ! script.m_impl->alive || ! script.m_impl->init_deferred ||
        script.m_impl->init_in_progress || JS_IsUndefined(script.m_impl->deferred_module))
        return false;
    for (usize index {}; index < m_impl->deferred_scripts.len(); ++index) {
        if (m_impl->deferred_scripts[index].get() != &script) continue;
        auto* field                   = script.m_impl.get();
        auto  module                  = field->deferred_module;
        field->deferred_module        = JS_UNDEFINED;
        field->init_in_progress       = true;
        const bool evaluated          = EvaluateFieldScriptModule(m_impl->ctx,
                                                                  m_impl.get(),
                                                                  script,
                                                                  module,
                                                                  *field->deferred_properties,
                                                                  *field->deferred_initial_value);
        field->init_in_progress       = false;
        field->deferred_properties    = None();
        field->deferred_initial_value = None();
        if (! evaluated || ! field->alive) return false;
        usize current {};
        while (current < m_impl->deferred_scripts.len() &&
               m_impl->deferred_scripts[current].get() != &script)
            ++current;
        if (current == m_impl->deferred_scripts.len()) return false;
        auto owned                   = m_impl->deferred_scripts.remove(current);
        script.m_impl->init_deferred = false;
        m_impl->scripts.push(rstd::move(owned));
        RunFieldScriptInit(m_impl->ctx, m_impl.get(), &script);
        return true;
    }
    return false;
}

bool JsRuntime::OwnsDeferredFieldScript(const FieldScript& script) const noexcept {
    return script.m_impl->init_deferred && ! script.m_impl->init_in_progress &&
           script.m_impl->rt == m_impl.as_ptr().as_raw_ptr();
}

bool JsRuntime::DiscardDeferredFieldScript(FieldScript& script) {
    for (usize index {}; index < m_impl->deferred_scripts.len(); ++index) {
        if (m_impl->deferred_scripts[index].get() != &script) continue;
        for (auto& timer : m_impl->host.deferred) {
            if (timer.owner != &script) continue;
            timer.dead  = true;
            timer.owner = nullptr;
        }
        auto owned = m_impl->deferred_scripts.remove(index);
        ReleaseFieldScriptValues(m_impl->ctx, *owned);
        return true;
    }
    return false;
}

void JsRuntime::PruneInactiveFieldScripts() {
    usize index {};
    while (index < m_impl->scripts.len()) {
        if (m_impl->scripts[index]->alive()) {
            ++index;
            continue;
        }
        auto script = m_impl->scripts.remove(index);
        ReleaseFieldScriptValues(m_impl->ctx, *script);
    }
}

} // namespace owe::script
