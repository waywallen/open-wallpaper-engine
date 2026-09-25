// Test corpus index.
//
// Walks workshop/* once, dumps every entry that has a scene.pkg via
// DumpWorkshop, and exposes lookup-by-version slices. The corpus is built
// lazily on first access.
//
// Skipped workshops (e.g. ones that hang MdlParser::Parse) are listed
// in kSkipIds and never parsed.

module;

#include <cstdio>

export module wescene.testing.corpus;

import rstd;
import rstd.cppstd;
import wescene.json;
import wescene.pkg.parse;
import wescene.pkg_fs;
import wescene.fs;
import wescene.types;
import wescene.testing.pkg_header;
import wescene.testing.json_builder;

using namespace rstd::prelude;
using namespace rstd::literals;

export namespace owe::testing
{

struct WorkshopEntry {
    std::string id;
    std::string dir;
    owe::Json   snapshot;
};

class Corpus {
public:
    // Returns the singleton, building it on first access.
    static const Corpus& instance();

    // All entries successfully dumped.
    const std::vector<WorkshopEntry>& entries() const { return entries_; }

    // Sorted unique sets of every version stamp observed across the corpus.
    const std::set<std::string>& pkg_versions() const { return pkg_versions_; }
    const std::set<int>&         texv_versions() const { return texv_versions_; }
    const std::set<int>&         texi_versions() const { return texi_versions_; }
    const std::set<int>&         texb_versions() const { return texb_versions_; }
    const std::set<int>&         texs_versions() const { return texs_versions_; }
    const std::set<int>&         tex_formats() const { return tex_formats_; }
    const std::set<int>&         mdlv_versions() const { return mdlv_versions_; }
    const std::set<int>&         mdls_versions() const { return mdls_versions_; }
    const std::set<int>&         mdla_versions() const { return mdla_versions_; }

    // Slice accessors.
    struct PkgRef {
        const WorkshopEntry* workshop;
    };
    struct TexRef {
        const WorkshopEntry* workshop;
        const owe::Json*     tex;
    };
    struct MdlRef {
        const WorkshopEntry* workshop;
        const owe::Json*     mdl;
    };

    std::vector<PkgRef> workshops_with_pkg(const std::string& pkgv) const;
    std::vector<TexRef> textures_with_texv(int v) const;
    std::vector<TexRef> textures_with_texi(int v) const;
    std::vector<TexRef> textures_with_texb(int v) const;
    std::vector<TexRef> textures_with_texs(int v) const;
    std::vector<TexRef> textures_with_format(int v) const;
    std::vector<MdlRef> mdls_with_mdlv(int v) const;
    std::vector<MdlRef> mdls_with_mdls(int v) const;
    std::vector<MdlRef> mdls_with_mdla(int v) const;

private:
    Corpus();
    void build();

    std::vector<WorkshopEntry> entries_;
    std::set<std::string>      pkg_versions_;
    std::set<int>              texv_versions_;
    std::set<int>              texi_versions_;
    std::set<int>              texb_versions_;
    std::set<int>              texs_versions_;
    std::set<int>              tex_formats_;
    std::set<int>              mdlv_versions_;
    std::set<int>              mdls_versions_;
    std::set<int>              mdla_versions_;
};

// Gates which sections DumpWorkshop emits. Defaults preserve the historic
// "do everything except shader compile" behavior so Corpus/version_tests
// fixtures don't shift.
struct DumpFlags {
    bool tex { true };      // emit "textures" array (ReadTexMeta)
    bool shader { false };  // emit "shaders" array (CompileMaterialShader)
    bool mdl { true };      // emit "puppets" array
    bool mdl_full { true }; // puppets entries via full MdlParser::Parse;
                            // false ⇒ just the MdlHeader fields
};

// Per-workshop JSON snapshot used by `wescene-test valid`, by
// `wescene-test scan --json-dir`, and by Corpus to index versions.
// On failure returns a json object with `{"error": "..."}` and `err`
// is set to the same message.
owe::Json DumpWorkshop(const std::string& workshop_dir, std::string& err, DumpFlags flags = {});

} // namespace owe::testing

