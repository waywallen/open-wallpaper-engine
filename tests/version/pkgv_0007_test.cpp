// Deep test for scene.pkg version PKGV0007.
//
// To regenerate after a deliberate parser change:
//     ninja -C build wpdump
//     ./build/tests/wpdump workshop/<id> tests/fixtures/pkgv_0007/<id>.json

#include "fixture_helpers.hpp"

DEFINE_FIXTURE_TEST_SUITE(
    Pkgv0007Test,
    { "1929493662", WAYWALLEN_FIXTURE_DIR "/pkgv_0007/1929493662.json" },
    { "2149140853", WAYWALLEN_FIXTURE_DIR "/pkgv_0007/2149140853.json" });
