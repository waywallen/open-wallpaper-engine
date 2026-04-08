// Deep test for scene.pkg version PKGV0003.
//
// Auto-seeded by Iter 3. To regenerate after a deliberate parser change:
//     ninja -C build wpdump
//     ./build/tests/wpdump workshop/1883906261 \
//         tests/fixtures/pkgv_0003/1883906261.json

#include "fixture_helpers.hpp"

DEFINE_FIXTURE_TEST_SUITE(
    Pkgv0003Test,
    { "1883906261", WAYWALLEN_FIXTURE_DIR "/pkgv_0003/1883906261.json" });
