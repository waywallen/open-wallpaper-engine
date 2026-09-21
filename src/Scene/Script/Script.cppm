module;
#include <rstd/enum.hpp>

export module wescene.script;
export import owe.scene_audio_response;
import wescene.core;
import wescene.json;
import rstd;
import wescene.scene;

using namespace rstd::prelude;
using rstd::path::PathBuf;
using rstd::sync::Arc;

export namespace owe::script
{

// --- shared value variant ----------------------------------------------------

// Result of a script's update() call, after coercion. The variant is a
// snapshot of what the JS code returned this frame; the actuator (in the
// renderer) reads it and writes into the bound C++ field.
struct ScalarValue {
    double v { 0.0 };
};
struct Vec2Value {
    double x { 0.0 }, y { 0.0 };
};
struct Vec3Value {
    double x { 0.0 }, y { 0.0 }, z { 0.0 };
};
struct Vec4Value {
    double x { 0.0 }, y { 0.0 }, z { 0.0 }, w { 0.0 };
};
struct ColorValue {
    double r { 0.0 }, g { 0.0 }, b { 0.0 };
};
struct StringValue {
    String s;
};
struct BoolValue {
    bool v { false };
};

class ScriptValue {
    RSTD_ENUM_DEFAULT(ScriptValue, (Empty), (Empty), (Scalar, (ScalarValue value;)),
                      (Bool, (BoolValue value;)), (Vec2, (Vec2Value value;)),
                      (Vec3, (Vec3Value value;)), (Vec4, (Vec4Value value;)),
                      (Color, (ColorValue value;)), (String, (StringValue value;)))
public:
    ScriptValue(ScalarValue value): ScriptValue(Scalar(rstd::move(value))) {}
    ScriptValue(BoolValue value): ScriptValue(Bool(rstd::move(value))) {}
    ScriptValue(Vec2Value value): ScriptValue(Vec2(rstd::move(value))) {}
    ScriptValue(Vec3Value value): ScriptValue(Vec3(rstd::move(value))) {}
    ScriptValue(Vec4Value value): ScriptValue(Vec4(rstd::move(value))) {}
    ScriptValue(ColorValue value): ScriptValue(Color(rstd::move(value))) {}
    ScriptValue(StringValue value): ScriptValue(String(rstd::move(value))) {}
};

struct BoneTranslation {
    float x { 0.0f }, y { 0.0f }, z { 0.0f };
};

// What kind of value a FieldScript is expected to produce. Set at parse
// time based on the field name's well-known type — see the per-field-kind
// table in the API doc.
enum class FieldKind
{
    Unknown,
    Scalar,
    Bool,
    Vec2,
    Vec3,
    Vec4,
    Color,
    String
};

// --- frame inputs -----------------------------------------------------------

// One snapshot of host-supplied per-frame state, fed by the renderer into
// `JsRuntime::TickFieldScripts` once per frame. Mirrors the engine.* fields
// the audio-response cluster (and the parallax cluster) actually read.
struct FrameInputs {
    float                frametime { 0.0f };   // seconds since last frame
    float                runtime { 0.0f };     // seconds since wallpaper start
    float                time_of_day { 0.0f }; // 0..1, 0=midnight, 0.5=noon
    float                canvas_w { 1920.0f };
    float                canvas_h { 1080.0f };
    float                screen_w { 1920.0f };
    float                screen_h { 1080.0f };
    scene_audio::Buffers audio;
    // Cursor state. (cursor_x, cursor_y) is normalised canvas coords:
    // x ∈ [0,1] left-to-right, y ∈ [0,1] top-to-bottom. button bits use
    // GLFW numbering (left=0, right=1, middle=2). down is held-state,
    // pressed/released are edge events for this frame only.
    float    cursor_x { 0.0f }, cursor_y { 0.0f };
    bool     cursor_in_window { false };
    uint32_t mouse_buttons_down { 0 };
    uint32_t mouse_buttons_pressed { 0 };
    uint32_t mouse_buttons_released { 0 };
};

struct MediaStatus {
    uint32_t state { 0 };
    String   title;
    String   artist;
    String   album;
    String   album_artist;
    String   art_url;
    String   previous_art_url;
    auto     clone() const -> MediaStatus {
        return { state,
                 title.clone(),
                 artist.clone(),
                 album.clone(),
                 album_artist.clone(),
                 art_url.clone(),
                 previous_art_url.clone() };
    }
};

// --- script properties (configuration) --------------------------------------

// One descriptor produced by createScriptProperties().addX() calls inside
// the JS module. Captured at module-load time and then merged with the
// per-binding `scriptproperties` config from scene.json before exposing
// the resulting `scriptProperties.<name>` accessors back to the script.
struct PropDescriptor {
    enum class Kind
    {
        Slider,
        Checkbox,
        Text,
        Combo,
        Color,
        Delimiter,
        Other
    };
    Kind   kind { Kind::Other };
    String name;
    String label;
    Json   default_value; // captured verbatim
    double min { 0.0 };
    double max { 1.0 };
    bool   integer { false };
};

// --- runtime ----------------------------------------------------------------

class FieldScript;

enum class ScriptPropertyObjectKind
{
    Layer,
    Effect,
    Material,
    AnimationLayer,
};

struct ScriptBindingContext {
    owe::SceneNode*                          layer { nullptr };
    ScriptPropertyObjectKind                 object_kind { ScriptPropertyObjectKind::Layer };
    String                                   property;
    Option<Arc<owe::SceneAnimationPlayback>> animation;
    Option<owe::SceneImageEffectRef>         effect;
    owe::SceneMaterial*                      material { nullptr };

