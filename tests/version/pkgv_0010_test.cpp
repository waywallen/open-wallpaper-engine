// Deep test for scene.pkg version PKGV0010.
//
// To regenerate after a deliberate parser change:
//     ninja -C build wpdump
//     ./build/tests/wpdump workshop/<id> tests/fixtures/pkgv_0010/<id>.json

#include "fixture_helpers.hpp"

DEFINE_FIXTURE_TEST_SUITE(
    Pkgv0010Test,
    { "2004893596", WAYWALLEN_FIXTURE_DIR "/pkgv_0010/2004893596.json" },
    { "2373012879", WAYWALLEN_FIXTURE_DIR "/pkgv_0010/2373012879.json" });
