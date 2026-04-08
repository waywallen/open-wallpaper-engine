// Test corpus index.
//
// Walks workshop/* once, dumps every entry that has a scene.pkg via
// DumpWorkshop, and exposes lookup-by-version slices that the gtest
// fixtures parameterise on. The corpus is built lazily on first access
// (Meyer's singleton) so it's safe to call from INSTANTIATE_TEST_SUITE_P
// at static-init time.
//
// Skipped workshops (e.g. ones that hang WPMdlParser::Parse) are listed
// in corpus.cpp's kSkipIds and never parsed.

#pragma once

#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace wallpaper::testing {

struct WorkshopEntry {
    std::string    id;
    std::string    dir;
    nlohmann::json snapshot;
};

class Corpus {
public:
    // Returns the singleton, building it on first access.
    static const Corpus& instance();

    // All entries successfully dumped.
    const std::vector<WorkshopEntry>& entries() const { return entries_; }

    // Sorted unique sets of every version stamp observed across the corpus.
    // These drive INSTANTIATE_TEST_SUITE_P value lists.
    const std::set<std::string>& pkg_versions() const { return pkg_versions_; }
    const std::set<int>&         texv_versions() const { return texv_versions_; }
    const std::set<int>&         texi_versions() const { return texi_versions_; }
    const std::set<int>&         texb_versions() const { return texb_versions_; }
    const std::set<int>&         tex_formats() const { return tex_formats_; }
    const std::set<int>&         mdlv_versions() const { return mdlv_versions_; }
    const std::set<int>&         mdls_versions() const { return mdls_versions_; }
    const std::set<int>&         mdla_versions() const { return mdla_versions_; }

    // Slice accessors. Each returns a list of (workshop_id, json_pointer)
    // pairs identifying the matching pkg / texture / mdl entry. The json
    // payload is owned by the corpus singleton, so the returned pointers
    // remain valid for the lifetime of the test process.
    struct PkgRef {
        const WorkshopEntry* workshop;
    };
    struct TexRef {
        const WorkshopEntry* workshop;
        const nlohmann::json* tex;
    };
    struct MdlRef {
        const WorkshopEntry* workshop;
        const nlohmann::json* mdl;
    };

    std::vector<PkgRef> workshops_with_pkg(const std::string& pkgv) const;
    std::vector<TexRef> textures_with_texv(int v) const;
    std::vector<TexRef> textures_with_texi(int v) const;
    std::vector<TexRef> textures_with_texb(int v) const;
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
    std::set<int>              tex_formats_;
    std::set<int>              mdlv_versions_;
    std::set<int>              mdls_versions_;
    std::set<int>              mdla_versions_;
};

} // namespace wallpaper::testing
