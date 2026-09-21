// Per-version deep tests.
//
// Each category checks a list of (workshop_id, fixture_path) pairs. The test
// re-runs DumpWorkshop and compares the result against a checked-in snapshot.
//
// To regenerate a fixture after a parser change:
//
//   ninja -C build wescene-test
//   build/debug/bin/owe-tests/wescene-test valid workshop/<id> \
//       -o tests/fixtures/<category>/<v>/<id>.json

#include <rstd/test/gtest.hpp>

import rstd.cppstd;
import wescene.json;
import wescene.testing.corpus;
using namespace rstd::literals;

namespace owe::testing
{

struct FixturePick {
    const char* workshop_id;
    const char* fixture_path;
};

rstd::Option<std::string> FirstDifference(const Json& expected, const Json& actual,
                                          std::string path = "$") {
    if (expected == actual) return rstd::None();
    if (auto expected_values = expected.as_array(); expected_values.is_some()) {
        auto actual_values = actual.as_array();
        if (actual_values.is_none() || (*expected_values)->len() != (*actual_values)->len())
            return rstd::Some(rstd::move(path));
        for (rstd::usize i; i < (*expected_values)->len(); ++i) {
            if (auto difference =
                    FirstDifference((**expected_values)[i],
                                    (**actual_values)[i],
                                    path + "[" + std::to_string(i.to_primitive()) + "]"))
                return difference;
        }
        return rstd::Some(rstd::move(path));
    }
    if (expected.is_object() && actual.is_object()) {
        rstd::Option<std::string> difference;
        auto                      expected_object = expected.as_object();
        (*expected_object)->iter().for_each([&](auto entry) {
            auto [entry_key, entry_value] = entry;
            if (difference) return;
            const auto& expected_value = *entry_value;
            auto        actual_value   = actual.get(entry_key->as_str());
            if (actual_value.is_none()) {
                difference = rstd::Some(path + "." + rstd::cppstd::to_string(entry_key->as_str()));
                return;
            }
            difference = FirstDifference(expected_value,
                                         **actual_value,
                                         path + "." + rstd::cppstd::to_string(entry_key->as_str()));
        });
        if (difference) return difference;
        auto actual_object = actual.as_object();
        (*actual_object)->iter().for_each([&](auto entry) {
            auto [entry_key, entry_value] = entry;
            if (! difference && expected.get(entry_key->as_str()).is_none())
                difference = rstd::Some(path + "." + rstd::cppstd::to_string(entry_key->as_str()));
        });
        if (difference.is_some()) return difference;
        return rstd::Some(rstd::move(path));
    }
    return rstd::Some(rstd::move(path));
}

inline void VerifyFixture(const FixturePick& pick) {
    SCOPED_TRACE(std::string("workshop=") + pick.workshop_id + " fixture=" + pick.fixture_path);

    const std::string workshop_dir = std::string(WAYWALLEN_WORKSHOP_DIR) + "/" + pick.workshop_id;

    std::string err;
    auto        actual = DumpWorkshop(workshop_dir, err);
    ASSERT_TRUE(err.empty()) << "DumpWorkshop failed: " << err;

    std::ifstream in(pick.fixture_path);
    ASSERT_TRUE(in.good()) << "cannot open fixture " << pick.fixture_path;
    std::stringstream source;
    source << in.rdbuf();
    auto parsed = ParseJson(rstd::cppstd::as_str(source.str()).unwrap());
    ASSERT_TRUE(parsed.is_ok()) << "fixture is not valid JSON: " << pick.fixture_path;
    auto expected = parsed.unwrap();

    if (actual == expected) return;

    auto difference = FirstDifference(expected, actual);
    ADD_FAILURE() << "snapshot drift for workshop " << pick.workshop_id << "\n"
                  << "fixture:  " << pick.fixture_path << "\n"
                  << "first unequal path: "
                  << (difference.is_some() ? *difference : std::string("$")) << "\n";
}

} // namespace owe::testing

