// Deep test for scene.pkg version PKGV0022.
//
// Auto-seeded by Iter 3. To regenerate after a deliberate parser change:
//     ninja -C build wpdump
//     ./build/tests/wpdump workshop/1425503532 \
//         tests/fixtures/pkgv_0022/1425503532.json

#include "fixture_helpers.hpp"

DEFINE_FIXTURE_TEST_SUITE(
    Pkgv0022Test,
    { "1425503532", WAYWALLEN_FIXTURE_DIR "/pkgv_0022/1425503532.json" });
