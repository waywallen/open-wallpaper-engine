include(FetchContent)
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)

# ------------------------------------------------------------------
# Locally vendored deps still living under third_party/
# (glad, argparse, effolkronium, vog, stb_image.h, vk_mem_alloc.h)
# ------------------------------------------------------------------
add_subdirectory(glad)

# ------------------------------------------------------------------
# Eigen (header only)
# ------------------------------------------------------------------
FetchContent_Declare(
    eigen
    GIT_REPOSITORY https://gitlab.com/libeigen/eigen.git
    GIT_TAG        3.4.0
    GIT_SHALLOW    TRUE
    # Skip add_subdirectory: we only need Eigen's headers and the full
    # Eigen CMake project brings in a lot of unrelated targets.
    SOURCE_SUBDIR  cmake-noop
)
FetchContent_MakeAvailable(eigen)
add_library(eigen INTERFACE)
add_library(Eigen3::Eigen ALIAS eigen)
target_include_directories(eigen SYSTEM INTERFACE ${eigen_SOURCE_DIR})

# ------------------------------------------------------------------
# nlohmann_json
# ------------------------------------------------------------------
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install    OFF CACHE INTERNAL "")
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# ------------------------------------------------------------------
# SPIRV-Reflect (produces target `spirv-reflect-static`)
# ------------------------------------------------------------------
set(SPIRV_REFLECT_STATIC_LIB  ON  CACHE INTERNAL "")
set(SPIRV_REFLECT_EXECUTABLE  OFF CACHE INTERNAL "")
set(SPIRV_REFLECT_BUILD_TESTS OFF CACHE INTERNAL "")
set(SPIRV_REFLECT_EXAMPLES    OFF CACHE INTERNAL "")
FetchContent_Declare(
    spirv_reflect
    GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Reflect.git
    GIT_TAG        vulkan-sdk-1.3.290.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(spirv_reflect)

# ------------------------------------------------------------------
# glslang (produces target `glslang`)
# ------------------------------------------------------------------
set(SKIP_GLSLANG_INSTALL   ON  CACHE INTERNAL "")
set(ENABLE_GLSLANG_BINARIES OFF CACHE INTERNAL "")
set(ENABLE_SPVREMAPPER     OFF CACHE INTERNAL "")
set(ENABLE_GLSLANG_JS      OFF CACHE INTERNAL "")
set(ENABLE_HLSL            OFF CACHE INTERNAL "")
set(ENABLE_OPT             OFF CACHE INTERNAL "")
FetchContent_Declare(
    glslang
    GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
    GIT_TAG        15.3.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(glslang)

# ------------------------------------------------------------------
# rstd
# ------------------------------------------------------------------
FetchContent_Declare(
    rstd
    GIT_REPOSITORY https://github.com/hypengw/rstd.git
    GIT_TAG        2ace236467a4e6aba8d29fc31a12d78f9483dcc4
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(rstd)

# ------------------------------------------------------------------
# miniaudio (single-header)
#
# Sources in src/Audio/ include it as `<miniaudio/miniaudio.h>`, so
# we populate into a `miniaudio/` subdirectory and expose the parent
# path on an INTERFACE target which downstream libs link against.
# ------------------------------------------------------------------
FetchContent_Declare(
    miniaudio
    GIT_REPOSITORY https://github.com/mackron/miniaudio.git
    GIT_TAG        0.11.21
    GIT_SHALLOW    TRUE
    SOURCE_DIR     ${CMAKE_CURRENT_BINARY_DIR}/miniaudio-root/miniaudio
    # miniaudio is a single-header library with no CMakeLists.txt;
    # setting SOURCE_SUBDIR to a nonexistent path makes
    # FetchContent_MakeAvailable populate without add_subdirectory.
    SOURCE_SUBDIR  cmake-noop
)
FetchContent_MakeAvailable(miniaudio)
add_library(miniaudio INTERFACE)
target_include_directories(miniaudio SYSTEM INTERFACE
    ${CMAKE_CURRENT_BINARY_DIR}/miniaudio-root)
