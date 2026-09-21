// Per-workshop probe of root scene.json parsing with explicit pkg version.

module;

export module wescene.testing.scene_parse_probe;

import rstd.cppstd;
import wescene.pkg.parse;
import wescene.fs;
using namespace rstd::literals;

export namespace owe::testing
{

struct SceneParseResult {
    bool        ok { false };
    std::string error;
    // pkg_version: integer parsed from "PKGV00xx"; 0 means unknown / loose dir.
    std::uint16_t pkg_version { 0 };
    // scene_json_version: scene.json's own top-level "version" field; 0 if absent.
    std::uint16_t scene_json_version { 0 };
};

// Reads workshop_dir/scene.pkg through the parse-layer document loader.
// All failures come back as ok=false with a populated `error`.
SceneParseResult ProbeSceneParse(const std::string& workshop_dir);

struct WorkshopProbe {
    std::string   id;          // workshop directory name (steam id)
    std::string   dir;         // absolute workshop directory
    std::string   pkg_stamp;   // raw "PKGV00xx" string from pkg header
    std::uint16_t pkg_version; // ParsePkgVersionStamp(pkg_stamp); 0 on error
};

// Light-weight enumerator: walks `workshop_root` for direct subdirectories
// containing `scene.pkg`, reads only the version stamp, and returns a
// sorted list. Does NOT call DumpWorkshop / MdlParser / TexImageParser
// — safe to use even when the corpus has a workshop that crashes those.
std::vector<WorkshopProbe> EnumerateWorkshopProbes(const std::string& workshop_root);

} // namespace owe::testing

namespace owe::testing
{

namespace
{

namespace fs = std::filesystem;

// Mirrors WPPkgFs::open's first read: just the length-prefixed
// version stamp. Avoids paying for the full pkg+vfs construction.
bool ReadPkgVersionStamp(const std::string& pkg_path, std::string& out) {
    auto stream =
        owe::fs::OpenPhysicalBinary(owe::fs::Path(rstd::cppstd::as_str(pkg_path).unwrap()));
    if (stream.is_err()) return false;
    std::int32_t len = stream->ReadInt32();
    if (len < 0) return false;
    out.resize(static_cast<std::size_t>(len));
    stream->Read(out.data(), static_cast<std::size_t>(len));
    return true;
}

} // namespace

SceneParseResult ProbeSceneParse(const std::string& workshop_dir) {
    SceneParseResult out;

    const std::string pkg_path = workshop_dir + "/scene.pkg";
    if (! fs::exists(pkg_path)) {
        out.error = "scene.pkg not found";
        return out;
    }

    auto doc =
        wpscene::LoadSceneDocumentFromPkg(owe::fs::Path(rstd::cppstd::as_str(pkg_path).unwrap()));
    if (! doc) {
        out.error = "LoadSceneDocumentFromPkg returned nullopt";
        return out;
    }
    out.pkg_version        = doc->metadata.pkg_version;
    out.scene_json_version = doc->metadata.scene_json_version;

    out.ok = true;
    return out;
}

std::vector<WorkshopProbe> EnumerateWorkshopProbes(const std::string& workshop_root) {
    std::vector<WorkshopProbe> out;
    fs::path                   root { workshop_root };
    if (! fs::exists(root) || ! fs::is_directory(root)) return out;

    std::vector<fs::path> dirs;
    for (auto& e : fs::directory_iterator(root)) {
        if (! e.is_directory()) continue;
        if (! fs::exists(e.path() / "scene.pkg")) continue;
        dirs.push_back(e.path());
    }
    std::sort(dirs.begin(), dirs.end());

    out.reserve(dirs.size());
    for (const auto& d : dirs) {
        WorkshopProbe p;
        p.id  = d.filename().string();
        p.dir = d.string();
        if (! ReadPkgVersionStamp((d / "scene.pkg").string(), p.pkg_stamp)) {
            continue;
        }
        p.pkg_version = wpscene::ParsePkgVersionStamp(rstd::cppstd::as_str(p.pkg_stamp).unwrap());
        out.push_back(std::move(p));
    }
    return out;
}

} // namespace owe::testing
