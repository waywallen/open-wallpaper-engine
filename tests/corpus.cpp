#include "corpus.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>

#include "dump.hpp"

namespace wallpaper::testing {

namespace {

namespace fs = std::filesystem;

// Workshops that hang or crash the dumper. Listed here so the corpus
// build never blocks. Tracked as Iter A in the test roadmap (fix
// WPMdlParser::Parse's missing EOF guard).
const std::set<std::string> kSkipIds {
    "2435537849",
};

// Workshop root: resolved at build time via the WAYWALLEN_WORKSHOP_DIR
// macro defined in tests/CMakeLists.txt. Falls back to "workshop" so the
// binary stays runnable from the source root for ad-hoc dev loops.
constexpr const char* kWorkshopDirMacro =
#ifdef WAYWALLEN_WORKSHOP_DIR
    WAYWALLEN_WORKSHOP_DIR
#else
    "workshop"
#endif
    ;

} // namespace

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

        // Harvest version stamps for the parameter generators.
        if (e.snapshot.contains("pkg"))
            pkg_versions_.insert(e.snapshot["pkg"].value("version", std::string {}));
        if (e.snapshot.contains("textures")) {
            for (const auto& t : e.snapshot["textures"]) {
                if (! t.value("ok", false)) continue;
                texv_versions_.insert(t.value("texv", 0));
                texi_versions_.insert(t.value("texi", 0));
                texb_versions_.insert(t.value("texb", 0));
                tex_formats_.insert(t.value("format", -1));
            }
        }
        if (e.snapshot.contains("puppets")) {
            for (const auto& m : e.snapshot["puppets"]) {
                mdlv_versions_.insert(m.value("mdlv", 0));
                mdls_versions_.insert(m.value("mdls", 0));
                mdla_versions_.insert(m.value("mdla", 0));
            }
        }
        entries_.push_back(std::move(e));
    }

    std::fprintf(stderr,
                 "corpus: indexed %zu workshops; pkgv=%zu texv=%zu texi=%zu texb=%zu "
                 "fmt=%zu mdlv=%zu mdls=%zu mdla=%zu\n",
                 entries_.size(), pkg_versions_.size(), texv_versions_.size(),
                 texi_versions_.size(), texb_versions_.size(), tex_formats_.size(),
                 mdlv_versions_.size(), mdls_versions_.size(), mdla_versions_.size());
}

std::vector<Corpus::PkgRef> Corpus::workshops_with_pkg(const std::string& v) const {
    std::vector<PkgRef> out;
    for (const auto& e : entries_)
        if (e.snapshot.value("/pkg/version"_json_pointer, std::string {}) == v) out.push_back({ &e });
    return out;
}

namespace {

template <typename Predicate>
std::vector<Corpus::TexRef> tex_filter(const std::vector<WorkshopEntry>& es, Predicate pred) {
    std::vector<Corpus::TexRef> out;
    for (const auto& e : es) {
        if (! e.snapshot.contains("textures")) continue;
        for (const auto& t : e.snapshot["textures"]) {
            if (! t.value("ok", false)) continue;
            if (pred(t)) out.push_back({ &e, &t });
        }
    }
    return out;
}

template <typename Predicate>
std::vector<Corpus::MdlRef> mdl_filter(const std::vector<WorkshopEntry>& es, Predicate pred) {
    std::vector<Corpus::MdlRef> out;
    for (const auto& e : es) {
        if (! e.snapshot.contains("puppets")) continue;
        for (const auto& m : e.snapshot["puppets"]) {
            if (pred(m)) out.push_back({ &e, &m });
        }
    }
    return out;
}

} // namespace

std::vector<Corpus::TexRef> Corpus::textures_with_texv(int v) const {
    return tex_filter(entries_, [v](const nlohmann::json& t) { return t.value("texv", -1) == v; });
}
std::vector<Corpus::TexRef> Corpus::textures_with_texi(int v) const {
    return tex_filter(entries_, [v](const nlohmann::json& t) { return t.value("texi", -1) == v; });
}
std::vector<Corpus::TexRef> Corpus::textures_with_texb(int v) const {
    return tex_filter(entries_, [v](const nlohmann::json& t) { return t.value("texb", -1) == v; });
}
std::vector<Corpus::TexRef> Corpus::textures_with_format(int v) const {
    return tex_filter(entries_, [v](const nlohmann::json& t) { return t.value("format", -1) == v; });
}
std::vector<Corpus::MdlRef> Corpus::mdls_with_mdlv(int v) const {
    return mdl_filter(entries_, [v](const nlohmann::json& m) { return m.value("mdlv", -1) == v; });
}
std::vector<Corpus::MdlRef> Corpus::mdls_with_mdls(int v) const {
    return mdl_filter(entries_, [v](const nlohmann::json& m) { return m.value("mdls", -1) == v; });
}
std::vector<Corpus::MdlRef> Corpus::mdls_with_mdla(int v) const {
    return mdl_filter(entries_, [v](const nlohmann::json& m) { return m.value("mdla", -1) == v; });
}

} // namespace wallpaper::testing
