export module wescene.pkg.scene_obj:field_binding;
import rstd;
import rstd.cppstd;
import wescene.json;

using namespace rstd::prelude;

// Property-binding side channel.
//
// In Wallpaper Engine any scalar field on a scene object can take three
// shapes:
//
//    1. Plain literal       → `42`, `"1 2 3"`, `true`
//    2. Property-bound       → `{"value": X, "user": "<binding-name>"}`
//                             (auto-unwrapped by wescene.json GetJsonValue)
//    3. Animated / scripted  → `{"value": X, "animation": {...}}` or
//                             `{"value": X, "scriptproperties": {...}}`
//
// The renderer currently consumes only (1)/(2). The animation curve and
// scriptproperties subtrees are absorbed verbatim so the parsed data
// stays schema-complete; SceneSchema tests assert every observed leaf
// path under `*.animation.*` is captured.
//
// One curve covers a vec3 field (c0/c1/c2 axes) or a scalar (c0 only).

export namespace owe::wpscene
{

struct AnimKeyframeTangent {
    bool  enabled { false };
    float x { 0.0f };
    float y { 0.0f };
    // `magic` is a sometimes-present opaque editor value (unsigned int);
    // captured to keep the schema check honest.
    i32 magic { 0 };
};

struct AnimKeyframe {
    i32                 frame { 0 };
    float               value { 0.0f };
    bool                lockangle { false };
    bool                locklength { false };
    AnimKeyframeTangent front;
    AnimKeyframeTangent back;
};

// One timeline marker. The editor drops these on an animation and the
// wallpaper reacts to them from the layer's `animationEvent(event, value)`
// export — that is the only way a scene script learns where a timeline
// currently is.
struct AnimEvent {
    i32         frame { 0 };
    std::string name;
};

struct AnimOptions {
    float       fps { 30.0f };
    i32         length { 0 };
    std::string mode;
    std::string name;
    bool        startpaused { false };
    bool        wraploop { false };
    // `smoothing` may be null/int/float in the corpus; kept as raw json
    // until a renderer consumer needs it.
    owe::Json smoothing;
    owe::Json parent; // object describing parent anim
    // Sibling fields driven by the same timeline (`[{"key": "origin"}]`).
    // Their scripts see this animation's markers too.
    std::vector<std::string> children;
    std::vector<AnimEvent>   events;
};

struct AnimCurve : rstd::DefaultInClass<AnimCurve, rstd::clone::Clone> {
    std::vector<AnimKeyframe> c0;
    std::vector<AnimKeyframe> c1; // empty for scalar fields
    std::vector<AnimKeyframe> c2;
    AnimOptions               options;
    bool                      relative { false }; // only on `origin`

    auto clone() const -> AnimCurve;
};

// FromJson helpers (defined in FieldBinding.cpp).
bool ParseAnimKeyframeTangent(const owe::Json&, AnimKeyframeTangent&);
bool ParseAnimKeyframe(const owe::Json&, AnimKeyframe&);
bool ParseAnimAxis(const owe::Json&, std::vector<AnimKeyframe>&);
bool ParseAnimEvent(const owe::Json&, AnimEvent&);
bool ParseAnimOptions(const owe::Json&, AnimOptions&);
bool ParseAnimCurve(const owe::Json&, AnimCurve&);

// One captured `{value, script, scriptproperties, user}` per-field
// binding. `source` is the inline JS module text observed in scene.json's
// `"script"` key (5286 bindings, 2877 unique sources in the workshop
// corpus — see `tests/wpscriptdump`). `properties` mirrors the per-binding
// `scriptproperties` config block; `initial_value` is the binding's
// `value` field, fed to `init(value)` by the runtime. `user` carries the
// optional user-property name from `{user, value}` companion bindings.
struct ScriptBinding {
    std::string source;
    owe::Json   properties;
    owe::Json   initial_value;
    std::string user;

    auto clone() const -> ScriptBinding;
};

// Side-channel container attached to every parseable object kind. Only
// fields that actually carry a binding contribute entries — empty maps
// for the common case where every field is a plain literal.
struct FieldBindings {
    std::unordered_map<std::string, AnimCurve>     animations;
    rstd::json::Map                                scriptproperties;
    std::unordered_map<std::string, ScriptBinding> scripts;

    auto clone() const -> FieldBindings;
    void Update(const FieldBindings& other);
};

std::size_t AbsorbFieldBinding(std::string_view field, const owe::Json& value, FieldBindings& out);

// Walks every direct child of `obj_json` and, when the child is an
// object containing `animation` and/or `scriptproperties`, captures into
// `out`. Idempotent: re-running on the same json overwrites prior
// entries. Returns the count of bindings absorbed.
std::size_t AbsorbAllFieldBindings(const owe::Json& obj_json, FieldBindings& out);

} // namespace owe::wpscene
