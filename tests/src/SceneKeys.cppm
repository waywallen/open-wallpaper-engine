// scene.json key-path scanner.
//
// Walks every workshop/<id>/scene.pkg under workshop_root, extracts the
// raw scene.json from the pkg, and aggregates per-pkg-version statistics
// of every key path observed in the json tree.

module;

#include <cstdio>
#include <rstd/enum.hpp>

export module wescene.testing.scene_keys;

import rstd.cppstd;
import wescene.json;
import wescene.pkg_fs;
import wescene.fs;
import wescene.testing.pkg_header;
import wescene.testing.json_builder;

using namespace rstd::literals;

export namespace owe::testing
{

// Returns:
//   {
//     "<pkg_version>": {
//       "total_scenes": <int>,
//       "keys": {
//         "<dot.path[].with.brackets>": {
//           "present_in":  <int>,
//           "occurrences": <int>,
//           "value_types": ["object", "array", "string", ...]
//         },
//         ...
//       }
//     },
//     ...
//   }
owe::Json ScanSceneKeys(const std::string& workshop_root);

} // namespace owe::testing

namespace owe::testing
{

namespace
{

namespace fs = std::filesystem;
using Json   = owe::Json;

const char* TypeName(const Json& value) {
    RSTD_MATCH(value) {
        RSTD_CASE(Null) { return "null"; }
        RSTD_CASE(Object) { return "object"; }
        RSTD_CASE(Array) { return "array"; }
        RSTD_CASE(String) { return "string"; }
        RSTD_CASE(Bool) { return "boolean"; }
        RSTD_CASE(Number, number) {
            if (number.is_f64()) return "number_float";
            if (number.is_u64()) return "number_unsigned";
            return "number_integer";
        }
    }
    rstd::unreachable();
}

struct LocalObs {
    int                   occurrences { 0 };
    std::set<std::string> types;
};
using LocalMap = std::unordered_map<std::string, LocalObs>;

void Walk(const Json& node, std::string& path, LocalMap& out) {
    if (! path.empty()) {
        auto& obs = out[path];
        obs.occurrences += 1;
        obs.types.insert(TypeName(node));
    }
    RSTD_MATCH(node) {
        RSTD_CASE(Object, object) {
            object.iter().for_each([&](auto entry) {
                auto [entry_key, entry_value] = entry;
                const auto        key         = rstd::cppstd::as_string_view(entry_key->as_str());
                const auto&       value       = *entry_value;
                const std::size_t old         = path.size();
                if (! path.empty()) path += '.';
                path += key;
                Walk(value, path, out);
                path.resize(old);
            });
            return;
        }
        RSTD_CASE(Array, array) {
            const std::size_t old = path.size();
            path += "[]";
            for (const auto& el : array) Walk(el, path, out);
            path.resize(old);
            return;
        }
        RSTD_CASE(Null) { return; }
        RSTD_CASE(Bool) { return; }
        RSTD_CASE(Number) { return; }
        RSTD_CASE(String) { return; }
    }
}

struct KeyStats {
    std::uint64_t         present_in { 0 };
    std::uint64_t         occurrences { 0 };
    std::set<std::string> value_types;
};

struct VersionAgg {
    std::uint64_t                   total_scenes { 0 };
    std::map<std::string, KeyStats> keys;
};

void Merge(VersionAgg& agg, const LocalMap& local) {
    agg.total_scenes += 1;
    for (const auto& [path, obs] : local) {
        auto& dst = agg.keys[path];
        dst.present_in += 1;
        dst.occurrences += static_cast<std::uint64_t>(obs.occurrences);
        for (const auto& t : obs.types) dst.value_types.insert(t);
    }
}

bool ScanOneWorkshop(const fs::path& workshop_dir, std::map<std::string, VersionAgg>& by_version) {
    const std::string id       = workshop_dir.filename().string();
    const std::string pkg_path = (workshop_dir / "scene.pkg").string();

    if (! fs::exists(pkg_path)) return false;

    std::string           pkg_version;
    std::vector<PkgEntry> entries;
    if (! ReadPkgHeader(pkg_path, pkg_version, entries)) {
        std::fprintf(stderr, "wpscan: skip %s: bad pkg header\n", id.c_str());
        return false;
    }

    bool has_scene_json = false;
    for (const auto& e : entries)
        if (e.path == "/scene.json") {
            has_scene_json = true;
            break;
        }
    if (! has_scene_json) return false;

    auto wfs = owe::fs::WPPkgFs::open(owe::fs::Path(rstd::cppstd::as_str(pkg_path).unwrap()));
    if (wfs.is_err()) {
        std::fprintf(stderr, "wpscan: skip %s: WPPkgFs::open failed\n", id.c_str());
        return false;
    }
    owe::fs::VFS vfs;
    if (vfs.mount("/assets"_str, wfs->mount_handle()).is_err()) return false;

    auto stream = owe::fs::OpenBinary(vfs, owe::fs::Path("/assets/scene.json"_str));
    if (stream.is_err()) {
        std::fprintf(stderr, "wpscan: skip %s: scene.json open failed\n", id.c_str());
        return false;
    }
    auto text = stream->ReadAllStr();

    auto parsed = owe::ParseJson(text.as_str());
    if (parsed.is_err()) {
        std::fprintf(stderr, "wpscan: skip %s: invalid scene JSON\n", id.c_str());
        return false;
    }
    auto scene = parsed.unwrap();

    LocalMap    local;
    std::string path;
    Walk(scene, path, local);

    Merge(by_version[pkg_version], local);
    return true;
}

Json AggToJson(const std::map<std::string, VersionAgg>& by_version) {
    auto out = owe::MakeObject();
    for (const auto& [version, agg] : by_version) {
        auto jv = owe::MakeObject();
        owe::SetMember(jv, "total_scenes", agg.total_scenes);
        auto jkeys = owe::MakeObject();
        for (const auto& [path, st] : agg.keys) {
            auto jk = owe::MakeObject();
            owe::SetMember(jk, "present_in", st.present_in);
            owe::SetMember(jk, "occurrences", st.occurrences);
            auto jtypes = owe::MakeArray();
            for (const auto& t : st.value_types) owe::AppendElement(jtypes, t);
            owe::SetMember(jk, "value_types", std::move(jtypes));
            owe::SetMember(jkeys, path, std::move(jk));
        }
        owe::SetMember(jv, "keys", std::move(jkeys));
        owe::SetMember(out, version, std::move(jv));
    }
    return out;
}

} // namespace

Json ScanSceneKeys(const std::string& workshop_root) {
    std::map<std::string, VersionAgg> by_version;

    fs::path root { workshop_root };
    if (! fs::exists(root) || ! fs::is_directory(root)) {
        std::fprintf(stderr, "wpscan: workshop dir %s missing\n", root.c_str());
        return AggToJson(by_version);
    }

    std::vector<fs::path> dirs;
    for (auto& e : fs::directory_iterator(root)) {
        if (! e.is_directory()) continue;
        dirs.push_back(e.path());
    }
    std::sort(dirs.begin(), dirs.end());

    std::uint64_t scanned = 0;
    for (const auto& d : dirs) {
        if (ScanOneWorkshop(d, by_version)) ++scanned;
    }

    std::uint64_t total_keys = 0;
    for (const auto& [_, agg] : by_version) total_keys += agg.keys.size();
    std::fprintf(stderr,
                 "wpscan: scanned %llu scenes, %zu pkg versions, %llu unique key paths\n",
                 static_cast<unsigned long long>(scanned),
                 by_version.size(),
                 static_cast<unsigned long long>(total_keys));

    return AggToJson(by_version);
}

} // namespace owe::testing
