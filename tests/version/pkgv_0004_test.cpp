// Deep test for scene.pkg version PKGV0004.
//
// Auto-seeded by Iter 3. To regenerate after a deliberate parser change:
//     ninja -C build wpdump
//     ./build/tests/wpdump workshop/1303867053 \
//         tests/fixtures/pkgv_0004/1303867053.json

#include "fixture_helpers.hpp"

DEFINE_FIXTURE_TEST_SUITE(
    Pkgv0004Test,
    { "1303867053", WAYWALLEN_FIXTURE_DIR "/pkgv_0004/1303867053.json" });