#define DEFINE_FIXTURE_TEST_SUITE(SuiteName, ...)                                         \
    static const ::owe::testing::FixturePick k##SuiteName##Picks[] = { __VA_ARGS__ };     \
    TEST(SuiteName, MatchesFixtures) {                                                    \
        for (const auto& pick : k##SuiteName##Picks) ::owe::testing::VerifyFixture(pick); \
    }

DEFINE_FIXTURE_TEST_SUITE(Format0Test,
                          { "2281052567", WAYWALLEN_FIXTURE_DIR "/format_0/2281052567.json" });

DEFINE_FIXTURE_TEST_SUITE(Format4Test,
                          { "1120440003", WAYWALLEN_FIXTURE_DIR "/format_4/1120440003.json" });

DEFINE_FIXTURE_TEST_SUITE(Format6Test,
                          { "1721043273", WAYWALLEN_FIXTURE_DIR "/format_6/1721043273.json" });

DEFINE_FIXTURE_TEST_SUITE(Mdlv4Test,
                          { "1548688862", WAYWALLEN_FIXTURE_DIR "/mdlv_4/1548688862.json" });

DEFINE_FIXTURE_TEST_SUITE(Mdlv13Test,
                          { "2186130002", WAYWALLEN_FIXTURE_DIR "/mdlv_13/2186130002.json" });

DEFINE_FIXTURE_TEST_SUITE(Mdlv14Test,
                          { "2835012244", WAYWALLEN_FIXTURE_DIR "/mdlv_14/2835012244.json" });

DEFINE_FIXTURE_TEST_SUITE(Mdlv16Test,
                          { "2664591394", WAYWALLEN_FIXTURE_DIR "/mdlv_16/2664591394.json" });

DEFINE_FIXTURE_TEST_SUITE(Mdlv17Test,
                          { "2370640409", WAYWALLEN_FIXTURE_DIR "/mdlv_17/2370640409.json" });

DEFINE_FIXTURE_TEST_SUITE(Mdlv21Test,
                          { "3400879974", WAYWALLEN_FIXTURE_DIR "/mdlv_21/3400879974.json" });

DEFINE_FIXTURE_TEST_SUITE(Pkgv0001Test,
                          { "1120440003", WAYWALLEN_FIXTURE_DIR "/pkgv_0001/1120440003.json" },
                          { "960368411", WAYWALLEN_FIXTURE_DIR "/pkgv_0001/960368411.json" });

DEFINE_FIXTURE_TEST_SUITE(Pkgv0002Test,
                          { "1139304621", WAYWALLEN_FIXTURE_DIR "/pkgv_0002/1139304621.json" },
                          { "1845706469", WAYWALLEN_FIXTURE_DIR "/pkgv_0002/1845706469.json" });

DEFINE_FIXTURE_TEST_SUITE(Pkgv0003Test,
                          { "1883906261", WAYWALLEN_FIXTURE_DIR "/pkgv_0003/1883906261.json" });

DEFINE_FIXTURE_TEST_SUITE(Pkgv0004Test,
                          { "1303867053", WAYWALLEN_FIXTURE_DIR "/pkgv_0004/1303867053.json" },
                          { "1913713371", WAYWALLEN_FIXTURE_DIR "/pkgv_0004/1913713371.json" });

DEFINE_FIXTURE_TEST_SUITE(Pkgv0005Test,
                          { "1444077782", WAYWALLEN_FIXTURE_DIR "/pkgv_0005/1444077782.json" },
                          { "1993699454", WAYWALLEN_FIXTURE_DIR "/pkgv_0005/1993699454.json" });

DEFINE_FIXTURE_TEST_SUITE(Pkgv0006Test,
                          { "1583040279", WAYWALLEN_FIXTURE_DIR "/pkgv_0006/1583040279.json" },
                          { "2092117131", WAYWALLEN_FIXTURE_DIR "/pkgv_0006/2092117131.json" });

DEFINE_FIXTURE_TEST_SUITE(Pkgv0007Test,
                          { "1929493662", WAYWALLEN_FIXTURE_DIR "/pkgv_0007/1929493662.json" },
                          { "2149140853", WAYWALLEN_FIXTURE_DIR "/pkgv_0007/2149140853.json" });

DEFINE_FIXTURE_TEST_SUITE(Pkgv0008Test,
                          { "1695353047", WAYWALLEN_FIXTURE_DIR "/pkgv_0008/1695353047.json" },
                          { "2190689170", WAYWALLEN_FIXTURE_DIR "/pkgv_0008/2190689170.json" });

DEFINE_FIXTURE_TEST_SUITE(Pkgv0009Test,
                          { "1686808824", WAYWALLEN_FIXTURE_DIR "/pkgv_0009/1686808824.json" },
                          { "2311201876", WAYWALLEN_FIXTURE_DIR "/pkgv_0009/2311201876.json" });

DEFINE_FIXTURE_TEST_SUITE(Pkgv0010Test,
                          { "2004893596", WAYWALLEN_FIXTURE_DIR "/pkgv_0010/2004893596.json" },
                          { "2373012879", WAYWALLEN_FIXTURE_DIR "/pkgv_0010/2373012879.json" });

DEFINE_FIXTURE_TEST_SUITE(Pkgv0011Test,
                          { "2249672558", WAYWALLEN_FIXTURE_DIR "/pkgv_0011/2249672558.json" },
                          { "2387022483", WAYWALLEN_FIXTURE_DIR "/pkgv_0011/2387022483.json" });

DEFINE_FIXTURE_TEST_SUITE(Pkgv0012Test,
                          { "1569133561", WAYWALLEN_FIXTURE_DIR "/pkgv_0012/1569133561.json" },
                          { "2513410552", WAYWALLEN_FIXTURE_DIR "/pkgv_0012/2513410552.json" });

DEFINE_FIXTURE_TEST_SUITE(Pkgv0013Test,
                          { "2000029791", WAYWALLEN_FIXTURE_DIR "/pkgv_0013/2000029791.json" },
                          { "2606180040", WAYWALLEN_FIXTURE_DIR "/pkgv_0013/2606180040.json" });

DEFINE_FIXTURE_TEST_SUITE(Pkgv0014Test,
                          { "1721043273", WAYWALLEN_FIXTURE_DIR "/pkgv_0014/1721043273.json" },
                          { "2662089955", WAYWALLEN_FIXTURE_DIR "/pkgv_0014/2662089955.json" });

DEFINE_FIXTURE_TEST_SUITE(Pkgv0015Test,
                          { "1339064732", WAYWALLEN_FIXTURE_DIR "/pkgv_0015/1339064732.json" },
                          { "2740023533", WAYWALLEN_FIXTURE_DIR "/pkgv_0015/2740023533.json" });

DEFINE_FIXTURE_TEST_SUITE(Pkgv0016Test,
                          { "2384641783", WAYWALLEN_FIXTURE_DIR "/pkgv_0016/2384641783.json" },
                          { "2816746616", WAYWALLEN_FIXTURE_DIR "/pkgv_0016/2816746616.json" });

DEFINE_FIXTURE_TEST_SUITE(Pkgv0017Test,
                          { "1864677589", WAYWALLEN_FIXTURE_DIR "/pkgv_0017/1864677589.json" },
                          { "2094043344", WAYWALLEN_FIXTURE_DIR "/pkgv_0017/2094043344.json" },
                          { "2854083091", WAYWALLEN_FIXTURE_DIR "/pkgv_0017/2854083091.json" });

DEFINE_FIXTURE_TEST_SUITE(Pkgv0018Test,
                          { "2164394410", WAYWALLEN_FIXTURE_DIR "/pkgv_0018/2164394410.json" },
                          { "2854277071", WAYWALLEN_FIXTURE_DIR "/pkgv_0018/2854277071.json" });

DEFINE_FIXTURE_TEST_SUITE(Pkgv0019Test,
                          { "1851524277", WAYWALLEN_FIXTURE_DIR "/pkgv_0019/1851524277.json" },
                          { "2751564702", WAYWALLEN_FIXTURE_DIR "/pkgv_0019/2751564702.json" });

DEFINE_FIXTURE_TEST_SUITE(Pkgv0020Test,
                          { "2551699733", WAYWALLEN_FIXTURE_DIR "/pkgv_0020/2551699733.json" });

DEFINE_FIXTURE_TEST_SUITE(Pkgv0021Test,
                          { "2084207616", WAYWALLEN_FIXTURE_DIR "/pkgv_0021/2084207616.json" },
                          { "2699039775", WAYWALLEN_FIXTURE_DIR "/pkgv_0021/2699039775.json" });

DEFINE_FIXTURE_TEST_SUITE(Pkgv0022Test,
                          { "1425503532", WAYWALLEN_FIXTURE_DIR "/pkgv_0022/1425503532.json" },
                          { "2678777830", WAYWALLEN_FIXTURE_DIR "/pkgv_0022/2678777830.json" });

DEFINE_FIXTURE_TEST_SUITE(Pkgv0023Test,
                          { "2135858259", WAYWALLEN_FIXTURE_DIR "/pkgv_0023/2135858259.json" });

DEFINE_FIXTURE_TEST_SUITE(Texb1Test,
                          { "820654165", WAYWALLEN_FIXTURE_DIR "/texb_1/820654165.json" });

DEFINE_FIXTURE_TEST_SUITE(Texb2Test,
                          { "1120440003", WAYWALLEN_FIXTURE_DIR "/texb_2/1120440003.json" });

DEFINE_FIXTURE_TEST_SUITE(Texb3Test,
                          { "1197607981", WAYWALLEN_FIXTURE_DIR "/texb_3/1197607981.json" });

DEFINE_FIXTURE_TEST_SUITE(Texb4Test,
                          { "2370927443", WAYWALLEN_FIXTURE_DIR "/texb_4/2370927443.json" });
