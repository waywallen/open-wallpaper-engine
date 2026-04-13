# waywallen-renderer ��� wescene (Wallpaper Engine scene) host subprocess.
# Spawned by the Rust waywallen daemon for wallpapers of type "scene".

include(FetchContent)

FetchContent_Declare(
    waywallen-bridge
    GIT_REPOSITORY https://github.com/waywallen/waywallen-bridge.git
    GIT_TAG        main
)
FetchContent_GetProperties(waywallen-bridge)
if(NOT waywallen-bridge_POPULATED)
    FetchContent_Populate(waywallen-bridge)

    # Suppress bridge's install() rules when consumed as a subdirectory.
    set(_save_skip_install ${CMAKE_SKIP_INSTALL_RULES})
    set(CMAKE_SKIP_INSTALL_RULES ON)
    add_subdirectory(${waywallen-bridge_SOURCE_DIR} ${waywallen-bridge_BINARY_DIR} EXCLUDE_FROM_ALL)
    set(CMAKE_SKIP_INSTALL_RULES ${_save_skip_install})
endif()

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
