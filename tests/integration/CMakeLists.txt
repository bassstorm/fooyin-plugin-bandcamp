cmake_minimum_required(VERSION 3.16)

# Standalone live probe. Not added to the plugin build and not registered with CTest.
# Build on demand: cmake -S tests/integration -B build/probe && cmake --build build/probe
project(bandcamp_probe LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Network Concurrent)
find_package(LibXml2 REQUIRED)

# The probe builds the same core sources as the plugin, so the two cannot drift apart.
set(BANDCAMP_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/BandcampCoreSources.cmake)

add_executable(bandcamp-probe bandcamp_probe.cpp ${BANDCAMP_CORE_SOURCES})

target_include_directories(bandcamp-probe PRIVATE ${BANDCAMP_SRC_DIR})
target_link_libraries(bandcamp-probe PRIVATE Qt6::Core Qt6::Network Qt6::Concurrent LibXml2::LibXml2)

if(BANDCAMP_SANITIZE AND NOT MSVC)
    set(_probe_sanitize_flags -fsanitize=address,undefined -fno-omit-frame-pointer -g)
    target_compile_options(bandcamp-probe PRIVATE ${_probe_sanitize_flags})
    target_link_options(bandcamp-probe PRIVATE ${_probe_sanitize_flags})
endif()
