export module wescene.pkg.scene_obj:misc_object;
import rstd;
import wescene.fs;
import wescene.json;
import :animation_layer;
export import :field_binding;
import :visibility_binding;
import :image_object;
import :scene_document;

using namespace rstd::literals;
using namespace rstd::prelude;

// Object kinds beyond image/light/particle/sound: text overlays, .mdl
// model attachments, and editor camera markers. These exist only at the
// scene.json schema level; the renderer does not yet consume them, but the
// parser absorbs every observed top-level field so the data model stays
// schema-complete (drives SceneSchema.EveryParsedObjectKeyIsObserved).

export namespace owe::wpscene
{

// Text-overlay object (PKGV0005+). Discriminator: top-level `text` is
// non-null. The `text` and `font` fields appear in two shapes — plain
// string, or an object (e.g. `{"script": "..."}` for property-bound
// text). Both are captured verbatim as owe::Json so future consumers
// can decode either path without re-parsing.
struct TextObject {
    // Common positional/metadata (mirrors ImageObject prefix).
    i32                  id { 0 };
    String               name;
    array<float, 3>      origin { 0.0f, 0.0f, 0.0f };
    array<float, 3>      scale { 1.0f, 1.0f, 1.0f };
    array<float, 3>      angles { 0.0f, 0.0f, 0.0f };
    ParallaxDepthBinding parallax;
    bool                 visible { true };

    bool          locktransforms { false };
    bool          muteineditor { false };
    bool          nointerpolation { false };
    u32           parent { 0 };
    String        attachment;
    Vec<i32>      dependencies;
    owe::Json     instance;
    FieldBindings field_bindings;

    // Text-specific.
    owe::Json        text; // string | {script: ...} | {user: ..., value: ...}
    UserValueBinding text_user;
    owe::Json        font; // string | {value: ...}
    float            pointsize { 12.0f };
    u32              padding { 0 };
    String           horizontalalign;
    String           verticalalign;
    String           anchor;
    String           alignment { "center"_Str };

    // Text-flow controls (PKGV0018+).
    u32   maxrows { 0 };
    float maxwidth { 0.0f };
    bool  limitrows { false };
    bool  limitwidth { false };
    bool  limituseellipsis { false };

    VisibleUserBinding visible_user;
    String             visible_user_key;

    // Visual/material overlap with image kind.
    array<float, 3>  color { 1.0f, 1.0f, 1.0f };
    float            alpha { 1.0f };
    float            brightness { 1.0f };
    i32              colorBlendMode { 0 };
    array<float, 2>  size { 0.0f, 0.0f };
    bool             perspective { false };
    bool             reflected { true };
    bool             copybackground { false };
    bool             solid { false };
    bool             opaquebackground { false };
    bool             ledsource { false };
    array<float, 3>  backgroundcolor { 0.0f, 0.0f, 0.0f };
    float            backgroundbrightness { 1.0f };
    Vec<ImageEffect> effects;

