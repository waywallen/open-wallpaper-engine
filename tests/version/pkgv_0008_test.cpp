// Deep test for scene.pkg version PKGV0008.
//
// To regenerate after a deliberate parser change:
//     ninja -C build wpdump
//     ./build/tests/wpdump workshop/<id> tests/fixtures/pkgv_0008/<id>.json

#include "fixture_helpers.hpp"

DEFINE_FIXTURE_TEST_SUITE(
    Pkgv0008Test,
    { "1695353047", WAYWALLEN_FIXTURE_DIR "/pkgv_0008/1695353047.json" },
    { "2190689170", WAYWALLEN_FIXTURE_DIR "/pkgv_0008/2190689170.json" });
