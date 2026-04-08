// Deep test for scene.pkg version PKGV0017.
//
// To regenerate after a deliberate parser change:
//     ninja -C build wpdump
//     ./build/tests/wpdump workshop/<id> tests/fixtures/pkgv_0017/<id>.json

#include "fixture_helpers.hpp"

DEFINE_FIXTURE_TEST_SUITE(
    Pkgv0017Test,
    { "1864677589", WAYWALLEN_FIXTURE_DIR "/pkgv_0017/1864677589.json" },
    { "2094043344", WAYWALLEN_FIXTURE_DIR "/pkgv_0017/2094043344.json" },
    { "2854083091", WAYWALLEN_FIXTURE_DIR "/pkgv_0017/2854083091.json" });
