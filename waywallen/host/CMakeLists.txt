# waywallen-renderer — wescene (Wallpaper Engine scene) host subprocess.
# Spawned by the Rust waywallen daemon for wallpapers of type "scene".
#
# Requires waywallen-bridge (IPC library) installed and discoverable
# via find_package. Developers can point CMake at a sibling checkout
# with `-Dwaywallen-bridge_DIR=/path/to/waywallen-bridge/build` or
# `-DCMAKE_PREFIX_PATH=/path/to/bridge/install`.

find_package(waywallen-bridge CONFIG REQUIRED)

add_executable(waywallen-renderer
    main.cpp
)

target_compile_features(waywallen-renderer PRIVATE cxx_std_20)

target_include_directories(waywallen-renderer
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(waywallen-renderer
    PRIVATE
        wescene-renderer
        waywallen::bridge
)

target_compile_options(waywallen-renderer PRIVATE
    -Wall -Wextra -Wpedantic
)

# Install the binary and the companion plugin manifests so the
# daemon can discover the renderer + its Lua sources at runtime.
install(TARGETS waywallen-renderer
    RUNTIME DESTINATION bin
)

install(DIRECTORY
    ${CMAKE_CURRENT_SOURCE_DIR}/../sources/
    DESTINATION share/waywallen/sources
    FILES_MATCHING PATTERN "*.lua"
)

install(DIRECTORY
    ${CMAKE_CURRENT_SOURCE_DIR}/../renderers/
    DESTINATION share/waywallen/renderers
    FILES_MATCHING PATTERN "*.toml"
)
