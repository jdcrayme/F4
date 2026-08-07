# GoogleTest is fetched and GoogleTest module included from the root
# CMakeLists.txt (via cmake/f4_deps.cmake). This file only registers
# test executables via gtest_discover_tests().

# Each test target links ONLY against f4-state-machine. If a test ever needs
# f4-math, f4-data, or any other library, that is a coupling bug and the test
# should be moved to a higher-level library's test suite. f4-state-machine is
# a foundation library with zero dependencies — its tests must prove that.
function(f4_state_machine_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE GTest::gtest_main f4-state-machine)
    gtest_discover_tests(${name})
endfunction()

f4_state_machine_add_test(test_state_machine test_state_machine.cpp)
f4_state_machine_add_test(test_stall_sm      test_stall_sm.cpp)
f4_state_machine_add_test(test_atc_sm        test_atc_sm.cpp)
f4_state_machine_add_test(test_layered       test_layered.cpp)
f4_state_machine_add_test(test_trace         test_trace.cpp)
f4_state_machine_add_test(test_serialize     test_serialize.cpp)
