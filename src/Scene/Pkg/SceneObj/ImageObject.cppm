export module wescene.pkg.scene_obj:image_object;
import wescene.core;
import rstd;
import wescene.fs;

export import :animation_layer;
export import :field_binding;
import :visibility_binding;
export import :material;
import :scene_document;

using rstd::collections::HashMap;

using namespace rstd::prelude;
using namespace rstd::literals;

export namespace owe

{

namespace wpscene
{

float NormalizeLayerAlpha(float alpha);

class EffectCommand {
public:
    bool   FromJson(const owe::Json&);
    String command;
    String target;
    String source;

    i32 afterpos { 0 }; // 0 for begin, start from 1
};

class EffectFbo {
public:
    bool   FromJson(const owe::Json&);
    String name;
    String format;
    u32    scale { 1 };
    u32    fit { 0 };
    bool   unique { false };
};

// objects[].instance — PKGV0018+. Embedded WE-format material binding
// (compiled-shader id + textures + combos).
class ObjectInstance {
public:
    bool                 FromJson(const owe::Json&);
    void                 ApplyTo(Material&) const;
    bool                 present { false };
    u32                  id { 0 };
    HashMap<String, i32> combos;
    Vec<String>          textures;
    // usertextures elements are polymorphic: bare property-name strings
    // (PKGV0022+) and `{name, type}` system bindings (PKGV0018+). Stored
    // as raw json so both shapes are preserved.
    rstd::json::Array usertextures;
};

class ImageEffect {
public:
    bool               FromJson(const owe::Json&, fs::VFS& vfs);               // legacy
    bool               FromJson(const owe::Json&, fs::VFS& vfs, SceneVersion); // canonical
    bool               FromFileJson(const owe::Json&, fs::VFS& vfs);
    i32                id;
    String             name;
    String             username; // PKGV0001+; per-instance label override
    bool               visible { true };
    VisibleUserBinding visible_user;
    String             visible_user_key;
    i32                version;
    Vec<Material>      materials;
    Vec<MaterialPass>  passes;
    Vec<EffectCommand> commands;
    Vec<EffectFbo>     fbos;
    // `visible` can also be `{"value": false, "script": "..."}`. The script
    // decides every frame whether the effect draws, so the authored value
    // says nothing about whether the effect is ever needed.
    FieldBindings field_bindings;

    const FieldBindingSpec* visible_binding() const {
        auto binding = field_bindings.Get("visible"_str);
        return binding.is_some() ? rstd::addressof(**binding) : nullptr;
    }
};

class ImageObject {
public:
    struct Config {
        bool passthrough { false };
    };
    bool                 FromJson(const owe::Json&, fs::VFS&);               // legacy
    bool                 FromJson(const owe::Json&, fs::VFS&, SceneVersion); // canonical
    bool                 FromAsset(rstd::ref<rstd::str>, array<float, 2>, fs::VFS&, SceneVersion);
    i32                  id { 0 };
    String               name;
    array<float, 3>      origin { 0.0f, 0.0f, 0.0f };
    array<float, 3>      scale { 1.0f, 1.0f, 1.0f };
    array<float, 3>      angles { 0.0f, 0.0f, 0.0f };
    array<float, 2>      size { 2.0f, 2.0f };
    ParallaxDepthBinding parallax;
    array<float, 3>      color { 1.0f, 1.0f, 1.0f };
    i32                  colorBlendMode { 0 };
    float                alpha { 1.0f };
    float                brightness { 1.0f };
    bool                 fullscreen { false };
    bool                 composite_layer { false };
    bool                 nopadding { false };
    bool                 visible { true };
    String               image;
    String               material_path;
    String               alignment { "center"_Str };
    Material             material;
    Vec<ImageEffect>     effects;
    Config               config;

    // Common cross-kind metadata (PKGV0001+ unless noted).
    bool           locktransforms { false };
    bool           muteineditor { false };
    bool           nointerpolation { false }; // PKGV0021+
    u32            parent { 0 };              // PKGV0019+; 0 = no parent
    Vec<i32>       dependencies;              // PKGV0001+; referenced object ids
    ObjectInstance instance;                  // PKGV0018+; instance binding

    // Image-kind specifics (gates listed for reference; reads are unconditional via _NOWARN).
    bool            perspective { false }; // PKGV0002+
    bool            reflected { true };
    bool            copybackground { false }; // PKGV0001+
    bool            solid { false };          // PKGV0002+
    bool            solid_layer { false };
    bool            opaquebackground { false };           // PKGV0005+
    bool            clampuvs { false };                   // PKGV0022+
    bool            castshadow { false };                 // PKGV0019+
    bool            disablepropagation { false };         // PKGV0023+
    String          depthtest { "enabled"_Str };          // PKGV0020+
    array<float, 3> backgroundcolor { 0.0f, 0.0f, 0.0f }; // PKGV0005+
    float           backgroundbrightness { 1.0f };        // PKGV0010+

    String                    puppet;
    Vec<PuppetAnimationLayer> puppet_layers;

    // PKGV0019+ named anchor on the parent's puppet (MDAT attachment). The
    // owning image renders at the parent puppet's bone[attachment.bone_index]
    // offset by attachment.local_xform (see Puppet::Attachment). Empty
    // string means no bone anchoring; the image inherits parent transform
    // directly.
    String attachment;

    // Per-field property-binding side channel; populated when scalar
    // fields (origin/scale/alpha/...) carry an `animation` curve or a
    // `scriptproperties` subtree. See FieldBinding.cppm.
    FieldBindings field_bindings;

    VisibleUserBinding visible_user;
    String             visible_user_key;
    UserValueBinding   color_user;
    String             color_user_key;
    UserValueBinding   alpha_user;
    String             alpha_user_key;
};

class ShapeObject {
public:
    bool FromJson(const owe::Json&, fs::VFS&, SceneVersion);

    i32                  id { 0 };
    String               name;
    String               shape;
    array<float, 3>      origin { 0.0f, 0.0f, 0.0f };
    array<float, 3>      scale { 1.0f, 1.0f, 1.0f };
    array<float, 3>      angles { 0.0f, 0.0f, 0.0f };
    ParallaxDepthBinding parallax;
    bool                 visible { true };
    Vec<ImageEffect>     effects;

    bool          locktransforms { false };
    bool          muteineditor { false };
    bool          nointerpolation { false };
    bool          reflected { true };
    bool          castshadow { false };
    bool          disablepropagation { false };
    u32           parent { 0 };
    String        attachment;
    Vec<i32>      dependencies;
    FieldBindings field_bindings;

    VisibleUserBinding visible_user;
    String             visible_user_key;
};

class ImageAssetInfo {
public:
    Option<array<float, 2>> size;
    String                  first_texture;
    bool                    solid_layer { false };
};

Option<ImageAssetInfo> LoadImageAssetInfo(fs::VFS& vfs, ref<str> image);

} // namespace wpscene
} // namespace owe
