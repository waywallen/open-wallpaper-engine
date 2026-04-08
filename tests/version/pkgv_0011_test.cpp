// Deep test for scene.pkg version PKGV0011.
//
// Auto-seeded by Iter 3. To regenerate after a deliberate parser change:
//     ninja -C build wpdump
//     ./build/tests/wpdump workshop/2249672558 \
//         tests/fixtures/pkgv_0011/2249672558.json

#include "fixture_helpers.hpp"

DEFINE_FIXTURE_TEST_SUITE(
    Pkgv0011Test,
    { "2249672558", WAYWALLEN_FIXTURE_DIR "/pkgv_0011/2249672558.json" });
