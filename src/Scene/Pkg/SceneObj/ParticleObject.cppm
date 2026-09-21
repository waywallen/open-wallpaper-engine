export module wescene.pkg.scene_obj:particle_object;
import rstd;
import wescene.core;
import wescene.utils;
import wescene.fs;

export import :field_binding;
import :visibility_binding;
export import :material;
import :scene_document;

using namespace rstd::prelude;
using rstd::collections::HashMap;
using rstd::sync::Arc;
using namespace rstd::literals;

export namespace owe

{
namespace wpscene
{

class ParticleControlpoint {
public:
    enum class FlagEnum
    {
        link_mouse = 0, // 1
        // this control point will follow the mouse cursor.
        worldspace = 1, // 2
        // the control point will always be at the same position in the world, independent from the
        // position of the particle system.
    };
    using EFlags = BitFlags<FlagEnum>;

    bool            FromJson(const owe::Json&);
    EFlags          flags { 0 };
    i32             id { -1 };
    array<float, 3> offset { 0.0f, 0.0f, 0.0f };
    // a static offset relative to the position of the particle system.
};

class ParticleRender : public rstd::DefaultInClass<ParticleRender, rstd::clone::Clone> {
public:
    auto   clone() const -> ParticleRender;
    bool   FromJson(const owe::Json&);
    String name;
    float  length { 0.05f };
    float  maxlength { 10.0f };
    float  subdivision { 3.0f };
    i32    segments { 4 };
};

class Initializer {
public:
    bool            FromJson(const owe::Json&);
    array<float, 3> max { 0.0f, 0.0f, 0.0f };
    array<float, 3> min { 0.0f, 0.0f, 0.0f };
    String          name;
};

class Emitter : public rstd::DefaultInClass<Emitter, rstd::clone::Clone> {
public:
    auto clone() const -> Emitter;
    enum class FlagEnum : rstd::uint32_t
    {
        one_per_frame = 1,
    };
    using EFlags = BitFlags<FlagEnum>;

public:
    bool            FromJson(const owe::Json&);
    array<float, 3> directions { 1.0f, 1.0f, 0.0f };
    array<float, 3> distancemax { 256.0f, 256.0f, 256.0f };
    array<float, 3> distancemin { 0.0f, 0.0f, 0.0f };
    array<float, 3> origin { 0.0f, 0.0f, 0.0f };
    array<i32, 3>   sign {};
    u32             instantaneous { 0 };
    u32             max_emit_per_period { 0 };
    float           speedmin { 0 };
    float           speedmax { 0 };
    u32             audioprocessingmode { 0 };
    float           audioamount { 1.0f };
    float           audioexponent { 1.0f };
    array<float, 2> audiofrequency { 0.0f, 15.0f };
    array<float, 2> audiobounds { 0.0f, 1.0f };
    i32             controlpoint { 0 };
    i32             id;
    EFlags          flags;
    String          name;
    float           rate { 5.0f };
    float           duration { 0.0f };
};

class ParticleChild;
class Particle {
public:
    enum class FlagEnum
    {
        wordspace                 = 0, // 1
        spritenoframeblending     = 1, // 2
        perspective               = 2, // 4
        disable_color_override    = 3, // 8
        disable_count_override    = 4, // 16
        disable_lifetime_override = 5, // 32
        disable_size_override     = 6, // 64
        disable_speed_override    = 7, // 128
    };
    using EFlags = BitFlags<FlagEnum>;

public:
    bool     FromJson(const owe::Json&, fs::VFS&);
    Particle Clone() const;

    Vec<Emitter>              emitters;
    rstd::json::Array         initializers;
    rstd::json::Array         operators;
    Vec<ParticleRender>       renderers;
    Vec<ParticleControlpoint> controlpoints;

    Material material;

    Vec<ParticleChild> children;

    String animationmode;
    float  sequencemultiplier { 1.0f };
    u32    maxcount { 1 };
    float  starttime { 0.0f };
    EFlags flags { 0 };
};
class ParticleChild {
public:
    enum class FlagEnum
    {
        eventfollow = 1, // 2
    };
    using EFlags = BitFlags<FlagEnum>;

    bool          FromJson(const owe::Json&, fs::VFS&);
    ParticleChild Clone() const;

    // static
    // eventfollow
    // eventspawn
    // eventdeath
    String type { "static"_Str };
    String name;
    i32    maxcount { 20 };
    EFlags flags { 0 };

    Option<i32> controlpointstartindex;
    float       probability { 1.0f };

    array<float, 3> angles { 0.0f, 0.0f, 0.0f };
    array<float, 3> origin { 0.0f, 0.0f, 0.0f };
    array<float, 3> scale { 1.0f, 1.0f, 1.0f };

    Particle obj;
};

class ParticleInstanceoverride {
public:
    bool FromJosn(const owe::Json&);
    auto clone() const -> ParticleInstanceoverride;
    bool enabled { false };
    bool overColor { false };
    bool overColorn { false };

    float           alpha { 1.0f };
    float           count { 1.0f };
    float           lifetime { 1.0f };
    float           rate { 1.0f };
    float           speed { 1.0f };
    float           size { 1.0f };
    float           brightness { 1.0f };
    i32             id { 0 };
    array<float, 3> color { 1.0f, 1.0f, 1.0f };
    array<float, 3> colorn { 1.0f, 1.0f, 1.0f };

    // Presence distinguishes an absent override from an explicit world-space origin.
    array<Option<array<float, 3>>, 8> controlpoint;
    array<array<float, 3>, 8>         controlpointangle {};
    auto                              FieldBindingsView() const noexcept -> const FieldBindings* {
        return m_field_bindings.is_some() ? (*m_field_bindings).as_ptr().as_raw_ptr() : nullptr;
    }

    // field name (e.g. "alpha", "size", "color", "colorn", "lifetime",
    // "rate", "speed", "count", "brightness") -> user-property key when the
    // scene.json value is wrapped in `{"user":"<key>","value":...}`. The
    // owning particle subsystem keeps the override behind an Arc so
    // RenderSetUserProperty can mutate the relevant field at runtime and the
    // change is picked up by every initializer/operator captured closure.
    HashMap<String, String> bindings;

private:
    Option<Arc<FieldBindings>> m_field_bindings;
};

class ParticleObject {
public:
    bool                     FromJson(const owe::Json&, fs::VFS&);               // legacy
    bool                     FromJson(const owe::Json&, fs::VFS&, SceneVersion); // canonical
    bool                     FromAsset(ref<str>, fs::VFS&);
    ParticleObject           Clone() const;
    i32                      id { 0 };
    String                   name;
    array<float, 3>          origin { 0.0f, 0.0f, 0.0f };
    array<float, 3>          scale { 1.0f, 1.0f, 1.0f };
    array<float, 3>          angles { 0.0f, 0.0f, 0.0f };
    ParallaxDepthBinding     parallax;
    bool                     visible { true };
    String                   particle;
    Particle                 particleObj;
    ParticleInstanceoverride instanceoverride;

    // Common cross-kind metadata.
    bool            locktransforms { false };
    bool            muteineditor { false };
    bool            nointerpolation { false };
    bool            reflected { true };
    u32             parent { 0 };
    String          attachment;
    Vec<i32>        dependencies;
    owe::Json       instance;
    owe::Json       particlesrc;                       // PKGV0001+; always null in corpus
    array<float, 3> controlpoint { 0.0f, 0.0f, 0.0f }; // PKGV0019+
    FieldBindings   field_bindings;

    VisibleUserBinding visible_user;
    String             visible_user_key;
};

} // namespace wpscene
} // namespace owe
