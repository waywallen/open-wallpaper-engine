// Deep test for scene.pkg version PKGV0019.
//
// Auto-seeded by Iter 3. To regenerate after a deliberate parser change:
//     ninja -C build wpdump
//     ./build/tests/wpdump workshop/1851524277 \
//         tests/fixtures/pkgv_0019/1851524277.json

#include "fixture_helpers.hpp"

DEFINE_FIXTURE_TEST_SUITE(
    Pkgv0019Test,
    { "1851524277", WAYWALLEN_FIXTURE_DIR "/pkgv_0019/1851524277.json" });
