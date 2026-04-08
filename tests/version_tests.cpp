// Per-version gtest fixtures.
//
// One TEST_P per category (pkg / texv / texi / texb / texture format /
// mdlv / mdls / mdla). The parameter list comes from the corpus
// singleton which walks workshop/* and harvests every version stamp
// observed in the wild. Each test slices the corpus to entries
// matching its parameter and asserts:
//   * the slice is non-empty (otherwise the parameter wouldn't have
//     been generated)
//   * every matching asset satisfies category-specific structural
//     invariants (sane dimensions, plausible bone counts, etc.)
//
// Adding a new workshop with a previously-unseen version automatically
// generates a new test instance the next time the binary runs — no
// allow-list bumps required, but the new instance must still satisfy
// the invariants below or it fails immediately.

#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "corpus.hpp"

using wallpaper::testing::Corpus;

namespace {

// ----- parameter generators (see header note about static-local refs) ------

const std::vector<std::string>& AllPkgVersions() {
    static const auto v = [] {
        const auto& s = Corpus::instance().pkg_versions();
        return std::vector<std::string>(s.begin(), s.end());
    }();
    return v;
}
const std::vector<int>& AllTexvVersions() {
    static const auto v = [] {
        const auto& s = Corpus::instance().texv_versions();
        return std::vector<int>(s.begin(), s.end());
    }();
    return v;
}
const std::vector<int>& AllTexiVersions() {
    static const auto v = [] {
        const auto& s = Corpus::instance().texi_versions();
        return std::vector<int>(s.begin(), s.end());
    }();
    return v;
}
const std::vector<int>& AllTexbVersions() {
    static const auto v = [] {
        const auto& s = Corpus::instance().texb_versions();
        return std::vector<int>(s.begin(), s.end());
    }();
    return v;
}
const std::vector<int>& AllTexFormats() {
    static const auto v = [] {
        const auto& s = Corpus::instance().tex_formats();
        return std::vector<int>(s.begin(), s.end());
    }();
    return v;
}
const std::vector<int>& AllMdlvVersions() {
    static const auto v = [] {
        const auto& s = Corpus::instance().mdlv_versions();
        return std::vector<int>(s.begin(), s.end());
    }();
    return v;
}
const std::vector<int>& AllMdlsVersions() {
    static const auto v = [] {
        const auto& s = Corpus::instance().mdls_versions();
        return std::vector<int>(s.begin(), s.end());
    }();
    return v;
}
const std::vector<int>& AllMdlaVersions() {
    static const auto v = [] {
        const auto& s = Corpus::instance().mdla_versions();
        return std::vector<int>(s.begin(), s.end());
    }();
    return v;
}

std::string IntName(int v) { return "V" + std::to_string(v); }

} // namespace

// ============================================================================
// scene.pkg version
// ============================================================================

class ScenePkgVersionTest : public ::testing::TestWithParam<std::string> {};

TEST_P(ScenePkgVersionTest, AllWorkshopsParseAndExposeSaneScene) {
    const auto& version = GetParam();
    auto        slice   = Corpus::instance().workshops_with_pkg(version);
    ASSERT_FALSE(slice.empty()) << "no workshops for " << version;

    for (const auto& ref : slice) {
        const auto& w = *ref.workshop;
        SCOPED_TRACE("workshop " + w.id);

        ASSERT_TRUE(w.snapshot.contains("pkg"));
        EXPECT_EQ(w.snapshot["pkg"].value("version", std::string {}), version);
        EXPECT_GT(w.snapshot["pkg"].value("file_count", 0), 0);
        EXPECT_TRUE(w.snapshot["pkg"].value("has_scene_json", false));

        ASSERT_TRUE(w.snapshot.contains("scene"));
        const auto& scene = w.snapshot["scene"];
        EXPECT_TRUE(scene.value("parsed", false))
            << "scene.json failed: " << scene.value("error", std::string {});
        if (scene.value("is_ortho", false)) {
            EXPECT_GT(scene["ortho"].value("width", 0), 0);
            EXPECT_GT(scene["ortho"].value("height", 0), 0);
        }
    }
}

INSTANTIATE_TEST_SUITE_P(
    All, ScenePkgVersionTest, ::testing::ValuesIn(AllPkgVersions()),
    [](const ::testing::TestParamInfo<std::string>& info) { return info.param; });

// ============================================================================
// .tex header version stamps
// ============================================================================

class TextureTexvTest : public ::testing::TestWithParam<int> {};
class TextureTexiTest : public ::testing::TestWithParam<int> {};
class TextureTexbTest : public ::testing::TestWithParam<int> {};
class TextureFormatTest : public ::testing::TestWithParam<int> {};

static void CheckTexInvariants(const Corpus::TexRef& ref) {
    const auto& w = *ref.workshop;
    const auto& t = *ref.tex;
    SCOPED_TRACE("workshop " + w.id + " tex " + t.value("path", std::string {}));
    EXPECT_TRUE(t.value("ok", false));
    EXPECT_GT(t.value("width", 0), 0);
    EXPECT_GT(t.value("height", 0), 0);
    EXPECT_GT(t.value("map_width", 0), 0);
    EXPECT_GT(t.value("map_height", 0), 0);
    EXPECT_GT(t.value("count", 0), 0);
}

