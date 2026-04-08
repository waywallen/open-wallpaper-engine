// Deep test for scene.pkg version PKGV0023.
//
// To regenerate after a deliberate parser change:
//     ninja -C build wpdump
//     ./build/tests/wpdump workshop/<id> tests/fixtures/pkgv_0023/<id>.json

#include "fixture_helpers.hpp"

DEFINE_FIXTURE_TEST_SUITE(
    Pkgv0023Test,
    { "2135858259", WAYWALLEN_FIXTURE_DIR "/pkgv_0023/2135858259.json" });
