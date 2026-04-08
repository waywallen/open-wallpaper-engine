// Deep test for scene.pkg version PKGV0010.
//
// Auto-seeded by Iter 3. To regenerate after a deliberate parser change:
//     ninja -C build wpdump
//     ./build/tests/wpdump workshop/2004893596 \
//         tests/fixtures/pkgv_0010/2004893596.json

#include "fixture_helpers.hpp"

DEFINE_FIXTURE_TEST_SUITE(
    Pkgv0010Test,
    { "2004893596", WAYWALLEN_FIXTURE_DIR "/pkgv_0010/2004893596.json" });
