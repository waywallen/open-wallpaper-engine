// Deep test for scene.pkg version PKGV0001.
//
// Auto-seeded by Iter 3. To regenerate after a deliberate parser change:
//     ninja -C build wpdump
//     ./build/tests/wpdump workshop/1120440003 \
//         tests/fixtures/pkgv_0001/1120440003.json

#include "fixture_helpers.hpp"

DEFINE_FIXTURE_TEST_SUITE(
    Pkgv0001Test,
    { "1120440003", WAYWALLEN_FIXTURE_DIR "/pkgv_0001/1120440003.json" });
