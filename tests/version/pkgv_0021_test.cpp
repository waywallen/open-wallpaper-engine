// Deep test for scene.pkg version PKGV0021.
//
// Auto-seeded by Iter 3. To regenerate after a deliberate parser change:
//     ninja -C build wpdump
//     ./build/tests/wpdump workshop/2084207616 \
//         tests/fixtures/pkgv_0021/2084207616.json

#include "fixture_helpers.hpp"

DEFINE_FIXTURE_TEST_SUITE(
    Pkgv0021Test,
    { "2084207616", WAYWALLEN_FIXTURE_DIR "/pkgv_0021/2084207616.json" });
