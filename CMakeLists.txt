cmake_minimum_required(VERSION 3.21)

project(
    bandcamp
    VERSION 0.2.0
    DESCRIPTION "Stream music from Bandcamp in Fooyin"
    LANGUAGES CXX
)

set(CMAKE_AUTOMOC ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(BANDCAMP_PLUGIN_BUILD_TESTS "Build the Bandcamp deterministic and live tests" OFF)
option(BANDCAMP_PLUGIN_SANITIZE "Instrument test targets with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
set(BANDCAMP_PLUGIN_VERSION_SUFFIX "" CACHE STRING "Suffix appended to the project version in the plugin manifest and bundle name, e.g. .<hash>")
set(BANDCAMP_PLUGIN_VERSION "${PROJECT_VERSION}${BANDCAMP_PLUGIN_VERSION_SUFFIX}")

find_package(Fooyin REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Network Widgets)

# Sanitizer flags are an interface target so every consumer of the core
# library is instrumented and linked against the sanitizer runtime.
add_library(bandcamp_sanitize INTERFACE)
if(BANDCAMP_PLUGIN_SANITIZE AND NOT MSVC)
    # Recoverable UBSan reports would otherwise print and let the run pass.
    set(_bandcamp_sanitize_flags -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -g)
    target_compile_options(bandcamp_sanitize INTERFACE ${_bandcamp_sanitize_flags})
    target_link_options(bandcamp_sanitize INTERFACE ${_bandcamp_sanitize_flags})
endif()

add_library(bandcamp_core STATIC
    src/bandcampclient.cpp
    src/bandcampclient.h
    src/bandcampattemptsupervisor.cpp
    src/bandcampattemptsupervisor.h
    src/bandcampdiagnostics.cpp
    src/bandcampdiagnostics.h
    src/bandcamplogging.cpp
    src/bandcamplogging.h
    src/bandcamplistingresolver.cpp
    src/bandcamplistingresolver.h
    src/bandcampmodel.h
    src/bandcamporchestrator.cpp
    src/bandcamporchestrator.h
    src/bandcampreleaseresolver.cpp
    src/bandcampreleaseresolver.h
    src/bandcampassembly.cpp
    src/bandcampassembly.h
    src/bandcampparser.cpp
    src/bandcampparser.h
    src/bandcamphtmlreader.cpp
    src/bandcamphtmlreader_p.h
    src/bandcampdirtytitle.cpp
    src/bandcampdirtytitle.h
    src/bandcampoutcome.cpp
    src/bandcampoutcome.h
    src/bandcamppostprocessor.cpp
    src/bandcamppostprocessor.h
    src/bandcampresolutionservice.cpp
    src/bandcampresolutionservice.h
    src/bandcamptrackuri.cpp
    src/bandcamptrackuri.h
    src/bandcampsettings.cpp
    src/bandcampsettings.h
    src/bandcampurl.cpp
    src/bandcampurl.h
)
target_include_directories(bandcamp_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(bandcamp_core
    PUBLIC Qt6::Core Qt6::Network bandcamp_sanitize
)

add_library(bandcamp_fooyin STATIC
    src/bandcampplayback.cpp
    src/bandcampplayback.h
    src/bandcampplaybackpreparation.cpp
    src/bandcampplaybackpreparation.h
    src/bandcampfooyintrack.cpp
    src/bandcampfooyintrack.h
    src/bandcamplibrarygateway.cpp
    src/bandcamplibrarygateway.h
    src/bandcampimportcontroller.cpp
    src/bandcampimportcontroller.h
    src/bandcampimportpipeline.cpp
    src/bandcampimportpipeline.h
    src/bandcampstreamprobe.cpp
    src/bandcampstreamprobe.h
    src/bandcamprefreshcontroller.cpp
    src/bandcamprefreshcontroller.h
    src/bandcampprogressdialog.cpp
    src/bandcampprogressdialog.h
    src/bandcampremotestream.h
)
target_include_directories(bandcamp_fooyin PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(bandcamp_fooyin
    PUBLIC bandcamp_core Fooyin::Core Qt6::Core Qt6::Gui Qt6::Widgets bandcamp_sanitize
)

create_fooyin_plugin(
    bandcamp
    JSON_IN src/bandcamp.json.in
    DEPENDS Fooyin::Gui bandcamp_core bandcamp_fooyin
    SOURCES src/bandcampplugin.cpp
            src/bandcampplugin.h
            src/bandcampsettingsdialog.cpp
            src/bandcampsettingsdialog.h
            src/bandcampurldialog.cpp
            src/bandcampurldialog.h
)

create_fooyin_plugin_bundle(
    bandcamp
    NAME "fooyin-bandcamp-${BANDCAMP_PLUGIN_VERSION}"
    FILES README.md
    LICENSE_FILES LICENSE
)

if(BANDCAMP_PLUGIN_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests/deterministic)
    add_subdirectory(tests/live)
endif()

# The dev container pins this clang-format major; builds elsewhere skip the targets.
find_program(BANDCAMP_CLANG_FORMAT NAMES clang-format-20)
if(BANDCAMP_CLANG_FORMAT)
    file(GLOB_RECURSE _bandcamp_format_sources CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/*.h
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.h
    )
    add_custom_target(
        clang-format
        COMMAND ${BANDCAMP_CLANG_FORMAT} -i ${_bandcamp_format_sources}
        VERBATIM
    )
    add_custom_target(
        clang-format-check
        COMMAND ${BANDCAMP_CLANG_FORMAT} --dry-run --Werror ${_bandcamp_format_sources}
        VERBATIM
    )
else()
    message(STATUS "clang-format-20 not found; clang-format and clang-format-check targets are unavailable")
endif()
