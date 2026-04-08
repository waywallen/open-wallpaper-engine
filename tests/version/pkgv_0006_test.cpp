// Deep test for scene.pkg version PKGV0006.
//
// Auto-seeded by Iter 3. To regenerate after a deliberate parser change:
//     ninja -C build wpdump
//     ./build/tests/wpdump workshop/1583040279 \
//         tests/fixtures/pkgv_0006/1583040279.json

#include "fixture_helpers.hpp"

DEFINE_FIXTURE_TEST_SUITE(
    Pkgv0006Test,
    { "1583040279", WAYWALLEN_FIXTURE_DIR "/pkgv_0006/1583040279.json" });
