// Deep test for texb_3.
//
// Auto-seeded by Iter 4/5. To regenerate after a deliberate parser change:
//     ninja -C build wpdump
//     ./build/tests/wpdump workshop/1197607981 \
//         tests/fixtures/texb_3/1197607981.json

#include "fixture_helpers.hpp"

DEFINE_FIXTURE_TEST_SUITE(
    Texb3Test,
    { "1197607981", WAYWALLEN_FIXTURE_DIR "/texb_3/1197607981.json" });
