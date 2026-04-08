// Deep test for scene.pkg version PKGV0001.
//
// To regenerate after a deliberate parser change:
//     ninja -C build wpdump
//     ./build/tests/wpdump workshop/<id> tests/fixtures/pkgv_0001/<id>.json

#include "fixture_helpers.hpp"

DEFINE_FIXTURE_TEST_SUITE(
    Pkgv0001Test,
    { "1120440003", WAYWALLEN_FIXTURE_DIR "/pkgv_0001/1120440003.json" },
    { "960368411", WAYWALLEN_FIXTURE_DIR "/pkgv_0001/960368411.json" });
