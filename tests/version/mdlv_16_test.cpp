// Deep test for mdlv_16.
//
// Auto-seeded by Iter 4/5. To regenerate after a deliberate parser change:
//     ninja -C build wpdump
//     ./build/tests/wpdump workshop/2664591394 \
//         tests/fixtures/mdlv_16/2664591394.json

#include "fixture_helpers.hpp"

DEFINE_FIXTURE_TEST_SUITE(
    Mdlv16Test,
    { "2664591394", WAYWALLEN_FIXTURE_DIR "/mdlv_16/2664591394.json" });