    ScriptBindingContext() = default;
    ScriptBindingContext(owe::SceneNode* value): layer(value) {}
    ScriptBindingContext(mut_ptr<owe::SceneNode> value): layer(value.as_raw_ptr()) {}

    static auto ForLayer(owe::SceneNode*, ref<str>,
                         Option<Arc<owe::SceneAnimationPlayback>> = None()) -> ScriptBindingContext;
    static auto ForAnimationLayer(owe::SceneNode*, ref<str>, Arc<owe::SceneAnimationPlayback>)
        -> ScriptBindingContext;
    static auto ForEffect(owe::SceneNode*, owe::SceneImageEffectRef, ref<str>,
                          Option<Arc<owe::SceneAnimationPlayback>> = None())
        -> ScriptBindingContext;
    static auto ForMaterial(owe::SceneNode*, owe::SceneMaterial*, ref<str>,
                            Option<Arc<owe::SceneAnimationPlayback>> = None())
        -> ScriptBindingContext;
};

struct LayerAssetReference {
    ref<str>         path;
    Option<ref<str>> workshop_id;
};

// One JsRuntime per Scene. Owns one JSRuntime and one JSContext. Compiled
// modules are deduped by sha so duplicated sources across many bound fields
// only allocate once. The runtime is not thread-safe; the renderer's frame
// tick is the single owner.
class JsRuntime : NoCopy, NoMove {
public:
    JsRuntime();
    ~JsRuntime();

    // Returns nullptr on hard compile/init failure (logs once).
    // `node` (nullable) is the SceneNode the script will see as `thisLayer`
    // inside init/update. When null, `thisLayer` falls back to a generic
    // stub (the JS-side default created at bootstrap).
    FieldScript* MakeFieldScript(ref<str> source, ref<str> script_sha, FieldKind field_kind,
                                 const Json& properties_config, const Json& initial_value,
                                 ScriptBindingContext context = {});

    // Pending scripts initialize in ascending owner order when SetSceneRoot
    // completes scene assembly. Equal orders retain creation order.
    void SetInitializationOrder(FieldScript& script, rstd::uint64_t order);

    // Preserve the authored scene.json object for getInitialLayerConfig().
    // The package parser supplies this snapshot before pending initializers run.
    void RegisterInitialLayerConfig(owe::SceneNode* node, Json config);

    // Install the Scene root that backs `thisScene`. `thisScene.getLayer(name)`
    // searches from this node. Call once per scene after parsing finishes.
    void SetScene(owe::Scene* scene);
    void SetSceneRoot(owe::SceneNode* root);

    // Wire localStorage to a JSON file. Existing keys load synchronously;
    // subsequent script writes flush back to the file. Pass an empty
    // string to revert to in-memory-only behaviour.
    void SetPersistence(PathBuf path);

