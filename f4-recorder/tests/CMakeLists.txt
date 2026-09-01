# GoogleTest is fetched and GoogleTest module included from the root
# CMakeLists.txt (via cmake/f4_deps.cmake). This file only registers
# test executables via gtest_discover_tests().

function(f4_recorder_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE GTest::gtest_main f4-recorder)
    gtest_discover_tests(${name})
endfunction()

f4_recorder_add_test(test_flight_recorder test_flight_recorder.cpp)
f4_recorder_add_test(test_combat_events test_combat_events.cpp)
f4_recorder_add_test(test_fcs_trace test_fcs_trace.cpp)
f4_recorder_add_test(test_path_geometry test_path_geometry.cpp)
