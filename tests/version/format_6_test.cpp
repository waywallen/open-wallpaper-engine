// Deep test for tex format F6.
//
// Auto-seeded by Iter 6. To regenerate after a deliberate parser change:
//     ninja -C build wpdump
//     ./build/tests/wpdump workshop/1721043273 \
//         tests/fixtures/format_6/1721043273.json

#include "fixture_helpers.hpp"

DEFINE_FIXTURE_TEST_SUITE(
    Format6Test,
    { "1721043273", WAYWALLEN_FIXTURE_DIR "/format_6/1721043273.json" });
