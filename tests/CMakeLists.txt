# Test suite for wescene-renderer.
#
# Strategy: a single gtest binary indexes workshop/* once via DumpWorkshop
# (no on-disk fixtures) and uses TEST_P fixtures to register one test
# instance per (category, version) pair observed in the corpus. Each
# instance asserts category-specific structural invariants on the slice
# of assets matching its parameter.
#
# Adding a new workshop with a previously-unseen pkg/tex/mdl version
# automatically grows the test matrix on the next run — no allow-list
# bumps required, but the new instance must still satisfy the invariants
# in version_tests.cpp or it fails immediately.

include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
    GIT_SHALLOW    TRUE
)
set(BUILD_GMOCK OFF CACHE INTERNAL "")
set(INSTALL_GTEST OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(googletest)

# Shared library for the dumper + corpus index. Used by the gtest binary
# and any future ad-hoc tools.
add_library(wpdump_lib STATIC dump.cpp corpus.cpp)
target_link_libraries(
    wpdump_lib
    PUBLIC
        ${PROJECT_NAME}
        wpFs
        wpScene
        nlohmann_json::nlohmann_json
)
target_include_directories(wpdump_lib PUBLIC .)
target_compile_features(wpdump_lib PUBLIC cxx_std_20)
target_compile_definitions(
    wpdump_lib PUBLIC
    "WAYWALLEN_WORKSHOP_DIR=\"${CMAKE_SOURCE_DIR}/workshop\""
    "WAYWALLEN_FIXTURE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/fixtures\"")

add_executable(version_tests version_tests.cpp)
target_link_libraries(version_tests PRIVATE wpdump_lib GTest::gtest_main)

# CLI for fixture authoring: dumps a single workshop to a JSON file.
add_executable(wpdump wpdump_main.cpp)
target_link_libraries(wpdump PRIVATE wpdump_lib)

# Per-version deep tests live in tests/version/<category>_<version>_test.cpp.
# Each is a tiny gtest TU built into the same binary so the corpus + gtest
# fixture machinery is amortised across them.
file(GLOB version_test_srcs CONFIGURE_DEPENDS "version/*_test.cpp")
if(version_test_srcs)
    add_executable(deep_version_tests ${version_test_srcs})
    target_link_libraries(deep_version_tests PRIVATE wpdump_lib GTest::gtest_main)
    target_include_directories(deep_version_tests PRIVATE .)
endif()

# Use gtest_discover_tests so each TEST_P instance becomes its own ctest
# entry, runnable individually with `ctest -R ScenePkgVersionTest`.
include(GoogleTest)
gtest_discover_tests(
    version_tests
    PROPERTIES TIMEOUT 30
    DISCOVERY_TIMEOUT 60
)
if(TARGET deep_version_tests)
    gtest_discover_tests(
        deep_version_tests
        PROPERTIES TIMEOUT 30
        DISCOVERY_TIMEOUT 60
    )
endif()
