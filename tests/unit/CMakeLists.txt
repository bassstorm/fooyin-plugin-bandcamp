find_package(Qt6 REQUIRED COMPONENTS Core Test)

add_executable(bandcamp_unit_tests
    bandcamptests.cpp
    bandcamppostprocessortests.cpp
    bandcampcontracttests.cpp
    bandcampdiagnosticstests.cpp
    bandcampclienttests.cpp
)

target_link_libraries(bandcamp_unit_tests PRIVATE bandcamp_core Qt6::Core Qt6::Test)
set_target_properties(bandcamp_unit_tests PROPERTIES AUTOMOC ON)

add_test(NAME bandcamp.unit COMMAND bandcamp_unit_tests)