    // Push one frame's worth of host state into the runtime. The next
    // FieldScript::Update call will see these values via `engine.*`.
    void SetFrameInputs(const FrameInputs& fi);
    void SetAudioResponseDemand(Option<Arc<AudioResponseDemand>> demand);

    // Patch one Wallpaper Engine user property into engine.userProperties.
    // `property` should be the descriptor object shape used by project.json
    // (`{value: ...}` plus optional metadata).
    void SetUserProperty(ref<str> key, const Json& property);

    // Dispatch Wallpaper Engine media callbacks for the current media
    // snapshot. Call from the renderer owner thread.
    void SetMediaStatus(const MediaStatus& status);

    using BoneIndexResolver = Box<dyn<FnMut<uint32_t(owe::SceneNode*, ref<str>)>>>;
    using BoneTransformResolver =
        Box<dyn<FnMut<Option<BoneTranslation>(owe::SceneNode*, uint32_t, double)>>>;
    void SetBoneResolvers(BoneIndexResolver     index_resolver,
                          BoneTransformResolver transform_resolver);

    // Drive every alive FieldScript once. Invokes their cached `update`
    // export and stores the coerced return into FieldScript::last_value().
    // Exceptions are caught and logged once per script_sha.
    void TickAll(slice<owe::SceneAnimationEventDispatch> animation_events = {});

    // Walk every live FieldScript created by this runtime. Caller-provided
    // function gets a non-owning pointer; the renderer uses this to push
    // last_value() into per-field actuators.
    using EachFn = void (*)(FieldScript*, void*);
    void ForEachScript(EachFn fn, void* user);
    bool Empty() const noexcept;

    // Wire a text-content setter for a given SceneNode. When a script does
    // `thisLayer.text = "..."` on a wrapper whose opaque is `node`, the JS
    // setter dispatches into this callback. Used by text layers to receive
    // text writes from scripts bound to non-text fields (e.g. clock
    // scripts attached to `visible`).
    using TextSetter      = Arc<dyn<FnMut<void(ref<str>)>>>;
    using PointSizeGetter = Arc<dyn<FnMut<double()>>>;
    using PointSizeSetter = Arc<dyn<FnMut<void(double)>>>;
    void RegisterTextSetter(owe::SceneNode* node, TextSetter setter);
    void RegisterTextAlignSetters(owe::SceneNode* node, String horizontal, String vertical,
                                  double point_size, TextSetter set_horizontal,
                                  TextSetter              set_vertical,
                                  Option<PointSizeGetter> get_point_size = {},
                                  Option<PointSizeSetter> set_point_size = {});
    using NodeOriginGetter = Arc<dyn<FnMut<Vec3Value()>>>;
    using NodeOriginSetter = Arc<dyn<FnMut<void(Vec3Value)>>>;
    void RegisterNodeOriginAccessors(owe::SceneNode* node, NodeOriginGetter getter,
                                     NodeOriginSetter setter);
    using NodeParallaxDepthGetter = Arc<dyn<FnMut<Option<Vec2Value>(owe::SceneNode*)>>>;
    using NodeParallaxDepthSetter = Arc<dyn<FnMut<void(owe::SceneNode*, Vec2Value)>>>;
    void SetNodeParallaxDepthAccessors(NodeParallaxDepthGetter getter,
                                       NodeParallaxDepthSetter setter);
    using ImageAlignmentSetter = Arc<dyn<FnMut<void(owe::SceneNode*, ref<str>)>>>;
    void RegisterImageAlignmentSetter(owe::SceneNode* node, ref<str> alignment,
                                      ImageAlignmentSetter setter);
    void CloneImageAlignmentBinding(owe::SceneNode* source, owe::SceneNode* clone);

    using LayerFactory =
        Arc<dyn<FnMut<Option<Arc<owe::SceneNode>>(owe::SceneNode*, LayerAssetReference)>>>;
    void SetLayerFactory(LayerFactory factory);
    using LayerConfigFactory = Arc<dyn<FnMut<Option<Arc<owe::SceneNode>>(owe::SceneNode*, Json)>>>;
    void SetLayerConfigFactory(LayerConfigFactory factory);
    void ClearLayerFactory();
    void ClearLayerConfigFactory();

