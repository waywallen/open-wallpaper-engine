// Deep test for scene.pkg version PKGV0012.
//
// To regenerate after a deliberate parser change:
//     ninja -C build wpdump
//     ./build/tests/wpdump workshop/<id> tests/fixtures/pkgv_0012/<id>.json

#include "fixture_helpers.hpp"

DEFINE_FIXTURE_TEST_SUITE(
    Pkgv0012Test,
    { "1569133561", WAYWALLEN_FIXTURE_DIR "/pkgv_0012/1569133561.json" },
    { "2513410552", WAYWALLEN_FIXTURE_DIR "/pkgv_0012/2513410552.json" });
