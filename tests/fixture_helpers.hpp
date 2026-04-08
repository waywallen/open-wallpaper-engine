// Fixture helpers used by per-version test files.
//
// Each `tests/version/<category>_<version>_test.cpp` is a tiny gtest
// translation unit that registers one TEST_P fixture, parameterised on a
// list of (workshop_id, fixture_path) pairs. The TEST_P body just calls
// VerifyFixture(workshop_id, fixture_path), which:
//
//   1. Resolves WAYWALLEN_WORKSHOP_DIR + workshop_id to a real path.
//   2. Re-runs DumpWorkshop on it.
//   3. Loads the checked-in fixture json from disk.
//   4. EXPECT_EQs the two json values, falling back to a JSON Patch
//      diff (RFC 6902) for a precise field-level error report.
//
// To regenerate a fixture after a parser change, build wpdump and rerun
// it for the workshop in question:
//
//     ninja -C build wpdump
//     ./build/tests/wpdump workshop/<id> tests/fixtures/<category>/<v>/<id>.json
//
// The DEFINE_FIXTURE_TEST_SUITE macro keeps the per-version cpp files
// down to a single declaration each.

#pragma once

#include <fstream>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "dump.hpp"

namespace wallpaper::testing {

struct FixturePick {
    const char* workshop_id;
    const char* fixture_path;
};

inline void VerifyFixture(const FixturePick& pick) {
    SCOPED_TRACE(std::string("workshop=") + pick.workshop_id +
                 " fixture=" + pick.fixture_path);

    const std::string workshop_dir =
        std::string(WAYWALLEN_WORKSHOP_DIR) + "/" + pick.workshop_id;

    std::string err;
    auto        actual = DumpWorkshop(workshop_dir, err);
    ASSERT_TRUE(err.empty()) << "DumpWorkshop failed: " << err;

    std::ifstream in(pick.fixture_path);
    ASSERT_TRUE(in.good()) << "cannot open fixture " << pick.fixture_path;
    nlohmann::json expected;
    ASSERT_NO_THROW(in >> expected) << "fixture is not valid JSON: " << pick.fixture_path;

    if (actual == expected) return;

    auto patch = nlohmann::json::diff(expected, actual);
    ADD_FAILURE() << "snapshot drift for workshop " << pick.workshop_id << "\n"
                  << "fixture:  " << pick.fixture_path << "\n"
                  << "diff (expected -> actual):\n"
                  << patch.dump(2);
}

} // namespace wallpaper::testing

// One macro hides the gtest fixture boilerplate so a per-version test
// file is just two lines of declaration plus the picks list.
#define DEFINE_FIXTURE_TEST_SUITE(SuiteName, ...)                                  \
    class SuiteName : public ::testing::TestWithParam<                              \
                          ::wallpaper::testing::FixturePick> {};                    \
    TEST_P(SuiteName, MatchesFixture) {                                             \
        ::wallpaper::testing::VerifyFixture(GetParam());                            \
    }                                                                                \
    static const ::wallpaper::testing::FixturePick k##SuiteName##Picks[] = { __VA_ARGS__ }; \
    INSTANTIATE_TEST_SUITE_P(                                                       \
        All, SuiteName, ::testing::ValuesIn(k##SuiteName##Picks),                   \
        [](const ::testing::TestParamInfo<::wallpaper::testing::FixturePick>& i) {  \
            return std::string(i.param.workshop_id);                                \
        })
