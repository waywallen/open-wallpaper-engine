// Deep test for scene.pkg version PKGV0004.
//
// To regenerate after a deliberate parser change:
//     ninja -C build wpdump
//     ./build/tests/wpdump workshop/<id> tests/fixtures/pkgv_0004/<id>.json

#include "fixture_helpers.hpp"

DEFINE_FIXTURE_TEST_SUITE(
    Pkgv0004Test,
    { "1303867053", WAYWALLEN_FIXTURE_DIR "/pkgv_0004/1303867053.json" },
    { "1913713371", WAYWALLEN_FIXTURE_DIR "/pkgv_0004/1913713371.json" });
