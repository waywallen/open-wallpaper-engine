// Deep test for scene.pkg version PKGV0002.
//
// Auto-seeded by Iter 3. To regenerate after a deliberate parser change:
//     ninja -C build wpdump
//     ./build/tests/wpdump workshop/1139304621 \
//         tests/fixtures/pkgv_0002/1139304621.json

#include "fixture_helpers.hpp"

DEFINE_FIXTURE_TEST_SUITE(
    Pkgv0002Test,
    { "1139304621", WAYWALLEN_FIXTURE_DIR "/pkgv_0002/1139304621.json" });
