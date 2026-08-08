# f4-scenario-player/tests/CMakeLists.txt
#
# Two test executables:
#   - test_airport_geometry  — verify the runway/taxi/parking geometry
#                              synthesis from a Scenario (endpoints, colors,
#                              marker placement).
#   - test_coordinate_transform — verify the ENU → Raylib coordinate
#                              conversion (the most error-prone piece of
#                              the renderer — getting this wrong puts the
#                              aircraft underground or facing backward).
#
# Both are unit tests (no GL context, no Raylib init). The full
# screenshot smoke test runs in CI via the --screenshot CLI flag.

cmake_minimum_required(VERSION 3.20)

function(f4_scenario_player_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE GTest::gtest_main f4_scenario_player_lib)
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../src)  # for viewer_state.hpp (private)
    target_compile_features(${name} PRIVATE cxx_std_20)
    gtest_discover_tests(${name})
endfunction()

f4_scenario_player_add_test(test_airport_geometry     test_airport_geometry.cpp)
f4_scenario_player_add_test(test_coordinate_transform test_coordinate_transform.cpp)