    bool FromJson(const owe::Json& json, fs::VFS& vfs) {
        return FromJson(json, vfs, kSceneVersionUnknown);
    }
    bool FromJson(const owe::Json& json, fs::VFS& vfs, SceneVersion /*v*/) {
        owe::GetJsonValue(json, "id"_str, id, false);
        owe::GetJsonValue(json, "name"_str, name, false);
        owe::GetJsonValue(json, "origin"_str, origin, false);
        owe::GetJsonValue(json, "scale"_str, scale, false);
        owe::GetJsonValue(json, "angles"_str, angles, false);
        ReadParallaxDepth(json, parallax);
        ReadVisibleProperty(json, visible, visible_user);
        visible_user_key = visible_user.name.clone();
        owe::GetJsonValue(json, "locktransforms"_str, locktransforms, false);
        owe::GetJsonValue(json, "muteineditor"_str, muteineditor, false);
        owe::GetJsonValue(json, "nointerpolation"_str, nointerpolation, false);
        owe::GetJsonValue(json, "parent"_str, parent, false);
        owe::GetJsonValue(json, "attachment"_str, attachment, false);
        owe::GetJsonValue(json, "dependencies"_str, dependencies, false);
        if (auto value = json.get("instance"_str); value.is_some()) instance = (*value)->clone();

        if (auto value = json.get("text"_str); value.is_some()) text = (*value)->clone();
        ReadUserValueBinding(json, "text"_str, text_user);
        if (auto value = json.get("font"_str); value.is_some()) font = (*value)->clone();

        owe::GetJsonValue(json, "pointsize"_str, pointsize, false);
        owe::GetJsonValue(json, "padding"_str, padding, false);
        owe::GetJsonValue(json, "horizontalalign"_str, horizontalalign, false);
        owe::GetJsonValue(json, "verticalalign"_str, verticalalign, false);
        owe::GetJsonValue(json, "anchor"_str, anchor, false);
        owe::GetJsonValue(json, "alignment"_str, alignment, false);
        owe::GetJsonValue(json, "maxrows"_str, maxrows, false);
        owe::GetJsonValue(json, "maxwidth"_str, maxwidth, false);
        owe::GetJsonValue(json, "limitrows"_str, limitrows, false);
        owe::GetJsonValue(json, "limitwidth"_str, limitwidth, false);
        owe::GetJsonValue(json, "limituseellipsis"_str, limituseellipsis, false);
        owe::GetJsonValue(json, "color"_str, color, false);
        owe::GetJsonValue(json, "alpha"_str, alpha, false);
        owe::GetJsonValue(json, "brightness"_str, brightness, false);
        owe::GetJsonValue(json, "colorBlendMode"_str, colorBlendMode, false);
        owe::GetJsonValue(json, "size"_str, size, false);
        owe::GetJsonValue(json, "perspective"_str, perspective, false);
        owe::GetJsonValue(json, "reflected"_str, reflected, false);
        owe::GetJsonValue(json, "copybackground"_str, copybackground, false);
        owe::GetJsonValue(json, "solid"_str, solid, false);
        owe::GetJsonValue(json, "opaquebackground"_str, opaquebackground, false);
        owe::GetJsonValue(json, "ledsource"_str, ledsource, false);
        owe::GetJsonValue(json, "backgroundcolor"_str, backgroundcolor, false);
        owe::GetJsonValue(json, "backgroundbrightness"_str, backgroundbrightness, false);
        if (auto effect_values = json.get("effects"_str); effect_values.is_some()) {
            auto array = (*effect_values)->as_array();
            if (array.is_some()) {
                for (const auto& jE : **array) {
                    ImageEffect wpeff;
                    wpeff.FromJson(jE, vfs);
                    effects.push(rstd::move(wpeff));
                }
            }
        }
        AbsorbAllFieldBindings(json, field_bindings);
        return true;
    }
};

// 3D model attachment (PKGV0001+). Discriminator: top-level `model` is a
// non-null string. WE links to a `.mdl` file under /assets and optionally
// names a sub-attachment to overlay.
struct ModelObject {
    i32                  id { 0 };
    String               name;
    array<float, 3>      origin { 0.0f, 0.0f, 0.0f };
    array<float, 3>      scale { 1.0f, 1.0f, 1.0f };
    array<float, 3>      angles { 0.0f, 0.0f, 0.0f };
    ParallaxDepthBinding parallax;
    bool                 visible { true };

    bool          locktransforms { false };
    bool          muteineditor { false };
    bool          nointerpolation { false };
    u32           parent { 0 };
    Vec<i32>      dependencies;
    owe::Json     instance;
    FieldBindings field_bindings;

    String model;
    String attachment;
    u32    skin { 0 };
    bool   perspective { false };
    bool   reflected { true };
    bool   castshadow { true };

    Vec<PuppetAnimationLayer> puppet_layers;
    VisibleUserBinding        visible_user;
    String                    visible_user_key;

    bool FromJson(const owe::Json& json, fs::VFS& vfs) {
        return FromJson(json, vfs, kSceneVersionUnknown);
    }
    bool FromJson(const owe::Json& json, fs::VFS&, SceneVersion /*v*/) {
        owe::GetJsonValue(json, "id"_str, id, false);
        owe::GetJsonValue(json, "name"_str, name, false);
        owe::GetJsonValue(json, "origin"_str, origin, false);
        owe::GetJsonValue(json, "scale"_str, scale, false);
        owe::GetJsonValue(json, "angles"_str, angles, false);
        ReadParallaxDepth(json, parallax);
        ReadVisibleProperty(json, visible, visible_user);
        visible_user_key = visible_user.name.clone();
        owe::GetJsonValue(json, "locktransforms"_str, locktransforms, false);
        owe::GetJsonValue(json, "muteineditor"_str, muteineditor, false);
        owe::GetJsonValue(json, "nointerpolation"_str, nointerpolation, false);
        owe::GetJsonValue(json, "parent"_str, parent, false);
        owe::GetJsonValue(json, "dependencies"_str, dependencies, false);
        if (auto value = json.get("instance"_str); value.is_some()) instance = (*value)->clone();

        owe::GetJsonValue(json, "model"_str, model, false);
        owe::GetJsonValue(json, "attachment"_str, attachment, false);
        owe::GetJsonValue(json, "skin"_str, skin, false);
        owe::GetJsonValue(json, "perspective"_str, perspective, false);
        owe::GetJsonValue(json, "reflected"_str, reflected, false);
        owe::GetJsonValue(json, "castshadow"_str, castshadow, false);
        ReadPuppetAnimationLayers(json, puppet_layers);
        AbsorbAllFieldBindings(json, field_bindings);
        return true;
    }
};

// Editor camera marker (PKGV0020+). Discriminator: top-level `camera` is
// a non-null string. Carries camera animation paths and per-camera
// projection overrides.
struct CameraObject {
    i32                  id { 0 };
    String               name;
    array<float, 3>      origin { 0.0f, 0.0f, 0.0f };
    array<float, 3>      scale { 1.0f, 1.0f, 1.0f };
    array<float, 3>      angles { 0.0f, 0.0f, 0.0f };
    ParallaxDepthBinding parallax;
    bool                 visible { true };

