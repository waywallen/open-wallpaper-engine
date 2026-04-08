// Deep test for mdlv_4.
//
// Auto-seeded by Iter 4/5. To regenerate after a deliberate parser change:
//     ninja -C build wpdump
//     ./build/tests/wpdump workshop/1548688862 \
//         tests/fixtures/mdlv_4/1548688862.json

#include "fixture_helpers.hpp"

DEFINE_FIXTURE_TEST_SUITE(
    Mdlv4Test,
    { "1548688862", WAYWALLEN_FIXTURE_DIR "/mdlv_4/1548688862.json" });
