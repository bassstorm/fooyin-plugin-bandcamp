find_package(Qt6 REQUIRED COMPONENTS Core Test)

add_executable(bandcamp_deterministic_tests
    bandcamptests.cpp
    bandcampparsertests.cpp
    bandcamppostprocessortests.cpp
    bandcampcontracttests.cpp
    bandcampdiagnosticstests.cpp
    bandcampclienttests.cpp
    bandcamporchestratortests.cpp
    bandcampservicetests.cpp
    bandcampfooyintracktests.cpp
)

target_link_libraries(bandcamp_deterministic_tests PRIVATE bandcamp_fooyin bandcamp_core Fooyin::Core Qt6::Core Qt6::Test)
set_target_properties(bandcamp_deterministic_tests PROPERTIES AUTOMOC ON)

add_test(NAME bandcamp.deterministic COMMAND bandcamp_deterministic_tests)
