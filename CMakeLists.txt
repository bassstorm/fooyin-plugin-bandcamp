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
find_package(Qt6 REQUIRED COMPONENTS Concurrent)

create_fooyin_plugin(
    bandcamp
    JSON_IN src/bandcamp.json.in
    DEPENDS Fooyin::Gui Qt6::Concurrent
    SOURCES src/bandcampclient.cpp
            src/bandcampclient.h
            src/bandcamplogging.cpp
            src/bandcamplogging.h
            src/bandcampmodel.h
            src/bandcampparser.cpp
            src/bandcampparser.h
            src/bandcampdirtytitle.cpp
            src/bandcampdirtytitle.h
            src/bandcamppostprocessor.cpp
            src/bandcamppostprocessor.h
            src/bandcampprocessors.cpp
            src/bandcampprocessors.h
            src/bandcampplugin.cpp
            src/bandcampplugin.h
            src/bandcampresultsdialog.cpp
            src/bandcampresultsdialog.h
            src/bandcampservice.cpp
            src/bandcampservice.h
            src/bandcampsettings.cpp
            src/bandcampsettings.h
            src/bandcampsettingsdialog.cpp
            src/bandcampsettingsdialog.h
            src/bandcampurl.cpp
            src/bandcampurl.h
            src/bandcampurldialog.cpp
            src/bandcampurldialog.h
)

if(APPLE)
    set_property(TARGET bandcamp PROPERTY INSTALL_RPATH "@loader_path/..")
elseif(UNIX)
    set_property(TARGET bandcamp PROPERTY INSTALL_RPATH "$ORIGIN/..")
endif()

if(BANDCAMP_BUILD_TESTS)
    add_subdirectory(tests/unit)
endif()
