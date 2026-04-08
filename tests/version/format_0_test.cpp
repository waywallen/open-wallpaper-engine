// Deep test for tex format F0.
//
// Auto-seeded by Iter 6. To regenerate after a deliberate parser change:
//     ninja -C build wpdump
//     ./build/tests/wpdump workshop/2281052567 \
//         tests/fixtures/format_0/2281052567.json

#include "fixture_helpers.hpp"

DEFINE_FIXTURE_TEST_SUITE(
    Format0Test,
    { "2281052567", WAYWALLEN_FIXTURE_DIR "/format_0/2281052567.json" });
