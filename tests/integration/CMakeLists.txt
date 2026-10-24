cmake_minimum_required(VERSION 3.16)

# Standalone live probe. Not added to the plugin build and not registered with CTest.
# Build on demand: cmake -S tests/integration -B build/probe && cmake --build build/probe
project(bandcamp_probe LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Network Concurrent)

set(BANDCAMP_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../src)

add_executable(bandcamp-probe
    bandcamp_probe.cpp
    ${BANDCAMP_SRC}/bandcamplogging.cpp
    ${BANDCAMP_SRC}/bandcampclient.cpp
    ${BANDCAMP_SRC}/bandcampdirtytitle.cpp
    ${BANDCAMP_SRC}/bandcamppostprocessor.cpp
    ${BANDCAMP_SRC}/bandcampparser.cpp
    ${BANDCAMP_SRC}/bandcampprocessors.cpp
    ${BANDCAMP_SRC}/bandcampservice.cpp
    ${BANDCAMP_SRC}/bandcampurl.cpp
)

target_include_directories(bandcamp-probe PRIVATE ${BANDCAMP_SRC})
target_link_libraries(bandcamp-probe PRIVATE Qt6::Core Qt6::Network Qt6::Concurrent)

if(BANDCAMP_SANITIZE AND NOT MSVC)
    set(_probe_sanitize_flags -fsanitize=address,undefined -fno-omit-frame-pointer -g)
    target_compile_options(bandcamp-probe PRIVATE ${_probe_sanitize_flags})
    target_link_options(bandcamp-probe PRIVATE ${_probe_sanitize_flags})
endif()
