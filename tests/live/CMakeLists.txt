find_package(Qt6 REQUIRED COMPONENTS Core Network Test)

add_executable(bandcamp_live_tests bandcamplivetests.cpp)
target_link_libraries(bandcamp_live_tests PRIVATE bandcamp_core Qt6::Core Qt6::Network Qt6::Test)

add_test(NAME bandcamp.live COMMAND bandcamp_live_tests)
set_tests_properties(bandcamp.live PROPERTIES LABELS live TIMEOUT 900)