TEST_P(TextureTexvTest, AllInstancesParse) {
    auto slice = Corpus::instance().textures_with_texv(GetParam());
    ASSERT_FALSE(slice.empty());
    for (const auto& r : slice) {
        EXPECT_EQ(r.tex->value("texv", -1), GetParam());
        CheckTexInvariants(r);
    }
}
TEST_P(TextureTexiTest, AllInstancesParse) {
    auto slice = Corpus::instance().textures_with_texi(GetParam());
    ASSERT_FALSE(slice.empty());
    for (const auto& r : slice) {
        EXPECT_EQ(r.tex->value("texi", -1), GetParam());
        CheckTexInvariants(r);
    }
}
TEST_P(TextureTexbTest, AllInstancesParse) {
    auto slice = Corpus::instance().textures_with_texb(GetParam());
    ASSERT_FALSE(slice.empty());
    for (const auto& r : slice) {
        EXPECT_EQ(r.tex->value("texb", -1), GetParam());
        CheckTexInvariants(r);
    }
}
TEST_P(TextureFormatTest, AllInstancesParse) {
    auto slice = Corpus::instance().textures_with_format(GetParam());
    ASSERT_FALSE(slice.empty());
    for (const auto& r : slice) {
        EXPECT_EQ(r.tex->value("format", -1), GetParam());
        CheckTexInvariants(r);
    }
}

INSTANTIATE_TEST_SUITE_P(
    All, TextureTexvTest, ::testing::ValuesIn(AllTexvVersions()),
    [](const ::testing::TestParamInfo<int>& i) { return IntName(i.param); });
INSTANTIATE_TEST_SUITE_P(
    All, TextureTexiTest, ::testing::ValuesIn(AllTexiVersions()),
    [](const ::testing::TestParamInfo<int>& i) { return IntName(i.param); });
INSTANTIATE_TEST_SUITE_P(
    All, TextureTexbTest, ::testing::ValuesIn(AllTexbVersions()),
    [](const ::testing::TestParamInfo<int>& i) { return IntName(i.param); });
INSTANTIATE_TEST_SUITE_P(
    All, TextureFormatTest, ::testing::ValuesIn(AllTexFormats()),
    [](const ::testing::TestParamInfo<int>& i) { return "F" + std::to_string(i.param); });

// ============================================================================
// .mdl header version stamps
// ============================================================================

class MdlMdlvTest : public ::testing::TestWithParam<int> {};
class MdlMdlsTest : public ::testing::TestWithParam<int> {};
class MdlMdlaTest : public ::testing::TestWithParam<int> {};

static void CheckMdlInvariants(const Corpus::MdlRef& ref) {
    const auto& w = *ref.workshop;
    const auto& m = *ref.mdl;
    SCOPED_TRACE("workshop " + w.id + " mdl " + m.value("path", std::string {}));
    // Failed parses are tolerated (some .mdl files are non-puppet 3D
    // models that WPMdlParser intentionally rejects), but the version
    // stamps must still be readable.
    EXPECT_GE(m.value("mdlv", -1), 0);
    EXPECT_GE(m.value("mdls", -1), 0);
    EXPECT_GE(m.value("mdla", -1), 0);
    if (m.value("ok", false)) {
        EXPECT_GT(m.value("bones", 0), 0);
    }
}

TEST_P(MdlMdlvTest, AllInstancesExposeStamps) {
    auto slice = Corpus::instance().mdls_with_mdlv(GetParam());
    ASSERT_FALSE(slice.empty());
    for (const auto& r : slice) {
        EXPECT_EQ(r.mdl->value("mdlv", -1), GetParam());
        CheckMdlInvariants(r);
    }
}
TEST_P(MdlMdlsTest, AllInstancesExposeStamps) {
    auto slice = Corpus::instance().mdls_with_mdls(GetParam());
    ASSERT_FALSE(slice.empty());
    for (const auto& r : slice) {
        EXPECT_EQ(r.mdl->value("mdls", -1), GetParam());
        CheckMdlInvariants(r);
    }
}
TEST_P(MdlMdlaTest, AllInstancesExposeStamps) {
    auto slice = Corpus::instance().mdls_with_mdla(GetParam());
    ASSERT_FALSE(slice.empty());
    for (const auto& r : slice) {
        EXPECT_EQ(r.mdl->value("mdla", -1), GetParam());
        CheckMdlInvariants(r);
    }
}

INSTANTIATE_TEST_SUITE_P(
    All, MdlMdlvTest, ::testing::ValuesIn(AllMdlvVersions()),
    [](const ::testing::TestParamInfo<int>& i) { return "Mdlv" + std::to_string(i.param); });
INSTANTIATE_TEST_SUITE_P(
    All, MdlMdlsTest, ::testing::ValuesIn(AllMdlsVersions()),
    [](const ::testing::TestParamInfo<int>& i) { return "Mdls" + std::to_string(i.param); });
INSTANTIATE_TEST_SUITE_P(
    All, MdlMdlaTest, ::testing::ValuesIn(AllMdlaVersions()),
    [](const ::testing::TestParamInfo<int>& i) { return "Mdla" + std::to_string(i.param); });

// ============================================================================
// Smoke: corpus must contain at least one workshop with at least one of
// each major asset class. Catches an empty workshop dir / pathing bug.
// ============================================================================

TEST(CorpusSmoke, NonEmpty) {
    const auto& c = Corpus::instance();
    EXPECT_FALSE(c.entries().empty());
    EXPECT_FALSE(c.pkg_versions().empty());
    EXPECT_FALSE(c.texv_versions().empty());
    EXPECT_FALSE(c.mdlv_versions().empty());
}
