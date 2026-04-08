// Deep test for mdlv_13.
//
// Auto-seeded by Iter 4/5. To regenerate after a deliberate parser change:
//     ninja -C build wpdump
//     ./build/tests/wpdump workshop/2186130002 \
//         tests/fixtures/mdlv_13/2186130002.json

#include "fixture_helpers.hpp"

DEFINE_FIXTURE_TEST_SUITE(
    Mdlv13Test,
    { "2186130002", WAYWALLEN_FIXTURE_DIR "/mdlv_13/2186130002.json" });
