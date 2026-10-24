find_package(Qt6 REQUIRED COMPONENTS Core Test)

add_executable(bandcamp_unit_tests
    bandcamptests.cpp
    bandcamppostprocessortests.cpp
    ${CMAKE_SOURCE_DIR}/src/bandcamplogging.cpp
    ${CMAKE_SOURCE_DIR}/src/bandcampdirtytitle.cpp
    ${CMAKE_SOURCE_DIR}/src/bandcamppostprocessor.cpp
    ${CMAKE_SOURCE_DIR}/src/bandcampparser.cpp
    ${CMAKE_SOURCE_DIR}/src/bandcampprocessors.cpp
    ${CMAKE_SOURCE_DIR}/src/bandcampsettings.cpp
    ${CMAKE_SOURCE_DIR}/src/bandcampurl.cpp
)

target_include_directories(bandcamp_unit_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(bandcamp_unit_tests PRIVATE Qt6::Core Qt6::Test)
set_target_properties(bandcamp_unit_tests PROPERTIES AUTOMOC ON)

if(BANDCAMP_SANITIZE AND NOT MSVC)
    set(_bandcamp_sanitize_flags -fsanitize=address,undefined -fno-omit-frame-pointer -g)
    target_compile_options(bandcamp_unit_tests PRIVATE ${_bandcamp_sanitize_flags})
    target_link_options(bandcamp_unit_tests PRIVATE ${_bandcamp_sanitize_flags})
endif()
