// Deep test for scene.pkg version PKGV0022.
//
// To regenerate after a deliberate parser change:
//     ninja -C build wpdump
//     ./build/tests/wpdump workshop/<id> tests/fixtures/pkgv_0022/<id>.json

#include "fixture_helpers.hpp"

DEFINE_FIXTURE_TEST_SUITE(
    Pkgv0022Test,
    { "1425503532", WAYWALLEN_FIXTURE_DIR "/pkgv_0022/1425503532.json" },
    { "2678777830", WAYWALLEN_FIXTURE_DIR "/pkgv_0022/2678777830.json" });
