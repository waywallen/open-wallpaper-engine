cmake_minimum_required(VERSION 3.14)
project(wescene-renderer)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Restrict to Clang. The renderer + tests rely on Clang-specific behaviour
# (clang-format style, sanitizer flags wired in src/CMakeLists.txt, and
# observed glslang/FetchContent quirks) and we have no CI matrix for GCC.
# Override with -DWAYWALLEN_ALLOW_NONCLANG=ON if you know what you're doing.
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT WAYWALLEN_ALLOW_NONCLANG)
    message(FATAL_ERROR
        "wescene-renderer requires Clang (got ${CMAKE_CXX_COMPILER_ID}). "
        "Re-run cmake with `-DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang`, "
        "or pass -DWAYWALLEN_ALLOW_NONCLANG=ON to bypass this check.")
endif()

find_package(PkgConfig REQUIRED)


include_directories(SYSTEM third_party)
add_subdirectory(third_party)

link_libraries(Eigen3::Eigen)
add_subdirectory(src)


# Test suite — opt-in via BUILD_TESTING (default ON via CTest module).
include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# waywallen-renderer: renderer host subprocess that speaks the IPC
# protocol to the Rust waywallen daemon via waywallen-bridge. Opt-in so
# consumers that just want the scene renderer library are unaffected.
option(BUILD_WAYWALLEN_HOST "Build the waywallen-renderer host executable" ON)
if(BUILD_WAYWALLEN_HOST)
    add_subdirectory(waywallen/host)
endif()

if(BUILD_QML)
    find_package(Qt6 COMPONENTS Gui Quick Qml REQUIRED)

    set(CMAKE_AUTOMOC ON) 
    set(CMAKE_AUTORCC ON) 
    set(CMAKE_AUTOUIC ON)

    add_library(${PROJECT_NAME}-qml
    STATIC
        qml_helper/SceneBackend.cpp
        qml_helper/glExtra.cpp
    )
    target_link_libraries(${PROJECT_NAME}-qml
    PUBLIC
        Qt::Quick
        Qt::Gui
        glad
        ${PROJECT_NAME}
    )
    target_include_directories(${PROJECT_NAME}-qml PUBLIC qml_helper third_party)

    set(CMAKE_AUTOMOC OFF) 
    set(CMAKE_AUTORCC OFF) 
    set(CMAKE_AUTOUIC OFF) 
endif()
