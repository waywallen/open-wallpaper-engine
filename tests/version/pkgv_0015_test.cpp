// Deep test for scene.pkg version PKGV0015.
//
// Auto-seeded by Iter 3. To regenerate after a deliberate parser change:
//     ninja -C build wpdump
//     ./build/tests/wpdump workshop/1339064732 \
//         tests/fixtures/pkgv_0015/1339064732.json

#include "fixture_helpers.hpp"

DEFINE_FIXTURE_TEST_SUITE(
    Pkgv0015Test,
    { "1339064732", WAYWALLEN_FIXTURE_DIR "/pkgv_0015/1339064732.json" });
