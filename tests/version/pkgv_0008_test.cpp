// Deep test for scene.pkg version PKGV0008.
//
// Auto-seeded by Iter 3. To regenerate after a deliberate parser change:
//     ninja -C build wpdump
//     ./build/tests/wpdump workshop/1695353047 \
//         tests/fixtures/pkgv_0008/1695353047.json

#include "fixture_helpers.hpp"

DEFINE_FIXTURE_TEST_SUITE(
    Pkgv0008Test,
    { "1695353047", WAYWALLEN_FIXTURE_DIR "/pkgv_0008/1695353047.json" });
