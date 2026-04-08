// Deep test for scene.pkg version PKGV0020.
//
// Auto-seeded by Iter 3. To regenerate after a deliberate parser change:
//     ninja -C build wpdump
//     ./build/tests/wpdump workshop/2551699733 \
//         tests/fixtures/pkgv_0020/2551699733.json

#include "fixture_helpers.hpp"

DEFINE_FIXTURE_TEST_SUITE(
    Pkgv0020Test,
    { "2551699733", WAYWALLEN_FIXTURE_DIR "/pkgv_0020/2551699733.json" });
