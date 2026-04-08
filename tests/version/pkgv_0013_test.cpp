// Deep test for scene.pkg version PKGV0013.
//
// To regenerate after a deliberate parser change:
//     ninja -C build wpdump
//     ./build/tests/wpdump workshop/<id> tests/fixtures/pkgv_0013/<id>.json

#include "fixture_helpers.hpp"

DEFINE_FIXTURE_TEST_SUITE(
    Pkgv0013Test,
    { "2000029791", WAYWALLEN_FIXTURE_DIR "/pkgv_0013/2000029791.json" },
    { "2606180040", WAYWALLEN_FIXTURE_DIR "/pkgv_0013/2606180040.json" });
