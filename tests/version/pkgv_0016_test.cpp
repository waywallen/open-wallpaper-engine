// Deep test for scene.pkg version PKGV0016.
//
// To regenerate after a deliberate parser change:
//     ninja -C build wpdump
//     ./build/tests/wpdump workshop/<id> tests/fixtures/pkgv_0016/<id>.json

#include "fixture_helpers.hpp"

DEFINE_FIXTURE_TEST_SUITE(
    Pkgv0016Test,
    { "2384641783", WAYWALLEN_FIXTURE_DIR "/pkgv_0016/2384641783.json" },
    { "2816746616", WAYWALLEN_FIXTURE_DIR "/pkgv_0016/2816746616.json" });