    bool          locktransforms { false };
    bool          muteineditor { false };
    bool          nointerpolation { false };
    u32           parent { 0 };
    Vec<i32>      dependencies;
    owe::Json     instance;
    FieldBindings field_bindings;

    String camera; // camera name reference
    String path;   // animation path .json
    String queuemode;
    float  fov { 50.0f };
    float  zoom { 1.0f };
    bool   solid { false };
    bool   disablepropagation { false };

    VisibleUserBinding visible_user;
    String             visible_user_key;

    bool FromJson(const owe::Json& json, fs::VFS& vfs) {
        return FromJson(json, vfs, kSceneVersionUnknown);
    }
    bool FromJson(const owe::Json& json, fs::VFS&, SceneVersion /*v*/) {
        owe::GetJsonValue(json, "id"_str, id, false);
        owe::GetJsonValue(json, "name"_str, name, false);
        owe::GetJsonValue(json, "origin"_str, origin, false);
        owe::GetJsonValue(json, "scale"_str, scale, false);
        owe::GetJsonValue(json, "angles"_str, angles, false);
        ReadParallaxDepth(json, parallax);
        ReadVisibleProperty(json, visible, visible_user);
        visible_user_key = visible_user.name.clone();
        owe::GetJsonValue(json, "locktransforms"_str, locktransforms, false);
        owe::GetJsonValue(json, "muteineditor"_str, muteineditor, false);
        owe::GetJsonValue(json, "nointerpolation"_str, nointerpolation, false);
        owe::GetJsonValue(json, "parent"_str, parent, false);
        owe::GetJsonValue(json, "dependencies"_str, dependencies, false);
        if (auto value = json.get("instance"_str); value.is_some()) instance = (*value)->clone();

        owe::GetJsonValue(json, "camera"_str, camera, false);
        owe::GetJsonValue(json, "path"_str, path, false);
        owe::GetJsonValue(json, "queuemode"_str, queuemode, false);
        owe::GetJsonValue(json, "fov"_str, fov, false);
        owe::GetJsonValue(json, "zoom"_str, zoom, false);
        owe::GetJsonValue(json, "solid"_str, solid, false);
        owe::GetJsonValue(json, "disablepropagation"_str, disablepropagation, false);
        AbsorbAllFieldBindings(json, field_bindings);
        return true;
    }
};

struct CameraPathClip {
    i32               id { 0 };
    String            name;
    bool              visible { true };
    AnimOptions       options;
    Option<AnimCurve> eye;
    Option<AnimCurve> center;
    Option<AnimCurve> up;
    Option<AnimCurve> fov;
    Option<AnimCurve> zoom;

    bool FromJson(const owe::Json& json) {
        if (! json.is_object()) return false;
        owe::GetJsonValue(json, "id"_str, id, false);
        name.clear();
        owe::GetJsonValue(json, "name"_str, name, false);
        owe::GetJsonValue(json, "visible"_str, visible, false);
        if (auto value = json.get("options"_str); value.is_some())
            ParseAnimOptions(**value, options);

        auto parse_curve = [&](ref<str> key) -> Option<AnimCurve> {
            auto value = json.get(key);
            if (value.is_none() || (*value)->is_null()) return None();
            AnimCurve curve;
            bool      parsed = (*value)->is_array() ? ParseAnimAxis(**value, curve.c0)
                                                    : ParseAnimCurve(**value, curve);
            if (! parsed) return None();
            curve.options = options.clone();
            return Some(rstd::move(curve));
        };
        eye    = parse_curve("eye"_str);
        center = parse_curve("center"_str);
        up     = parse_curve("up"_str);
        fov    = parse_curve("fov"_str);
        zoom   = parse_curve("zoom"_str);
        return true;
    }
};

struct CameraPathDocument {
    Vec<CameraPathClip> paths;

    bool FromJson(const owe::Json& json) {
        auto value = json.get("paths"_str);
        if (value.is_none()) return false;
        auto values = (*value)->as_array();
        if (values.is_none()) return false;
        for (const auto& entry : **values) {
            CameraPathClip path;
            if (path.FromJson(entry)) paths.push(rstd::move(path));
        }
        return true;
    }
};

} // namespace owe::wpscene
