// Deep test for scene.pkg version PKGV0017.
//
// Picks two representative workshop entries with different feature
// surfaces and pins their full DumpWorkshop snapshot against a
// checked-in JSON fixture in tests/fixtures/pkgv_0017/. Any drift in
// the parsed pkg/scene/texture/puppet metadata fails this test with
// a JSON-Patch diff pointing at the offending field.
//
// To regenerate after a deliberate parser change:
//     ninja -C build wpdump
//     ./build/tests/wpdump workshop/<id> \
//         tests/fixtures/pkgv_0017/<id>.json

#include "fixture_helpers.hpp"

DEFINE_FIXTURE_TEST_SUITE(
    Pkgv0017Test,
    { "1864677589", WAYWALLEN_FIXTURE_DIR "/pkgv_0017/1864677589.json" },
    { "2094043344", WAYWALLEN_FIXTURE_DIR "/pkgv_0017/2094043344.json" });
