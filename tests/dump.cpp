#include "dump.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "WPCommon.hpp"
#include "WPMdlParser.hpp"
#include "WPPkgFs.hpp"
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
// Mirrors src/WPTexImageParser.cpp::LoadHeader (which lives in an anonymous
// namespace and is therefore not reusable). We only read the bytes we need
// for fingerprinting; the actual pixel payload is intentionally skipped.

struct TexMeta {
    std::string path;
    int32_t     texv { 0 };
    int32_t     texi { 0 };
    int32_t     texb { 0 };
    int32_t     format { 0 };
    uint32_t    flags { 0 };
    int32_t     width { 0 };
    int32_t     height { 0 };
    int32_t     map_width { 0 };
    int32_t     map_height { 0 };
    int32_t     count { 0 };
    bool        ok { false };
};

TexMeta ReadTexMeta(wallpaper::fs::VFS& vfs, const std::string& vfs_path) {
    TexMeta meta;
    meta.path  = vfs_path;
    auto pfile = vfs.Open(vfs_path);
    if (! pfile) return meta;
    auto& f      = *pfile;
    meta.texv    = wallpaper::ReadTexVesion(f);
    meta.texi    = wallpaper::ReadTexVesion(f);
    meta.format  = f.ReadInt32();
    meta.flags   = f.ReadUint32();
    meta.width   = f.ReadInt32();
    meta.height  = f.ReadInt32();
    meta.map_width  = f.ReadInt32();
    meta.map_height = f.ReadInt32();
    f.ReadInt32(); // unknown
    meta.texb    = wallpaper::ReadTexVesion(f);
    meta.count   = f.ReadInt32();
    meta.ok      = (meta.texv > 0 && meta.width > 0 && meta.height > 0);
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
        TexMeta     m        = ReadTexMeta(vfs, vfs_path);
        json        jm;
        jm["path"]       = e.path;
        jm["ok"]         = m.ok;
        jm["texv"]       = m.texv;
        jm["texi"]       = m.texi;
        jm["texb"]       = m.texb;
        jm["format"]     = m.format;
        jm["flags"]      = m.flags;
        jm["width"]      = m.width;
        jm["height"]     = m.height;
        jm["map_width"]  = m.map_width;
        jm["map_height"] = m.map_height;
        jm["count"]      = m.count;
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
        jm["path"] = e.path;
        jm["ok"]   = ok;
        jm["mdlv"] = mdl.mdlv;
        jm["mdls"] = mdl.mdls;
        jm["mdla"] = mdl.mdla;
        jm["bones"] = ok && mdl.puppet ? static_cast<int>(mdl.puppet->bones.size()) : 0;
        jm["anims"] = ok && mdl.puppet ? static_cast<int>(mdl.puppet->anims.size()) : 0;
        jmdl.push_back(std::move(jm));
    }
    sort_by_path(jmdl);
    out["puppets"] = std::move(jmdl);

    return out;
}

} // namespace wallpaper::testing
