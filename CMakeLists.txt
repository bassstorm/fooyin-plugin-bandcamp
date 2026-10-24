cmake_minimum_required(VERSION 3.18)

project(
    bandcamp
    VERSION 0.1.4
    DESCRIPTION "Bandcamp plugin for fooyin"
    LANGUAGES CXX
)

set(CMAKE_AUTOMOC ON)

option(BANDCAMP_BUILD_TESTS "Build the Bandcamp unit tests" OFF)
option(BANDCAMP_SANITIZE "Build the unit tests with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

find_package(Fooyin REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core Concurrent Network)

# Sanitizer flags are an interface target so every consumer of the core
# library is instrumented and linked against the sanitizer runtime.
add_library(bandcamp_sanitize INTERFACE)
if(BANDCAMP_SANITIZE AND NOT MSVC)
    set(_bandcamp_sanitize_flags -fsanitize=address,undefined -fno-omit-frame-pointer -g)
    target_compile_options(bandcamp_sanitize INTERFACE ${_bandcamp_sanitize_flags})
    target_link_options(bandcamp_sanitize INTERFACE ${_bandcamp_sanitize_flags})
endif()

# Fooyin-independent code shared by the plugin, the unit tests, and the probe.
set(BANDCAMP_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
include(cmake/BandcampCoreSources.cmake)
add_library(bandcamp_core STATIC ${BANDCAMP_CORE_SOURCES})
set_target_properties(bandcamp_core PROPERTIES
    AUTOMOC ON
    POSITION_INDEPENDENT_CODE ON
)
target_include_directories(bandcamp_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(bandcamp_core
    PUBLIC Qt6::Core Qt6::Concurrent Qt6::Network bandcamp_sanitize
)

create_fooyin_plugin(
    bandcamp
    JSON_IN src/bandcamp.json.in
    DEPENDS Fooyin::Gui Qt6::Concurrent bandcamp_core
    SOURCES src/bandcampplugin.cpp
            src/bandcampplugin.h
            src/bandcampresultsdialog.cpp
            src/bandcampresultsdialog.h
            src/bandcampsettingsdialog.cpp
            src/bandcampsettingsdialog.h
            src/bandcampurldialog.cpp
            src/bandcampurldialog.h
)

if(APPLE)
    set_property(TARGET bandcamp PROPERTY INSTALL_RPATH "@loader_path/..")
elseif(UNIX)
    set_property(TARGET bandcamp PROPERTY INSTALL_RPATH "$ORIGIN/..")
endif()

if(BANDCAMP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests/unit)
endif()
