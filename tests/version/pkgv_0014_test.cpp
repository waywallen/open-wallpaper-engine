// Deep test for scene.pkg version PKGV0014.
//
// Auto-seeded by Iter 3. To regenerate after a deliberate parser change:
//     ninja -C build wpdump
//     ./build/tests/wpdump workshop/1721043273 \
//         tests/fixtures/pkgv_0014/1721043273.json

#include "fixture_helpers.hpp"

DEFINE_FIXTURE_TEST_SUITE(
    Pkgv0014Test,
    { "1721043273", WAYWALLEN_FIXTURE_DIR "/pkgv_0014/1721043273.json" });
