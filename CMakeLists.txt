cmake_minimum_required(VERSION 3.18)

project(
    bandcamp
    VERSION 0.1.4
    DESCRIPTION "Bandcamp plugin for fooyin"
    LANGUAGES CXX
)

set(CMAKE_AUTOMOC ON)

option(BANDCAMP_BUILD_TESTS "Build the Bandcamp deterministic and live tests" OFF)
option(BANDCAMP_SANITIZE "Instrument test targets with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

find_package(Fooyin REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core Concurrent Network)
find_package(LibXml2 REQUIRED)

# Sanitizer flags are an interface target so every consumer of the core
# library is instrumented and linked against the sanitizer runtime.
add_library(bandcamp_sanitize INTERFACE)
if(BANDCAMP_SANITIZE AND NOT MSVC)
    # Recoverable UBSan reports would otherwise print and let the run pass.
    set(_bandcamp_sanitize_flags -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -g)
    target_compile_options(bandcamp_sanitize INTERFACE ${_bandcamp_sanitize_flags})
    target_link_options(bandcamp_sanitize INTERFACE ${_bandcamp_sanitize_flags})
endif()

set(BANDCAMP_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
include(cmake/BandcampCoreSources.cmake)
add_library(bandcamp_core STATIC ${BANDCAMP_CORE_SOURCES})
set_target_properties(bandcamp_core PROPERTIES
    AUTOMOC ON
    POSITION_INDEPENDENT_CODE ON
)
target_include_directories(bandcamp_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(bandcamp_core
    PUBLIC Qt6::Core Qt6::Concurrent Qt6::Network LibXml2::LibXml2 bandcamp_sanitize
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

# Fooyin's Install... dialog reads a zip (.zip or .fyplugin) with the module under
# a platform directory such as linux-x64/. The module is the same Release binary.
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _bandcamp_arch)
if(_bandcamp_arch MATCHES "^(x86_64|amd64)$")
    set(_bandcamp_arch x64)
elseif(_bandcamp_arch MATCHES "^(aarch64|arm64)$")
    set(_bandcamp_arch arm64)
endif()
if(WIN32)
    set(_bandcamp_os windows)
elseif(APPLE)
    set(_bandcamp_os macos)
elseif(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    set(_bandcamp_os freebsd)
else()
    set(_bandcamp_os linux)
endif()
set(_bandcamp_platform "${_bandcamp_os}-${_bandcamp_arch}")
set(_bandcamp_stage "${CMAKE_CURRENT_BINARY_DIR}/bundle")
set(_bandcamp_bundle "${CMAKE_CURRENT_BINARY_DIR}/fooyin-bandcamp-${PROJECT_VERSION}-${_bandcamp_platform}.fyplugin")

add_custom_target(
    bandcamp_bundle
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${_bandcamp_stage}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_bandcamp_stage}/${_bandcamp_platform}"
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:bandcamp> "${_bandcamp_stage}/${_bandcamp_platform}/"
    COMMAND ${CMAKE_COMMAND} -E chdir "${_bandcamp_stage}"
            ${CMAKE_COMMAND} -E tar cf "${_bandcamp_bundle}" --format=zip "${_bandcamp_platform}"
    DEPENDS bandcamp
    VERBATIM
    COMMENT "Creating Fooyin plugin bundle ${_bandcamp_bundle}"
)

if(BANDCAMP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests/deterministic)
    add_subdirectory(tests/live)
endif()
