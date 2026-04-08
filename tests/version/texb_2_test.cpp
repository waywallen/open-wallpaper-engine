// Deep test for texb_2.
//
// Auto-seeded by Iter 4/5. To regenerate after a deliberate parser change:
//     ninja -C build wpdump
//     ./build/tests/wpdump workshop/1120440003 \
//         tests/fixtures/texb_2/1120440003.json

#include "fixture_helpers.hpp"

DEFINE_FIXTURE_TEST_SUITE(
    Texb2Test,
    { "1120440003", WAYWALLEN_FIXTURE_DIR "/texb_2/1120440003.json" });
