// Deep test for scene.pkg version PKGV0018.
//
// To regenerate after a deliberate parser change:
//     ninja -C build wpdump
//     ./build/tests/wpdump workshop/<id> tests/fixtures/pkgv_0018/<id>.json

#include "fixture_helpers.hpp"

DEFINE_FIXTURE_TEST_SUITE(
    Pkgv0018Test,
    { "2164394410", WAYWALLEN_FIXTURE_DIR "/pkgv_0018/2164394410.json" },
    { "2854277071", WAYWALLEN_FIXTURE_DIR "/pkgv_0018/2854277071.json" });
