# waywallen-renderer — permanent C++ renderer host subprocess spawned
# by the Rust waywallen daemon. Built when BUILD_WAYWALLEN_HOST=ON.

add_executable(waywallen-renderer
    main.cpp
    uds.cpp
)

target_compile_features(waywallen-renderer PRIVATE cxx_std_20)

target_include_directories(waywallen-renderer
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(waywallen-renderer
    PRIVATE
        wescene-renderer
        nlohmann_json::nlohmann_json
)

# The host only makes sense when the rest of wescene-renderer is built
# with offscreen DMA-BUF export support from iteration 1a.
target_compile_options(waywallen-renderer PRIVATE
    -Wall -Wextra -Wpedantic)
