// Deep test for tex format F4.
//
// Auto-seeded by Iter 6. To regenerate after a deliberate parser change:
//     ninja -C build wpdump
//     ./build/tests/wpdump workshop/1120440003 \
//         tests/fixtures/format_4/1120440003.json

#include "fixture_helpers.hpp"

DEFINE_FIXTURE_TEST_SUITE(
    Format4Test,
    { "1120440003", WAYWALLEN_FIXTURE_DIR "/format_4/1120440003.json" });
