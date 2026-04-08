// Deep test for scene.pkg version PKGV0005.
//
// To regenerate after a deliberate parser change:
//     ninja -C build wpdump
//     ./build/tests/wpdump workshop/<id> tests/fixtures/pkgv_0005/<id>.json

#include "fixture_helpers.hpp"

DEFINE_FIXTURE_TEST_SUITE(
    Pkgv0005Test,
    { "1444077782", WAYWALLEN_FIXTURE_DIR "/pkgv_0005/1444077782.json" },
    { "1993699454", WAYWALLEN_FIXTURE_DIR "/pkgv_0005/1993699454.json" });
