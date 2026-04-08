// Deep test for scene.pkg version PKGV0011.
//
// To regenerate after a deliberate parser change:
//     ninja -C build wpdump
//     ./build/tests/wpdump workshop/<id> tests/fixtures/pkgv_0011/<id>.json

#include "fixture_helpers.hpp"

DEFINE_FIXTURE_TEST_SUITE(
    Pkgv0011Test,
    { "2249672558", WAYWALLEN_FIXTURE_DIR "/pkgv_0011/2249672558.json" },
    { "2387022483", WAYWALLEN_FIXTURE_DIR "/pkgv_0011/2387022483.json" });
