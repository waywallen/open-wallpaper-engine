include(FetchContent)
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)

# ------------------------------------------------------------------
# Locally vendored deps still living under third_party/
# (glad, argparse, effolkronium, vog, stb_image.h, vk_mem_alloc.h)
# ------------------------------------------------------------------
add_subdirectory(glad)

# ------------------------------------------------------------------
# JSON-driven FetchContent for eigen, nlohmann_json, spirv_reflect,
# glslang, miniaudio. To enable the flatpak-aware dependency provider,
# pass:
#   -DCMAKE_PROJECT_TOP_LEVEL_INCLUDES=cmake/FetchDepsProvider.cmake
# ------------------------------------------------------------------
include(${CMAKE_SOURCE_DIR}/cmake/FetchDeps.cmake)

# Cache vars consumed by nested add_subdirectory calls inside fetchdeps().
set(JSON_BuildTests         OFF CACHE INTERNAL "")
set(JSON_Install            OFF CACHE INTERNAL "")
set(SPIRV_REFLECT_STATIC_LIB  ON  CACHE INTERNAL "")
set(SPIRV_REFLECT_EXECUTABLE  OFF CACHE INTERNAL "")
set(SPIRV_REFLECT_BUILD_TESTS OFF CACHE INTERNAL "")
set(SPIRV_REFLECT_EXAMPLES    OFF CACHE INTERNAL "")
set(SKIP_GLSLANG_INSTALL    ON  CACHE INTERNAL "")
set(ENABLE_GLSLANG_BINARIES OFF CACHE INTERNAL "")
set(ENABLE_SPVREMAPPER      OFF CACHE INTERNAL "")
set(ENABLE_GLSLANG_JS       OFF CACHE INTERNAL "")
set(ENABLE_HLSL             OFF CACHE INTERNAL "")
set(ENABLE_OPT              OFF CACHE INTERNAL "")

fetchdeps(${CMAKE_SOURCE_DIR}/deps.json)

# Post-fetch setup for eigen (header-only, source_subdir=cmake-noop).
add_library(eigen INTERFACE)
add_library(Eigen3::Eigen ALIAS eigen)
target_include_directories(eigen SYSTEM INTERFACE ${eigen_SOURCE_DIR})

# Post-fetch setup for miniaudio (single-header, source_subdir=cmake-noop).
# Consumers include as <miniaudio.h> / <extras/stb_vorbis.c>.
add_library(miniaudio INTERFACE)
target_include_directories(miniaudio SYSTEM INTERFACE ${miniaudio_SOURCE_DIR})
