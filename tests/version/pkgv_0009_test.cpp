// Deep test for scene.pkg version PKGV0009.
//
// To regenerate after a deliberate parser change:
//     ninja -C build wpdump
//     ./build/tests/wpdump workshop/<id> tests/fixtures/pkgv_0009/<id>.json

#include "fixture_helpers.hpp"

DEFINE_FIXTURE_TEST_SUITE(
    Pkgv0009Test,
    { "1686808824", WAYWALLEN_FIXTURE_DIR "/pkgv_0009/1686808824.json" },
    { "2311201876", WAYWALLEN_FIXTURE_DIR "/pkgv_0009/2311201876.json" });
