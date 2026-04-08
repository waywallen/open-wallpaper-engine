// Deep test for scene.pkg version PKGV0005.
//
// Auto-seeded by Iter 3. To regenerate after a deliberate parser change:
//     ninja -C build wpdump
//     ./build/tests/wpdump workshop/1444077782 \
//         tests/fixtures/pkgv_0005/1444077782.json

#include "fixture_helpers.hpp"

DEFINE_FIXTURE_TEST_SUITE(
    Pkgv0005Test,
    { "1444077782", WAYWALLEN_FIXTURE_DIR "/pkgv_0005/1444077782.json" });
