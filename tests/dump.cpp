#include "dump.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "WPCommon.hpp"
#include "WPMdlParser.hpp"
#include "WPPkgFs.hpp"
#include "WPTexImageParser.hpp"
#include "Image.hpp"
#include "wpscene/WPImageObject.h"
#include "wpscene/WPMaterial.h"
#include "wpscene/WPScene.h"

#include "Fs/CBinaryStream.h"
#include "Fs/IBinaryStream.h"
#include "Fs/PhysicalFs.h"
#include "Fs/VFS.h"

namespace wallpaper::testing {

namespace {

namespace fs = std::filesystem;
using json   = nlohmann::json;

// --- pkg header re-parser ----------------------------------------------------
//
// We deliberately re-parse scene.pkg's header here instead of reaching into
// WPPkgFs internals: the production class throws away the version string
// after logging it, and exposes neither file enumeration nor the version.
// Re-parsing is ~15 lines and keeps the production code untouched.

struct PkgEntry {
    std::string path;
    int32_t     offset { 0 };
    int32_t     length { 0 };
};

std::string ReadSizedString(wallpaper::fs::IBinaryStream& f) {
    int32_t len = f.ReadInt32();
    if (len < 0) return {};
    std::string out;
    out.resize(static_cast<std::size_t>(len));
    f.Read(out.data(), static_cast<std::size_t>(len));
    return out;
}

bool ReadPkgHeader(const std::string& pkg_path, std::string& version,
                   std::vector<PkgEntry>& entries) {
    auto stream = wallpaper::fs::CreateCBinaryStream(pkg_path);
    if (! stream) return false;
    version            = ReadSizedString(*stream);
    int32_t entryCount = stream->ReadInt32();
    if (entryCount < 0) return false;
    entries.reserve(static_cast<std::size_t>(entryCount));
    for (int32_t i = 0; i < entryCount; ++i) {
        PkgEntry e;
        e.path   = "/" + ReadSizedString(*stream);
        e.offset = stream->ReadInt32();
        e.length = stream->ReadInt32();
        entries.push_back(std::move(e));
    }
    return true;
}

// --- texture header reader ---------------------------------------------------
//
// Delegates to WPTexImageParser::ParseHeader so we exercise the same
// production code path the renderer uses. ParseHeader fills ImageHeader,
// including sprite frame counts (numFrames()) when isSprite is true.

struct TexMeta {
    std::string path;
    int32_t     texv { 0 };
    int32_t     texi { 0 };
    int32_t     texb { 0 };
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

TexMeta ReadTexMeta(wallpaper::fs::VFS& vfs, const std::string& pkg_path) {
    TexMeta meta;
    meta.path = pkg_path;

    // ParseHeader takes a "name" without /assets/materials/ prefix or
    // .tex suffix, so strip both before passing it through.
    constexpr std::string_view prefix = "/materials/";
    constexpr std::string_view suffix = ".tex";
    if (pkg_path.compare(0, prefix.size(), prefix) != 0) return meta;
    if (pkg_path.size() < prefix.size() + suffix.size()) return meta;
    if (pkg_path.compare(pkg_path.size() - suffix.size(), suffix.size(), suffix) != 0)
        return meta;
    std::string name = pkg_path.substr(prefix.size(),
                                       pkg_path.size() - prefix.size() - suffix.size());

    wallpaper::WPTexImageParser parser(&vfs);
    wallpaper::ImageHeader      h;
    try {
        h = parser.ParseHeader(name);
    } catch (const std::exception&) {
        return meta;
    }

    auto extra_val = [&](const std::string& k) -> int32_t {
        auto it = h.extraHeader.find(k);
        return it == h.extraHeader.end() ? 0 : it->second.val;
    };
    meta.texv          = extra_val("texv");
    meta.texi          = extra_val("texi");
    meta.texb          = extra_val("texb");
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
    meta.sprite_frames = static_cast<int64_t>(h.spriteAnim.numFrames());
    meta.mipmap_pow2   = h.mipmap_pow2;
    meta.mipmap_larger = h.mipmap_larger;
    meta.wrap_s        = static_cast<int>(h.sample.wrapS);
    meta.wrap_t        = static_cast<int>(h.sample.wrapT);
    meta.min_filter    = static_cast<int>(h.sample.minFilter);
    meta.mag_filter    = static_cast<int>(h.sample.magFilter);
    meta.ok            = (meta.texv > 0 && meta.width > 0 && meta.height > 0);
    return meta;
}

// --- helpers -----------------------------------------------------------------

bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Sort an array of objects by a string key for deterministic output.
void sort_by_path(json& arr) {
    std::sort(arr.begin(), arr.end(), [](const json& a, const json& b) {
        return a.value("path", std::string {}) < b.value("path", std::string {});
    });
}

// Convert an unordered_map<string, T> to a json object. nlohmann::json's
// default object storage sorts keys alphabetically, so the result is
// deterministic across runs.
template <typename Map>
json map_to_json(const Map& m) {
    json o = json::object();
    for (const auto& [k, v] : m) o[k] = v;
    return o;
}

json dump_material(const wallpaper::wpscene::WPMaterial& m) {
    return {
        { "shader", m.shader },
        { "blending", m.blending },
        { "cullmode", m.cullmode },
        { "depthtest", m.depthtest },
        { "depthwrite", m.depthwrite },
        { "use_puppet", m.use_puppet },
        { "textures", m.textures },
        { "combos", map_to_json(m.combos) },
        { "constantshadervalues", map_to_json(m.constantshadervalues) },
    };
}

// Pull the universal transform-ish fields straight off the raw object
// json so unknown subtypes (light/particle/sound) still produce a row.
// Field types in scene.json are inconsistent (origin can be either an
// array of floats or a "x y z" string), so we copy the raw value through
// instead of forcing a particular C++ type.
json dump_object_common(const json& obj) {
    json o;
    o["id"]      = obj.value("id", -1);
    o["name"]    = obj.value("name", std::string {});
    o["visible"] = obj.value("visible", true);
    for (const char* key :
         { "origin", "scale", "angles", "size", "parallaxDepth", "alignment" }) {
        if (obj.contains(key)) o[key] = obj[key];
    }
    return o;
}

// Run WPImageObject::FromJson against a single object json and dump the
// parsed fields. Returns nullopt if the object is not an image object
// (no "image" field) so the caller can fall back to common-only dumps.
json dump_image_object(const json& obj, wallpaper::fs::VFS& vfs) {
    json out                          = dump_object_common(obj);
    out["kind"]                       = "image";
    wallpaper::wpscene::WPImageObject img;
    bool                              ok = false;
    try {
        ok = img.FromJson(obj, vfs);
    } catch (const std::exception&) {
        ok = false;
    }
    out["parsed"]         = ok;
    if (! ok) return out;
    out["image"]          = img.image;
    out["color"]          = img.color;
    out["colorBlendMode"] = img.colorBlendMode;
    out["alpha"]          = img.alpha;
    out["brightness"]     = img.brightness;
    out["fullscreen"]     = img.fullscreen;
    out["nopadding"]      = img.nopadding;
    out["origin_parsed"]  = img.origin;
    out["scale_parsed"]   = img.scale;
    out["angles_parsed"]  = img.angles;
    out["size_parsed"]    = img.size;
    out["visible_parsed"] = img.visible;
    out["alignment_parsed"] = img.alignment;
    out["puppet"]         = img.puppet;
    out["material"]       = dump_material(img.material);
    out["effect_count"]   = static_cast<int>(img.effects.size());
    // WPImageEffect::id and ::version are left uninitialised by the
    // parser when the source json omits them, so dumping their raw
    // value produces stack garbage. Skip them — what we really care
    // about is the structural shape (name + sub-counts + materials).
    json effs = json::array();
    for (const auto& e : img.effects) {
        json je;
        je["name"]           = e.name;
        je["visible"]        = e.visible;
        je["material_count"] = static_cast<int>(e.materials.size());
        je["pass_count"]     = static_cast<int>(e.passes.size());
        je["fbo_count"]      = static_cast<int>(e.fbos.size());
        json mats            = json::array();
        for (const auto& mm : e.materials) mats.push_back(dump_material(mm));
        je["materials"] = std::move(mats);
        effs.push_back(std::move(je));
    }
    out["effects"] = std::move(effs);
    return out;
}

} // namespace

json DumpWorkshop(const std::string& workshop_dir, std::string& err) {
    err.clear();
    json out;
    out["workshop_dir"] = fs::path(workshop_dir).filename().string();

    const std::string pkg_path = workshop_dir + "/scene.pkg";
    if (! fs::exists(pkg_path)) {
        err           = "scene.pkg not found at " + pkg_path;
        out["error"]  = err;
        return out;
    }

    // ---- pkg header --------------------------------------------------------
    std::string           pkg_version;
    std::vector<PkgEntry> pkg_entries;
    if (! ReadPkgHeader(pkg_path, pkg_version, pkg_entries)) {
        err          = "failed to read pkg header";
        out["error"] = err;
        return out;
    }

    bool has_scene_json = false;
    for (const auto& e : pkg_entries)
        if (e.path == "/scene.json") {
            has_scene_json = true;
            break;
        }

    json& jpkg          = out["pkg"];
    jpkg["version"]     = pkg_version;
    jpkg["file_count"]  = static_cast<int>(pkg_entries.size());
    jpkg["has_scene_json"] = has_scene_json;

    // ---- mount VFS ---------------------------------------------------------
    wallpaper::fs::VFS vfs;
    auto pfs = wallpaper::fs::CreatePhysicalFs(workshop_dir);
    auto wfs = wallpaper::fs::WPPkgFs::CreatePkgFs(pkg_path);
    if (! wfs) {
        err          = "WPPkgFs::CreatePkgFs failed";
        out["error"] = err;
        return out;
    }
    vfs.Mount("/assets", std::move(wfs));
    if (pfs) vfs.Mount("/assets", std::move(pfs));

    // ---- scene.json --------------------------------------------------------
    if (has_scene_json) {
        auto stream = vfs.Open("/assets/scene.json");
        if (stream) {
            std::string text = stream->ReadAllStr();
            try {
                auto j = json::parse(text);
                wallpaper::wpscene::WPScene scene;
                bool                        parsed = scene.FromJson(j);
                json&                       jscene = out["scene"];
                jscene["parsed"]                   = parsed;
                jscene["is_ortho"]                 = scene.general.isOrtho;
                jscene["ortho"]                    = {
                    { "width", scene.general.orthogonalprojection.width },
                    { "height", scene.general.orthogonalprojection.height },
                };
                jscene["camera"] = {
                    { "center", scene.camera.center },
                    { "eye", scene.camera.eye },
                    { "up", scene.camera.up },
                };
                jscene["general"] = {
                    { "clearcolor", scene.general.clearcolor },
                    { "ambientcolor", scene.general.ambientcolor },
                    { "skylightcolor", scene.general.skylightcolor },
                    { "cameraparallax", scene.general.cameraparallax },
                    { "cameraparallaxamount", scene.general.cameraparallaxamount },
                    { "cameraparallaxdelay", scene.general.cameraparallaxdelay },
                    { "cameraparallaxmouseinfluence",
                      scene.general.cameraparallaxmouseinfluence },
                    { "zoom", scene.general.zoom },
                    { "fov", scene.general.fov },
                    { "nearz", scene.general.nearz },
                    { "farz", scene.general.farz },
                };
                // ---- objects ----
                json jobjects = json::array();
                if (j.contains("objects") && j["objects"].is_array()) {
                    for (const auto& obj : j["objects"]) {
                        if (obj.contains("image")) {
                            jobjects.push_back(dump_image_object(obj, vfs));
                        } else {
                            json o     = dump_object_common(obj);
                            o["kind"]  = obj.contains("light")    ? "light"
                                         : obj.contains("particle") ? "particle"
                                         : obj.contains("sound")    ? "sound"
                                                                    : "unknown";
                            jobjects.push_back(std::move(o));
                        }
                    }
                }
                std::sort(jobjects.begin(), jobjects.end(), [](const json& a, const json& b) {
                    return a.value("id", -1) < b.value("id", -1);
                });
                jscene["object_count"] = static_cast<int>(jobjects.size());
                jscene["objects"]      = std::move(jobjects);
            } catch (const std::exception& e) {
                out["scene"] = { { "parsed", false }, { "error", e.what() } };
            }
        }
    }

    // ---- textures ----------------------------------------------------------
    json jtex = json::array();
    for (const auto& e : pkg_entries) {
        if (! ends_with(e.path, ".tex")) continue;
        if (e.path.rfind("/materials/", 0) != 0) continue;
        std::string vfs_path = "/assets" + e.path;
        TexMeta     m        = ReadTexMeta(vfs, e.path);
        json        jm;
        jm["path"]          = e.path;
        jm["ok"]            = m.ok;
        jm["texv"]          = m.texv;
        jm["texi"]          = m.texi;
        jm["texb"]          = m.texb;
        jm["compo1"]        = m.compo1;
        jm["compo2"]        = m.compo2;
        jm["compo3"]        = m.compo3;
        jm["format"]        = m.format;
        jm["image_type"]    = m.image_type;
        jm["width"]         = m.width;
        jm["height"]        = m.height;
        jm["map_width"]     = m.map_width;
        jm["map_height"]    = m.map_height;
        jm["count"]         = m.count;
        jm["is_sprite"]     = m.is_sprite;
        jm["sprite_frames"] = m.sprite_frames;
        jm["mipmap_pow2"]   = m.mipmap_pow2;
        jm["mipmap_larger"] = m.mipmap_larger;
        jm["wrap_s"]        = m.wrap_s;
        jm["wrap_t"]        = m.wrap_t;
        jm["min_filter"]    = m.min_filter;
        jm["mag_filter"]    = m.mag_filter;
        jtex.push_back(std::move(jm));
    }
    sort_by_path(jtex);
    out["textures"] = std::move(jtex);

    // ---- puppets / mdls ----------------------------------------------------
    json jmdl = json::array();
    for (const auto& e : pkg_entries) {
        if (! ends_with(e.path, ".mdl")) continue;
        // WPMdlParser::Parse expects a path relative to /assets without the
        // leading slash.
        std::string rel = e.path;
        if (! rel.empty() && rel.front() == '/') rel.erase(0, 1);
        WPMdl mdl;
        bool  ok = false;
        try {
            ok = wallpaper::WPMdlParser::Parse(rel, vfs, mdl);
        } catch (const std::exception&) {
            ok = false;
        }
        json jm;
        jm["path"]          = e.path;
        jm["ok"]            = ok;
        jm["mdlv"]          = mdl.mdlv;
        jm["mdls"]          = mdl.mdls;
        jm["mdla"]          = mdl.mdla;
        jm["mat_json_file"] = mdl.mat_json_file;
        jm["vertex_count"]  = static_cast<int>(mdl.vertexs.size());
        jm["index_count"]   = static_cast<int>(mdl.indices.size());
        jm["bones"]         = ok && mdl.puppet ? static_cast<int>(mdl.puppet->bones.size()) : 0;
        jm["anims"]         = ok && mdl.puppet ? static_cast<int>(mdl.puppet->anims.size()) : 0;
        if (ok && mdl.puppet) {
            // Bone tree: parent index per bone + a translation hash (sum of
            // the four matrix columns) so a parser regression that flips
            // sign / column order or drops a bone is caught immediately.
            json bones = json::array();
            for (const auto& b : mdl.puppet->bones) {
                json jb;
                jb["parent"] = static_cast<int64_t>(b.parent);
                std::array<double, 4> col_sums { 0, 0, 0, 0 };
                for (int c = 0; c < 4; ++c)
                    for (int r = 0; r < 4; ++r)
                        col_sums[static_cast<std::size_t>(c)] += b.transform.matrix()(r, c);
                jb["transform_col_sums"] = col_sums;
                bones.push_back(std::move(jb));
            }
            jm["bone_tree"] = std::move(bones);

            json anims = json::array();
            for (const auto& a : mdl.puppet->anims) {
                json ja;
                ja["id"]       = a.id;
                ja["fps"]      = a.fps;
                ja["length"]   = a.length;
                ja["name"]     = a.name;
                ja["mode"]     = static_cast<int>(a.mode);
                ja["bone_track_count"] = static_cast<int>(a.bframes_array.size());
                int total_frames = 0;
                for (const auto& bf : a.bframes_array)
                    total_frames += static_cast<int>(bf.frames.size());
                ja["total_bone_frames"] = total_frames;
                anims.push_back(std::move(ja));
            }
            jm["anim_tracks"] = std::move(anims);
        }
        jmdl.push_back(std::move(jm));
    }
    sort_by_path(jmdl);
    out["puppets"] = std::move(jmdl);

    return out;
}

} // namespace wallpaper::testing
