# waywallen-wescene-renderer ��� wescene (Wallpaper Engine scene) host subprocess.
# Spawned by the Rust waywallen daemon for wallpapers of type "scene".

include(FetchContent)

find_package(waywallen-bridge REQUIRED)

add_executable(waywallen-wescene-renderer
    main.cpp
)

target_compile_features(waywallen-wescene-renderer PRIVATE cxx_std_20)

target_include_directories(waywallen-wescene-renderer
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/third_party
)

target_link_libraries(waywallen-wescene-renderer
    PRIVATE
        wescene-renderer
        waywallen::bridge
        argparse::argparse
)

target_compile_options(waywallen-wescene-renderer PRIVATE
    -Wall -Wextra -Wpedantic
)

# Install the binary and the companion plugin manifests so the
# daemon can discover the renderer + its Lua sources at runtime.
install(TARGETS waywallen-wescene-renderer
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY
    ${CMAKE_CURRENT_SOURCE_DIR}/sources/
    DESTINATION share/waywallen/sources
    FILES_MATCHING PATTERN "*.lua"
)

install(DIRECTORY
    ${CMAKE_CURRENT_SOURCE_DIR}/renderers/
    DESTINATION share/waywallen/renderers
    FILES_MATCHING PATTERN "*.toml"
)