    // Same exposure rule as FieldScript::Impl above: opaque outside the
    // module, but visible to peer module impl files.
    struct Impl;
    Box<Impl> m_impl;
};

class FieldScript : NoCopy, NoMove {
public:
    FieldScript();
    ~FieldScript();

    FieldKind          field_kind() const noexcept;
    const ScriptValue& last_value() const noexcept;
    bool               alive() const noexcept;
    ref<str>           script_sha() const noexcept;
    slice<String>      RegisteredAssets() const noexcept;
    Option<ref<str>>   WorkshopId() const noexcept;

    // Impl is intentionally exposed inside the wescene.script module so
    // JsRuntime::Impl (in the same module) can mutate it directly. Treated
    // as opaque by every other consumer; see Script.cpp.
    struct Impl;
    Box<Impl> m_impl;
};

// --- per-Scene script runtime + actuators -----------------------------------

// Where on a SceneNode the transform-style script value should be written.
// Used by MakeNodeTransformApply to manufacture the corresponding closure.
enum class NodeTransformTarget
{
    Translate, // Vec3 → SceneNode m_translate (origin field)
    Scale,     // Vec3 → SceneNode m_scale (scale field)
    Rotation,  // Vec3 → SceneNode m_rotation (angles field)
};

// One write-back binding from script.last_value() to whatever subsystem
// owns the bound field. The closure does the type coercion + write; the
// generic ScriptScene::Tick has no idea what 'apply' does.
using ScriptApply = Box<dyn<FnMut<void(const ScriptValue&)>>>;

struct Actuator {
    Actuator(FieldScript* value, ScriptApply callback)
        : script(value), apply(rstd::move(callback)) {}
    template<typename F>
    Actuator(FieldScript* value, F&& callback)
        : script(value), apply(ScriptApply::make(rstd::forward<F>(callback))) {}
    FieldScript* script { nullptr };
    ScriptApply  apply;
};

// Build the closure that drives a SceneNode transform field. Encapsulates
// the Vec3/Vec2/Scalar/Bool coercion table so callers stay one-liners.
// Captures `node` as Arc so actuator lifetime follows the SceneNode allocation.
ScriptApply MakeNodeTransformApply(Arc<owe::SceneNode> node, NodeTransformTarget target);

// Build the closure that drives a SceneNode alpha field.
ScriptApply MakeNodeAlphaApply(Arc<owe::SceneNode> node);

// Build the closure that drives a SceneNode volume field.
ScriptApply MakeNodeVolumeApply(Arc<owe::SceneNode> node);

// Build the closure that drives a SceneNode color field.
ScriptApply MakeNodeColorApply(Arc<owe::SceneNode> node);

// Owns one JsRuntime + the actuator list for one Scene. Constructed and
// populated by the parser, then installed as a Scene extension.
class ScriptScene : NoCopy, NoMove {
public:
    explicit ScriptScene(Option<Arc<AudioResponseDemand>> demand = rstd::None());
    ~ScriptScene();

    JsRuntime& runtime() noexcept;
    void       AddActuator(Actuator a);
    bool       empty() const noexcept;

    // Push the host's per-frame state, drive every FieldScript, drain
    // results into actuators. Call once per frame, before the renderer
    // begins drawing.
    void Tick(const FrameInputs& fi, slice<owe::SceneAnimationEventDispatch> animation_events = {});

    struct Impl;
    Box<Impl> m_impl;
};

// Attach a ScriptScene to a Scene. Takes ownership and replaces any previous attachment.
void InstallScriptScene(owe::Scene& scene, Box<ScriptScene> script_scene);

// Convenience tick: looks up the ScriptScene attached to `scene` and
// drives one frame. No-op when no ScriptScene is installed (image-only
// pkgs, scenes without script bindings).
void TickSceneScripts(owe::Scene& scene, const FrameInputs& fi);

// Patch `engine.userProperties` on the ScriptScene attached to `scene`.
// No-op when the scene has no script runtime.
void SetSceneUserProperty(owe::Scene& scene, ref<str> key, const Json& property);

void SetSceneMediaStatus(owe::Scene& scene, const MediaStatus& status);

// Forward `SetPersistence` to the ScriptScene attached to `scene`. No-op
// when the scene has no script runtime.
void SetScenePersistence(owe::Scene& scene, PathBuf path);

} // namespace owe::script
