export module wescene.pkg.scene_obj:field_binding;
import rstd;
import rstd.cppstd;
import wescene.json;

using namespace rstd::prelude;

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
    bool                step { false };
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

struct AnimOptions : rstd::DefaultInClass<AnimOptions, rstd::clone::Clone> {
    float       fps { 30.0f };
    i32         length { 0 };
    std::string mode;
    std::string name;
    bool        startpaused { false };
    bool        wraploop { false };
    // `smoothing` may be null/int/float in the corpus; kept as raw json
    // until a renderer consumer needs it.
    owe::Json              smoothing;
    Option<String>         parent;
    Vec<String>            children;
    std::vector<AnimEvent> events;

    auto clone() const -> AnimOptions;
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

struct ScriptBinding {
    std::string source;
    owe::Json   initial_value;

    auto clone() const -> ScriptBinding;
};

struct FieldBindingSpec {
    u64                   identity {};
    String                field;
    Option<AnimCurve>     animation;
    Option<owe::Json>     script_properties;
    Option<ScriptBinding> script;
    Option<String>        user;

    auto clone() const -> FieldBindingSpec;
    auto ScriptProperties() const noexcept -> const owe::Json&;
};

struct FieldBindings {
    auto Entries() const noexcept -> slice<FieldBindingSpec> { return entries.as_slice(); }
    auto Get(ref<str> field) const noexcept -> Option<ref<FieldBindingSpec>>;
    auto GetMut(ref<str> field) noexcept -> Option<mut_ref<FieldBindingSpec>>;
    auto Ensure(ref<str> field) -> mut_ref<FieldBindingSpec>;

    bool IsEmpty() const noexcept { return entries.is_empty(); }
    bool HasAnimation(ref<str> field) const noexcept;
    bool HasScript(ref<str> field) const noexcept;

    auto clone() const -> FieldBindings;
    void Update(const FieldBindings& other);

private:
    Vec<FieldBindingSpec> entries;
};

std::size_t AbsorbFieldBinding(std::string_view field, const owe::Json& value, FieldBindings& out);

// Walks every direct child of `obj_json` and, when the child is an
// object containing `animation` and/or `scriptproperties`, captures into
// `out`. Idempotent: re-running on the same json overwrites prior
// entries. Returns the count of bindings absorbed.
std::size_t AbsorbAllFieldBindings(const owe::Json& obj_json, FieldBindings& out);

} // namespace owe::wpscene
