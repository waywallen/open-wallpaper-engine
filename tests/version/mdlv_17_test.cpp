// Deep test for mdlv_17.
//
// Auto-seeded by Iter 4/5. To regenerate after a deliberate parser change:
//     ninja -C build wpdump
//     ./build/tests/wpdump workshop/2370640409 \
//         tests/fixtures/mdlv_17/2370640409.json

#include "fixture_helpers.hpp"

DEFINE_FIXTURE_TEST_SUITE(
    Mdlv17Test,
    { "2370640409", WAYWALLEN_FIXTURE_DIR "/mdlv_17/2370640409.json" });
