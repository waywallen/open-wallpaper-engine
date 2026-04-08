// Deep test for texb_1.
//
// Auto-seeded by Iter 4/5. To regenerate after a deliberate parser change:
//     ninja -C build wpdump
//     ./build/tests/wpdump workshop/820654165 \
//         tests/fixtures/texb_1/820654165.json

#include "fixture_helpers.hpp"

DEFINE_FIXTURE_TEST_SUITE(
    Texb1Test,
    { "820654165", WAYWALLEN_FIXTURE_DIR "/texb_1/820654165.json" });
