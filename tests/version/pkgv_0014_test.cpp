// Deep test for scene.pkg version PKGV0014.
//
// To regenerate after a deliberate parser change:
//     ninja -C build wpdump
//     ./build/tests/wpdump workshop/<id> tests/fixtures/pkgv_0014/<id>.json

#include "fixture_helpers.hpp"

DEFINE_FIXTURE_TEST_SUITE(
    Pkgv0014Test,
    { "1721043273", WAYWALLEN_FIXTURE_DIR "/pkgv_0014/1721043273.json" },
    { "2662089955", WAYWALLEN_FIXTURE_DIR "/pkgv_0014/2662089955.json" });