namespace owe::testing
{

namespace
{

namespace fs = std::filesystem;
using Json   = owe::Json;

rstd::int64_t JsonI64Or(const Json& value, rstd::int64_t fallback) {
    auto parsed = value.as_i64();
    return parsed.is_some() ? parsed->to_primitive() : fallback;
}

template<typename T>
Json SnapshotValue(const T& value) {
    return owe::IntoJson(value);
}

inline Json SnapshotValue(const Json& value) { return value.clone(); }

inline Json SnapshotValue(ref<str> value) {
    return owe::IntoJson(rstd::cppstd::as_string_view(value));
}

inline Json SnapshotValue(const String& value) { return SnapshotValue(value.as_str()); }

template<typename T>
Json SnapshotValue(const std::vector<T>& values) {
    auto out = owe::MakeArray();
    for (const auto& value : values) owe::AppendJson(out, SnapshotValue(value));
    return out;
}

template<typename T, std::size_t N>
Json SnapshotValue(const std::array<T, N>& values) {
    auto out = owe::MakeArray();
    for (const auto& value : values) owe::AppendJson(out, SnapshotValue(value));
    return out;
}

template<typename T>
Json SnapshotValue(const Vec<T>& values) {
    auto out = owe::MakeArray();
    for (const auto& value : values) owe::AppendJson(out, SnapshotValue(value));
    return out;
}

template<typename T, rstd::size_t N>
Json SnapshotValue(const array<T, N>& values) {
    auto out = owe::MakeArray();
    for (const auto& value : values) owe::AppendJson(out, SnapshotValue(value));
    return out;
}

template<typename T>
void SetSnapshot(Json& object, std::string_view key, const T& value) {
    owe::SetJson(object, key, SnapshotValue(value));
}

// Workshops that hang or crash the dumper.
const std::set<std::string> kSkipIds {
    "2435537849",
    "3346715292",
};

// The build can override the corpus roots. These fallbacks keep the tools
// runnable from the source root for ad-hoc development loops.
constexpr const char* kWorkshopDirMacro =
#ifdef WAYWALLEN_WORKSHOP_DIR
    WAYWALLEN_WORKSHOP_DIR
#else
    "workshop"
#endif
    ;

constexpr const char* kAssetsDirMacro =
#ifdef WAYWALLEN_ASSETS_DIR
    WAYWALLEN_ASSETS_DIR
#else
    ""
#endif
    ;

struct TexMeta {
    std::string path;
    int32_t     texv { 0 };
    int32_t     texi { 0 };
    int32_t     texb { 0 };
    int32_t     texs { 0 };
    int32_t     compo1 { 0 };
    int32_t     compo2 { 0 };
    int32_t     compo3 { 0 };
    int32_t     format { 0 };
    int32_t     image_type { 0 };
    int32_t     width { 0 };
    int32_t     height { 0 };
    int32_t     map_width { 0 };
    int32_t     map_height { 0 };
    int32_t     count { 0 };
    bool        is_sprite { false };
    int64_t     sprite_frames { 0 };
    bool        mipmap_pow2 { false };
    bool        mipmap_larger { false };
    int         wrap_s { 0 };
    int         wrap_t { 0 };
    int         min_filter { 0 };
    int         mag_filter { 0 };
    bool        ok { false };
};

TexMeta ReadTexMeta(owe::fs::VFS& vfs, const std::string& pkg_path) {
    TexMeta meta;
    meta.path = pkg_path;

    constexpr std::string_view prefix = "/materials/";
    constexpr std::string_view suffix = ".tex";
    if (pkg_path.compare(0, prefix.size(), prefix) != 0) return meta;
    if (pkg_path.size() < prefix.size() + suffix.size()) return meta;
    if (pkg_path.compare(pkg_path.size() - suffix.size(), suffix.size(), suffix) != 0) return meta;
    std::string name =
        pkg_path.substr(prefix.size(), pkg_path.size() - prefix.size() - suffix.size());

    owe::TexImageParser parser(&vfs);
    owe::ImageHeader    h;
    try {
        auto parsed = parser.ParseHeader(rstd::cppstd::as_str(name).unwrap());
        if (parsed.is_err()) return meta;
        h = rstd::move(parsed).unwrap_unchecked();
    } catch (const std::exception&) {
        return meta;
    }

    auto extra_val = [&](const std::string& k) -> int32_t {
        auto value = h.extraHeader.get(rstd::cppstd::as_str(k).unwrap());
        return value.is_none() ? 0 : (*value)->val;
    };
    meta.texv          = extra_val("texv");
    meta.texi          = extra_val("texi");
    meta.texb          = extra_val("texb");
    meta.texs          = extra_val("texs");
    meta.compo1        = extra_val("compo1");
    meta.compo2        = extra_val("compo2");
    meta.compo3        = extra_val("compo3");
    meta.format        = static_cast<int32_t>(h.format);
    meta.image_type    = static_cast<int32_t>(h.type);
    meta.width         = h.width;
    meta.height        = h.height;
    meta.map_width     = h.mapWidth;
    meta.map_height    = h.mapHeight;
    meta.count         = h.count;
    meta.is_sprite     = h.isSprite;
    meta.sprite_frames = static_cast<int64_t>(h.spriteAnim.numFrames().to_primitive());
    meta.mipmap_pow2   = h.mipmap_pow2;
    meta.mipmap_larger = h.mipmap_larger;
    meta.wrap_s        = static_cast<int>(h.sample.wrapS);
    meta.wrap_t        = static_cast<int>(h.sample.wrapT);
    meta.min_filter    = static_cast<int>(h.sample.minFilter);
    meta.mag_filter    = static_cast<int>(h.sample.magFilter);
    meta.ok            = (meta.texv > 0 && meta.width > 0 && meta.height > 0);
    return meta;
}

bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

void sort_by_path(Json& value) {
    auto array = value.as_array();
    if (array.is_none()) return;
    std::vector<std::size_t> ordered;
    ordered.reserve((*array)->len().to_primitive());
    for (std::size_t index = 0; index < (*array)->len().to_primitive(); ++index)
        ordered.push_back(index);
    std::sort(ordered.begin(), ordered.end(), [&](std::size_t a_index, std::size_t b_index) {
        const auto&      a      = (**array)[usize(a_index)];
        const auto&      b      = (**array)[usize(b_index)];
        auto             a_path = a.get("path"_str);
        auto             b_path = b.get("path"_str);
        std::string_view a_view;
        std::string_view b_view;
        if (a_path.is_some()) {
            auto string = (*a_path)->as_str();
            if (string.is_some()) a_view = rstd::cppstd::as_string_view(*string);
        }
        if (b_path.is_some()) {
            auto string = (*b_path)->as_str();
            if (string.is_some()) b_view = rstd::cppstd::as_string_view(*string);
        }
        return a_view.compare(b_view) < 0;
    });
    auto sorted = owe::MakeArray();
    for (auto index : ordered) owe::AppendJson(sorted, (**array)[usize(index)].clone());
    value = std::move(sorted);
}

template<typename Map>
Json map_to_json(const Map& m) {
    auto o = owe::MakeObject();
    for (auto [k, v] : m.iter()) SetSnapshot(o, rstd::cppstd::as_string_view(k->as_str()), *v);
    return o;
}

Json dump_material(const owe::wpscene::Material& m) {
    auto out = owe::MakeObject();
    SetSnapshot(out, "shader", m.shader);
    SetSnapshot(out, "blending", m.blending);
    SetSnapshot(out, "cullmode", m.cullmode);
    SetSnapshot(out, "depthtest", m.depthtest);
    SetSnapshot(out, "depthwrite", m.depthwrite);
    SetSnapshot(out, "use_puppet", m.use_puppet);
    SetSnapshot(out, "textures", m.textures);
    owe::SetJson(out, "combos", map_to_json(m.combos));
    owe::SetJson(out, "constantshadervalues", map_to_json(m.constantshadervalues));
    return out;
}

Json dump_material_pass(const owe::wpscene::MaterialPass& p) {
    auto bind = owe::MakeArray();
    for (const auto& b : p.bind) {
        auto item = owe::MakeObject();
        SetSnapshot(item, "name", b.name);
        SetSnapshot(item, "index", b.index);
        owe::AppendJson(bind, std::move(item));
    }
    auto out = owe::MakeObject();
    SetSnapshot(out, "target", p.target);
    SetSnapshot(out, "textures", p.textures);
    owe::SetJson(out, "combos", map_to_json(p.combos));
    owe::SetJson(out, "constantshadervalues", map_to_json(p.constantshadervalues));
    owe::SetJson(out, "bind", std::move(bind));
    return out;
}

Json dump_effect_fbo(const owe::wpscene::EffectFbo& f) {
    auto out = owe::MakeObject();
    SetSnapshot(out, "name", f.name);
    SetSnapshot(out, "format", f.format);
    SetSnapshot(out, "scale", f.scale);
    return out;
}

// Field types in scene.json are inconsistent (origin can be either an
// array of floats or a "x y z" string), so we copy the raw value through
// instead of forcing a particular C++ type.
Json dump_object_common(const Json& obj) {
    auto o  = owe::MakeObject();
    auto id = obj.get("id"_str);
    SetSnapshot(o, "id", id.is_some() ? static_cast<int>(JsonI64Or(**id, -1)) : -1);
    auto name = obj.get("name"_str);
    SetSnapshot(o,
                "name",
                name.is_some() && (*name)->as_str().is_some()
                    ? rstd::cppstd::to_string(*(*name)->as_str())
                    : "");
    // `visible` is sometimes a {script, value} object (scripted property);
    // json::value<bool> would throw type_error on that shape and tear down
    // the entire scene dump. Unwrap when present, default to true.
    bool visible = true;
    if (auto value = obj.get("visible"_str); value.is_some()) {
        auto initial = (*value)->get("value"_str);
        visible =
            (initial.is_some() ? (*initial)->as_bool() : (*value)->as_bool()).unwrap_or(visible);
    }
    SetSnapshot(o, "visible", visible);
    constexpr rstd::array<ref<str>, 6> keys {
        "origin"_str, "scale"_str, "angles"_str, "size"_str, "parallaxDepth"_str, "alignment"_str,
    };
    for (auto key : keys) {
        if (auto value = obj.get(key); value.is_some())
            owe::SetJson(o, rstd::cppstd::as_string_view(key), (*value)->clone());
    }
    return o;
}

Json dump_light_object(const Json& obj, owe::fs::VFS& vfs) {
    Json out = dump_object_common(obj);
    SetSnapshot(out, "kind", "light");
    owe::wpscene::LightObject lo;
    bool                      ok = false;
    try {
        ok = lo.FromJson(obj, vfs);
    } catch (const std::exception&) {
        ok = false;
    }
    SetSnapshot(out, "parsed", ok);
    if (! ok) return out;
    SetSnapshot(out, "light", lo.light);
    SetSnapshot(out, "color", lo.color);
    SetSnapshot(out, "intensity", lo.intensity);
    SetSnapshot(out, "radius", lo.radius);
    SetSnapshot(out, "origin_parsed", lo.origin);
    SetSnapshot(out, "scale_parsed", lo.scale);
    SetSnapshot(out, "angles_parsed", lo.angles);
    SetSnapshot(out, "visible_parsed", lo.visible);
    return out;
}

Json dump_particle_object(const Json& obj, owe::fs::VFS& vfs) {
    Json out = dump_object_common(obj);
    SetSnapshot(out, "kind", "particle");
    owe::wpscene::ParticleObject po;
    bool                         ok = false;
    try {
        ok = po.FromJson(obj, vfs);
    } catch (const std::exception&) {
        ok = false;
    }
    SetSnapshot(out, "parsed", ok);
    if (! ok) return out;
    SetSnapshot(out, "particle", po.particle);
    SetSnapshot(out, "origin_parsed", po.origin);
    SetSnapshot(out, "scale_parsed", po.scale);
    SetSnapshot(out, "angles_parsed", po.angles);
    SetSnapshot(out, "visible_parsed", po.visible);
    SetSnapshot(
        out, "emitter_count", static_cast<int>(po.particleObj.emitters.len().to_primitive()));
    SetSnapshot(out,
                "initializer_count",
                static_cast<int>(po.particleObj.initializers.len().to_primitive()));
    SetSnapshot(
        out, "operator_count", static_cast<int>(po.particleObj.operators.len().to_primitive()));
    SetSnapshot(
        out, "renderer_count", static_cast<int>(po.particleObj.renderers.len().to_primitive()));
    SetSnapshot(out,
                "controlpoint_count",
                static_cast<int>(po.particleObj.controlpoints.len().to_primitive()));
    SetSnapshot(out, "child_count", static_cast<int>(po.particleObj.children.len().to_primitive()));
    SetSnapshot(out, "maxcount", po.particleObj.maxcount);
    SetSnapshot(out, "starttime", static_cast<int>(po.particleObj.starttime));
    SetSnapshot(out, "animationmode", po.particleObj.animationmode);
    return out;
}

Json dump_sound_object(const Json& obj, owe::fs::VFS& vfs) {
    Json out = dump_object_common(obj);
    SetSnapshot(out, "kind", "sound");
    owe::wpscene::SoundObject so;
    bool                      ok = false;
    try {
        ok = so.FromJson(obj, vfs);
    } catch (const std::exception&) {
        ok = false;
    }
    SetSnapshot(out, "parsed", ok);
    if (! ok) return out;
    SetSnapshot(out, "playbackmode", so.playbackmode);
    SetSnapshot(out, "volume", so.volume);
    SetSnapshot(out, "mintime", so.mintime);
    SetSnapshot(out, "maxtime", so.maxtime);
    SetSnapshot(out, "visible_parsed", so.visible);
    SetSnapshot(out, "sound_paths", so.sound);
    return out;
}

Json dump_image_object(const Json& obj, owe::fs::VFS& vfs) {
    Json out = dump_object_common(obj);
    SetSnapshot(out, "kind", "image");
    owe::wpscene::ImageObject img;
    bool                      ok = false;
    try {
        ok = img.FromJson(obj, vfs);
    } catch (const std::exception&) {
        ok = false;
    }
    SetSnapshot(out, "parsed", ok);
    if (! ok) return out;
    SetSnapshot(out, "image", img.image);
    SetSnapshot(out, "color", img.color);
    SetSnapshot(out, "colorBlendMode", img.colorBlendMode);
    SetSnapshot(out, "alpha", img.alpha);
    SetSnapshot(out, "brightness", img.brightness);
    SetSnapshot(out, "fullscreen", img.fullscreen);
    SetSnapshot(out, "nopadding", img.nopadding);
    SetSnapshot(out, "origin_parsed", img.origin);
    SetSnapshot(out, "scale_parsed", img.scale);
    SetSnapshot(out, "angles_parsed", img.angles);
    SetSnapshot(out, "size_parsed", img.size);
    SetSnapshot(out, "visible_parsed", img.visible);
    SetSnapshot(out, "alignment_parsed", img.alignment);
    SetSnapshot(out, "puppet", img.puppet);
    owe::SetJson(out, "material", dump_material(img.material));
    SetSnapshot(out, "effect_count", static_cast<int>(img.effects.len().to_primitive()));
    // ImageEffect::id and ::version are left uninitialised by the
    // parser when the source json omits them, so dumping their raw value
    // produces stack garbage. Skip them.
    auto effs = owe::MakeArray();
    for (const auto& e : img.effects) {
        auto je = owe::MakeObject();
        SetSnapshot(je, "name", e.name);
        SetSnapshot(je, "visible", e.visible);
        auto mats = owe::MakeArray();
        for (const auto& mm : e.materials) owe::AppendJson(mats, dump_material(mm));
        owe::SetJson(je, "materials", std::move(mats));
        auto passes = owe::MakeArray();
        for (const auto& p : e.passes) owe::AppendJson(passes, dump_material_pass(p));
        owe::SetJson(je, "passes", std::move(passes));
        auto fbos = owe::MakeArray();
        for (const auto& f : e.fbos) owe::AppendJson(fbos, dump_effect_fbo(f));
        owe::SetJson(je, "fbos", std::move(fbos));
        SetSnapshot(je, "material_count", static_cast<int>(e.materials.len().to_primitive()));
        SetSnapshot(je, "pass_count", static_cast<int>(e.passes.len().to_primitive()));
        SetSnapshot(je, "fbo_count", static_cast<int>(e.fbos.len().to_primitive()));
        owe::AppendJson(effs, std::move(je));
    }
    owe::SetJson(out, "effects", std::move(effs));
    return out;
}

template<typename Predicate>
std::vector<Corpus::TexRef> tex_filter(const std::vector<WorkshopEntry>& es, Predicate pred) {
    std::vector<Corpus::TexRef> out;
    for (const auto& e : es) {
        auto textures = e.snapshot.get("textures"_str);
        if (textures.is_none()) continue;
        auto array = (*textures)->as_array();
        if (array.is_none()) continue;
        for (const auto& t : **array) {
            auto ok = t.get("ok"_str);
            if (ok.is_none() || ! (*ok)->as_bool().unwrap_or(false)) continue;
            if (pred(t)) out.push_back({ &e, &t });
        }
    }
    return out;
}

template<typename Predicate>
std::vector<Corpus::MdlRef> mdl_filter(const std::vector<WorkshopEntry>& es, Predicate pred) {
    std::vector<Corpus::MdlRef> out;
    for (const auto& e : es) {
        auto puppets = e.snapshot.get("puppets"_str);
        if (puppets.is_none()) continue;
        auto array = (*puppets)->as_array();
        if (array.is_none()) continue;
        for (const auto& m : **array) {
            if (pred(m)) out.push_back({ &e, &m });
        }
    }
    return out;
}

} // namespace

Json DumpWorkshop(const std::string& workshop_dir, std::string& err, DumpFlags flags) {
    err.clear();
    auto out = owe::MakeObject();
    SetSnapshot(out, "workshop_dir", fs::path(workshop_dir).filename().string());
    const std::string pkg_id = fs::path(workshop_dir).filename().string();

    const std::string pkg_path = workshop_dir + "/scene.pkg";
    if (! fs::exists(pkg_path)) {
        err = "scene.pkg not found at " + pkg_path;
        SetSnapshot(out, "error", err);
        return out;
    }

    std::string           pkg_version;
    std::vector<PkgEntry> pkg_entries;
    if (! ReadPkgHeader(pkg_path, pkg_version, pkg_entries)) {
        err = "failed to read pkg header";
        SetSnapshot(out, "error", err);
        return out;
    }

    bool has_scene_json = false;
    for (const auto& e : pkg_entries)
        if (e.path == "/scene.json") {
            has_scene_json = true;
            break;
        }

    auto jpkg = owe::MakeObject();
    SetSnapshot(jpkg, "version", pkg_version);
    SetSnapshot(jpkg, "file_count", static_cast<int>(pkg_entries.size()));
    SetSnapshot(jpkg, "has_scene_json", has_scene_json);
    owe::SetJson(out, "pkg", std::move(jpkg));

    owe::fs::VFS vfs;
    auto         afs =
        owe::fs::make_physical_fs(owe::fs::Path(rstd::cppstd::as_str(kAssetsDirMacro).unwrap()));
    if (afs.is_ok()) {
        (void)vfs.mount("/assets"_str, std::move(afs).unwrap_unchecked());
    }
    auto pfs =
        owe::fs::make_physical_fs(owe::fs::Path(rstd::cppstd::as_str(workshop_dir).unwrap()));
    auto wfs = owe::fs::WPPkgFs::open(owe::fs::Path(rstd::cppstd::as_str(pkg_path).unwrap()));
    if (wfs.is_err()) {
        err = "WPPkgFs::open failed";
        SetSnapshot(out, "error", err);
        return out;
    }
    (void)vfs.mount("/assets"_str, wfs->mount_handle());
    if (pfs.is_ok()) {
        (void)vfs.mount("/assets"_str, std::move(pfs).unwrap_unchecked());
    }

    if (has_scene_json) {
        auto stream = owe::fs::OpenBinary(vfs, owe::fs::Path("/assets/scene.json"_str));
        if (stream.is_ok()) {
            auto text        = stream->ReadAllStr();
            auto parsed_json = owe::ParseJson(text.as_str());
            if (parsed_json.is_ok()) {
                auto                        j = parsed_json.unwrap();
                owe::wpscene::SceneMetadata scene;
                bool                        parsed = scene.FromJson(j);
                auto                        jscene = owe::MakeObject();
                SetSnapshot(jscene, "parsed", parsed);
                SetSnapshot(jscene, "is_ortho", scene.general.isOrtho);
                auto ortho = owe::MakeObject();
                SetSnapshot(ortho, "width", scene.general.orthogonalprojection.width);
                SetSnapshot(ortho, "height", scene.general.orthogonalprojection.height);
                owe::SetJson(jscene, "ortho", std::move(ortho));
                auto camera = owe::MakeObject();
                SetSnapshot(camera, "center", scene.camera.center);
                SetSnapshot(camera, "eye", scene.camera.eye);
                SetSnapshot(camera, "up", scene.camera.up);
                owe::SetJson(jscene, "camera", std::move(camera));
                // cameraparallaxamount/delay/mouseinfluence are undefaulted
                // floats in SceneGeneral, so when the source scene.json
                // omits them the parser leaves stack garbage. Only emit them
                // when cameraparallax is enabled.
                auto jgen = owe::MakeObject();
                SetSnapshot(jgen, "clearcolor", scene.general.clearcolor);
                SetSnapshot(jgen, "ambientcolor", scene.general.ambientcolor);
                SetSnapshot(jgen, "skylightcolor", scene.general.skylightcolor);
                SetSnapshot(jgen, "cameraparallax", scene.general.cameraparallax);
                SetSnapshot(jgen, "zoom", scene.general.zoom);
                SetSnapshot(jgen, "fov", scene.general.fov);
                SetSnapshot(jgen, "nearz", scene.general.nearz);
                SetSnapshot(jgen, "farz", scene.general.farz);
                if (scene.general.cameraparallax) {
                    SetSnapshot(jgen, "cameraparallaxamount", scene.general.cameraparallaxamount);
                    SetSnapshot(jgen, "cameraparallaxdelay", scene.general.cameraparallaxdelay);
                    SetSnapshot(jgen,
                                "cameraparallaxmouseinfluence",
                                scene.general.cameraparallaxmouseinfluence);
                }
                owe::SetJson(jscene, "general", std::move(jgen));
                auto jobjects = owe::MakeArray();
                if (auto objects = j.get("objects"_str); objects.is_some()) {
                    auto object_array = (*objects)->as_array();
                    if (object_array.is_some())
                        for (const auto& obj : **object_array) {
                            if (obj.get("image"_str).is_some())
                                owe::AppendJson(jobjects, dump_image_object(obj, vfs));
                            else if (obj.get("light"_str).is_some())
                                owe::AppendJson(jobjects, dump_light_object(obj, vfs));
                            else if (obj.get("particle"_str).is_some())
                                owe::AppendJson(jobjects, dump_particle_object(obj, vfs));
                            else if (obj.get("sound"_str).is_some())
                                owe::AppendJson(jobjects, dump_sound_object(obj, vfs));
                            else {
                                Json o = dump_object_common(obj);
                                SetSnapshot(o, "kind", "unknown");
                                owe::AppendJson(jobjects, std::move(o));
                            }
                        }
                }
                auto                     values = jobjects.as_array();
                std::vector<std::size_t> ordered;
                ordered.reserve((*values)->len().to_primitive());
                for (std::size_t index = 0; index < (*values)->len().to_primitive(); ++index)
                    ordered.push_back(index);
                std::sort(
                    ordered.begin(), ordered.end(), [&](std::size_t a_index, std::size_t b_index) {
                        const auto& a    = (**values)[usize(a_index)];
                        const auto& b    = (**values)[usize(b_index)];
                        auto        a_id = a.get("id"_str);
                        auto        b_id = b.get("id"_str);
                        return (a_id.is_some() ? JsonI64Or(**a_id, -1) : -1) <
                               (b_id.is_some() ? JsonI64Or(**b_id, -1) : -1);
                    });
                auto sorted_objects = owe::MakeArray();
                for (auto index : ordered)
                    owe::AppendJson(sorted_objects, (**values)[usize(index)].clone());
                SetSnapshot(jscene, "object_count", static_cast<int>(ordered.size()));
                owe::SetJson(jscene, "objects", std::move(sorted_objects));
                owe::SetJson(out, "scene", std::move(jscene));
            } else {
                auto error = owe::MakeObject();
                SetSnapshot(error, "parsed", false);
                SetSnapshot(error, "error", "invalid JSON");
                owe::SetJson(out, "scene", std::move(error));
            }
        }
    }

    if (flags.tex) {
        auto jtex = owe::MakeArray();
        for (const auto& e : pkg_entries) {
            if (! ends_with(e.path, ".tex")) continue;
            if (e.path.rfind("/materials/", 0) != 0) continue;
            std::string vfs_path = "/assets" + e.path;
            TexMeta     m        = ReadTexMeta(vfs, e.path);
            auto        jm       = owe::MakeObject();
            SetSnapshot(jm, "path", e.path);
            SetSnapshot(jm, "ok", m.ok);
            SetSnapshot(jm, "texv", m.texv);
            SetSnapshot(jm, "texi", m.texi);
            SetSnapshot(jm, "texb", m.texb);
            SetSnapshot(jm, "texs", m.texs);
            SetSnapshot(jm, "compo1", m.compo1);
            SetSnapshot(jm, "compo2", m.compo2);
            SetSnapshot(jm, "compo3", m.compo3);
            SetSnapshot(jm, "format", m.format);
            SetSnapshot(jm, "image_type", m.image_type);
            SetSnapshot(jm, "width", m.width);
            SetSnapshot(jm, "height", m.height);
            SetSnapshot(jm, "map_width", m.map_width);
            SetSnapshot(jm, "map_height", m.map_height);
            SetSnapshot(jm, "count", m.count);
            SetSnapshot(jm, "is_sprite", m.is_sprite);
            SetSnapshot(jm, "sprite_frames", m.sprite_frames);
            SetSnapshot(jm, "mipmap_pow2", m.mipmap_pow2);
            SetSnapshot(jm, "mipmap_larger", m.mipmap_larger);
            SetSnapshot(jm, "wrap_s", m.wrap_s);
            SetSnapshot(jm, "wrap_t", m.wrap_t);
            SetSnapshot(jm, "min_filter", m.min_filter);
            SetSnapshot(jm, "mag_filter", m.mag_filter);
            owe::AppendJson(jtex, std::move(jm));
        }
        sort_by_path(jtex);
        owe::SetJson(out, "textures", std::move(jtex));
    }

    if (flags.shader) {
        auto jsh = owe::MakeArray();
        for (const auto& e : pkg_entries) {
            if (e.path.rfind("/materials/", 0) != 0) continue;
            if (! ends_with(e.path, ".json")) continue;
            auto jm = owe::MakeObject();
            SetSnapshot(jm, "path", e.path);
            auto stream = owe::fs::OpenBinary(
                vfs, owe::fs::Path(rstd::cppstd::as_str("/assets" + e.path).unwrap()));
            if (stream.is_err()) {
                SetSnapshot(jm, "ok", false);
                SetSnapshot(jm, "error", "cannot open");
                owe::AppendJson(jsh, std::move(jm));
                continue;
            }
            auto text            = stream->ReadAllStr();
            auto parsed_material = owe::ParseJson(text.as_str());
            if (parsed_material.is_err()) {
                SetSnapshot(jm, "ok", false);
                SetSnapshot(jm, "error", "invalid JSON");
                owe::AppendJson(jsh, std::move(jm));
                continue;
            }
            auto                             jmat = parsed_material.unwrap();
            owe::CompileMaterialShaderResult r;
            try {
                r = owe::ShaderParser::CompileMaterialShader(
                    jmat, vfs, rstd::cppstd::as_str(pkg_id).unwrap());
            } catch (const std::exception& ex) {
                r.ok    = false;
                r.error = rstd::into(rstd::cppstd::as_str(ex.what()).unwrap());
            } catch (...) {
                r.ok    = false;
                r.error = "unknown exception"_Str;
            }
            SetSnapshot(jm, "ok", r.ok);
            SetSnapshot(jm, "shader_name", r.shader_name);
            if (! r.ok) SetSnapshot(jm, "error", r.error);
            owe::AppendJson(jsh, std::move(jm));
        }
        sort_by_path(jsh);
        owe::SetJson(out, "shaders", std::move(jsh));
    }

    if (! flags.mdl) return out;

    auto emit_flag = [](uint32_t flag) {
        auto flag_arr = owe::MakeArray();
        for (int byte_idx = 0; byte_idx < 4; ++byte_idx) {
            uint8_t     b = static_cast<uint8_t>((flag >> (byte_idx * 8)) & 0xFFu);
            std::string bits(8, '0');
            for (int i = 0; i < 8; ++i)
                if (b & (1u << (7 - i))) bits[i] = '1';
            owe::AppendElement(flag_arr, std::move(bits));
        }
        return flag_arr;
    };

    auto jmdl = owe::MakeArray();
    if (! flags.mdl_full) {
        for (const auto& e : pkg_entries) {
            if (! ends_with(e.path, ".mdl")) continue;
            std::string rel = e.path;
            if (! rel.empty() && rel.front() == '/') rel.erase(0, 1);
            MdlHeader h;
            bool      ok = false;
            try {
                ok = owe::MdlParser::ParseHeader(rstd::cppstd::as_str(rel).unwrap(), vfs, h);
            } catch (const std::exception&) {
                ok = false;
            }
            auto jm = owe::MakeObject();
            SetSnapshot(jm, "path", e.path);
            SetSnapshot(jm, "ok", ok);
            SetSnapshot(jm, "mdlv", h.mdlv);
            owe::SetJson(jm, "flag", emit_flag(h.mdl_flag));
            SetSnapshot(jm, "skin_count", static_cast<int64_t>(h.skin_count));
            SetSnapshot(jm, "mesh_count", static_cast<int64_t>(h.mesh_count));
            owe::AppendJson(jmdl, std::move(jm));
        }
        sort_by_path(jmdl);
        owe::SetJson(out, "puppets", std::move(jmdl));
        return out;
    }

    for (const auto& e : pkg_entries) {
        if (! ends_with(e.path, ".mdl")) continue;
        // MdlParser::Parse expects a path relative to /assets without the
        // leading slash.
        std::string rel = e.path;
        if (! rel.empty() && rel.front() == '/') rel.erase(0, 1);
        Mdl  mdl;
        bool ok = false;
        try {
            ok = owe::MdlParser::Parse(rstd::cppstd::as_str(rel).unwrap(), vfs, mdl);
        } catch (const std::exception&) {
            ok = false;
        }
        auto jm = owe::MakeObject();
        SetSnapshot(jm, "path", e.path);
        SetSnapshot(jm, "ok", ok);
        SetSnapshot(jm, "mdlv", mdl.header.mdlv);
        owe::SetJson(jm, "flag", emit_flag(mdl.header.mdl_flag));
        SetSnapshot(jm, "skin_count", static_cast<int64_t>(mdl.header.skin_count));
        SetSnapshot(jm, "mesh_count", static_cast<int64_t>(mdl.header.mesh_count));
        SetSnapshot(jm, "mdls", mdl.mdls);
        SetSnapshot(jm, "mdla", mdl.mdla);
        const Mdl::Mesh* m0 = mdl.meshes.is_empty() ? nullptr : &mdl.meshes.first().unwrap().get();
        SetSnapshot(jm,
                    "mat_json_file",
                    m0 && ! m0->mat_json_files.is_empty()
                        ? m0->mat_json_files.first().unwrap()->as_str()
                        : ref<str>());
        SetSnapshot(
            jm, "vertex_count", m0 ? static_cast<int>(m0->positions.len().to_primitive()) : 0);
        SetSnapshot(jm, "index_count", m0 ? static_cast<int>(m0->indices.len().to_primitive()) : 0);
        SetSnapshot(
            jm, "vert_extra_count", m0 ? static_cast<int>(m0->part_uv2.len().to_primitive()) : 0);
        SetSnapshot(jm, "part_count", m0 ? static_cast<int>(m0->parts.len().to_primitive()) : 0);
        auto parts_arr = owe::MakeArray();
        if (m0) {
            for (const auto& pt : m0->parts) {
                auto part = owe::MakeObject();
                SetSnapshot(part, "id", static_cast<int64_t>(pt.id));
                SetSnapshot(part, "start", static_cast<int64_t>(pt.start));
                SetSnapshot(part, "size", static_cast<int64_t>(pt.size));
                owe::AppendJson(parts_arr, std::move(part));
            }
        }
        owe::SetJson(jm, "parts", std::move(parts_arr));
        SetSnapshot(jm,
                    "bones",
                    ok && mdl.puppet.is_some()
                        ? static_cast<int>((*mdl.puppet)->bones.len().to_primitive())
                        : 0);
        SetSnapshot(jm,
                    "anims",
                    ok && mdl.puppet.is_some()
                        ? static_cast<int>((*mdl.puppet)->anims.len().to_primitive())
                        : 0);
        if (ok && mdl.puppet.is_some()) {
            auto bones = owe::MakeArray();
            for (const auto& b : (*mdl.puppet)->bones) {
                auto jb = owe::MakeObject();
                SetSnapshot(jb, "name", b.name);
                SetSnapshot(jb, "bind_parent", static_cast<int64_t>(b.bind_parent));
                SetSnapshot(jb, "anim_parent", static_cast<int64_t>(b.anim_parent));
                SetSnapshot(jb, "has_sim_json", ! b.simulation_json.is_empty());
                SetSnapshot(jb, "has_file_skin_pivot", b.has_file_skin_pivot);
                SetSnapshot(jb,
                            "centroid_offset",
                            std::array { b.vertex_centroid_offset.x(),
                                         b.vertex_centroid_offset.y(),
                                         b.vertex_centroid_offset.z() });
                std::array<double, 4> col_sums { 0, 0, 0, 0 };
                for (int c = 0; c < 4; ++c)
                    for (int r = 0; r < 4; ++r)
                        col_sums[static_cast<std::size_t>(c)] += b.local_bind.matrix()(r, c);
                SetSnapshot(jb, "transform_col_sums", col_sums);
                owe::AppendJson(bones, std::move(jb));
            }
            owe::SetJson(jm, "bone_tree", std::move(bones));
            SetSnapshot(jm,
                        "attachment_count",
                        static_cast<int>((*mdl.puppet)->attachments.len().to_primitive()));
            auto atts = owe::MakeArray();
            for (const auto& a : (*mdl.puppet)->attachments) {
                auto attachment = owe::MakeObject();
                SetSnapshot(attachment, "name", a.name);
                owe::AppendJson(atts, std::move(attachment));
            }
            owe::SetJson(jm, "attachments", std::move(atts));

            auto anims = owe::MakeArray();
            for (const auto& a : (*mdl.puppet)->anims) {
                auto ja = owe::MakeObject();
                SetSnapshot(ja, "id", a.id);
                SetSnapshot(ja, "fps", a.fps);
                SetSnapshot(ja, "length", a.length);
                SetSnapshot(ja, "name", a.name);
                SetSnapshot(ja, "mode", static_cast<int>(a.mode));
                SetSnapshot(
                    ja, "bone_track_count", static_cast<int>(a.bone_tracks.len().to_primitive()));
                SetSnapshot(ja, "has_trans", a.trans.is_some());
                SetSnapshot(ja,
                            "blend_curves_count",
                            static_cast<int>(a.blend_curves.len().to_primitive()));
                SetSnapshot(
                    ja, "v4_events_count", static_cast<int>(a.v4_events.len().to_primitive()));
                SetSnapshot(ja, "has_aabb", a.has_aabb);
                SetSnapshot(ja,
                            "scalar_curves_count",
                            static_cast<int>(a.scalar_curves.len().to_primitive()));
                SetSnapshot(ja, "events_count", static_cast<int>(a.events.len().to_primitive()));
                int total_frames = 0;
                for (const auto& bt : a.bone_tracks)
                    total_frames += static_cast<int>(bt.frames.len().to_primitive());
                SetSnapshot(ja, "total_bone_frames", total_frames);
                auto moved = owe::MakeArray();
                for (usize ti {}; ti < a.bone_tracks.len(); ++ti) {
                    const auto& tk = a.bone_tracks[ti];
                    if (tk.frames.is_empty()) continue;
                    const auto& f0      = tk.frames.first().unwrap().get();
                    bool        any_pos = false, any_sc = false, any_an = false;
                    for (const auto& fr : tk.frames) {
                        if ((fr.position - f0.position).norm() > 0.5f) any_pos = true;
                        if ((fr.scale - f0.scale).norm() > 0.01f) any_sc = true;
                        if ((fr.angle - f0.angle).norm() > 0.001f) any_an = true;
                    }
                    if (any_pos || any_sc || any_an) {
                        auto item = owe::MakeObject();
                        SetSnapshot(item, "i", static_cast<int>(ti.to_primitive()));
                        SetSnapshot(item, "p", any_pos);
                        SetSnapshot(item, "s", any_sc);
                        SetSnapshot(item, "a", any_an);
                        owe::AppendJson(moved, std::move(item));
                    }
                }
                owe::SetJson(ja, "moved_bones", std::move(moved));
                owe::AppendJson(anims, std::move(ja));
            }
            owe::SetJson(jm, "anim_tracks", std::move(anims));
        }
        owe::AppendJson(jmdl, std::move(jm));
    }
    sort_by_path(jmdl);
    owe::SetJson(out, "puppets", std::move(jmdl));

    return out;
}

const Corpus& Corpus::instance() {
    static const Corpus c;
    return c;
}

Corpus::Corpus() { build(); }

void Corpus::build() {
    fs::path root { kWorkshopDirMacro };
    if (! fs::exists(root) || ! fs::is_directory(root)) {
        std::fprintf(stderr, "corpus: workshop dir %s missing\n", root.c_str());
        return;
    }

    std::vector<fs::path> dirs;
    for (auto& e : fs::directory_iterator(root)) {
        if (! e.is_directory()) continue;
        if (! fs::exists(e.path() / "scene.pkg")) continue;
        dirs.push_back(e.path());
    }
    std::sort(dirs.begin(), dirs.end());

    entries_.reserve(dirs.size());
    for (const auto& d : dirs) {
        std::string id = d.filename().string();
        if (kSkipIds.contains(id)) continue;

        std::string err;
        auto        snap = DumpWorkshop(d.string(), err);
        if (! err.empty()) {
            std::fprintf(stderr, "corpus: skip %s: %s\n", id.c_str(), err.c_str());
            continue;
        }

        WorkshopEntry e { std::move(id), d.string(), std::move(snap) };

        if (auto pkg = e.snapshot.get("pkg"_str); pkg.is_some()) {
            auto version = (*pkg)->get("version"_str);
            if (version.is_some() && (*version)->as_str().is_some())
                pkg_versions_.insert(rstd::cppstd::to_string(*(*version)->as_str()));
        }
        if (auto textures = e.snapshot.get("textures"_str); textures.is_some()) {
            auto array = (*textures)->as_array();
            if (array.is_some())
                for (const auto& t : **array) {
                    auto ok = t.get("ok"_str);
                    if (ok.is_none() || ! (*ok)->as_bool().unwrap_or(false)) continue;
                    if (auto value = t.get("texv"_str); value.is_some())
                        texv_versions_.insert(static_cast<int>(JsonI64Or(**value, 0)));
                    if (auto value = t.get("texi"_str); value.is_some())
                        texi_versions_.insert(static_cast<int>(JsonI64Or(**value, 0)));
                    if (auto value = t.get("texb"_str); value.is_some())
                        texb_versions_.insert(static_cast<int>(JsonI64Or(**value, 0)));
                    if (auto value = t.get("texs"_str); value.is_some())
                        texs_versions_.insert(static_cast<int>(JsonI64Or(**value, 0)));
                    if (auto value = t.get("format"_str); value.is_some())
                        tex_formats_.insert(static_cast<int>(JsonI64Or(**value, -1)));
                }
        }
        if (auto puppets = e.snapshot.get("puppets"_str); puppets.is_some()) {
            auto array = (*puppets)->as_array();
            if (array.is_some())
                for (const auto& m : **array) {
                    if (auto value = m.get("mdlv"_str); value.is_some())
                        mdlv_versions_.insert(static_cast<int>(JsonI64Or(**value, 0)));
                    if (auto value = m.get("mdls"_str); value.is_some())
                        mdls_versions_.insert(static_cast<int>(JsonI64Or(**value, 0)));
                    if (auto value = m.get("mdla"_str); value.is_some())
                        mdla_versions_.insert(static_cast<int>(JsonI64Or(**value, 0)));
                }
        }
        entries_.push_back(std::move(e));
    }

    std::fprintf(stderr,
                 "corpus: indexed %zu workshops; pkgv=%zu texv=%zu texi=%zu texb=%zu "
                 "texs=%zu fmt=%zu mdlv=%zu mdls=%zu mdla=%zu\n",
                 entries_.size(),
                 pkg_versions_.size(),
                 texv_versions_.size(),
                 texi_versions_.size(),
                 texb_versions_.size(),
                 texs_versions_.size(),
                 tex_formats_.size(),
                 mdlv_versions_.size(),
                 mdls_versions_.size(),
                 mdla_versions_.size());
}

std::vector<Corpus::PkgRef> Corpus::workshops_with_pkg(const std::string& v) const {
    std::vector<PkgRef> out;
    for (const auto& e : entries_) {
        const auto version = e.snapshot.pointer("/pkg/version"_str);
        if (version.is_some()) {
            auto value = (*version)->as_str();
            if (value.is_some() && rstd::cppstd::as_string_view(*value) == v) out.push_back({ &e });
        }
    }
    return out;
}

std::vector<Corpus::TexRef> Corpus::textures_with_texv(int v) const {
    return tex_filter(entries_, [v](const Json& t) {
        auto value = t.get("texv"_str);
        return value.is_some() && JsonI64Or(**value, -1) == v;
    });
}
std::vector<Corpus::TexRef> Corpus::textures_with_texi(int v) const {
    return tex_filter(entries_, [v](const Json& t) {
        auto value = t.get("texi"_str);
        return value.is_some() && JsonI64Or(**value, -1) == v;
    });
}
std::vector<Corpus::TexRef> Corpus::textures_with_texb(int v) const {
    return tex_filter(entries_, [v](const Json& t) {
        auto value = t.get("texb"_str);
        return value.is_some() && JsonI64Or(**value, -1) == v;
    });
}
std::vector<Corpus::TexRef> Corpus::textures_with_texs(int v) const {
    return tex_filter(entries_, [v](const Json& t) {
        auto value = t.get("texs"_str);
        return value.is_some() && JsonI64Or(**value, -1) == v;
    });
}
std::vector<Corpus::TexRef> Corpus::textures_with_format(int v) const {
    return tex_filter(entries_, [v](const Json& t) {
        auto value = t.get("format"_str);
        return value.is_some() && JsonI64Or(**value, -1) == v;
    });
}
std::vector<Corpus::MdlRef> Corpus::mdls_with_mdlv(int v) const {
    return mdl_filter(entries_, [v](const Json& m) {
        auto value = m.get("mdlv"_str);
        return value.is_some() && JsonI64Or(**value, -1) == v;
    });
}
std::vector<Corpus::MdlRef> Corpus::mdls_with_mdls(int v) const {
    return mdl_filter(entries_, [v](const Json& m) {
        auto value = m.get("mdls"_str);
        return value.is_some() && JsonI64Or(**value, -1) == v;
    });
}
std::vector<Corpus::MdlRef> Corpus::mdls_with_mdla(int v) const {
    return mdl_filter(entries_, [v](const Json& m) {
        auto value = m.get("mdla"_str);
        return value.is_some() && JsonI64Or(**value, -1) == v;
    });
}

} // namespace owe::testing
