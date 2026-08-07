# GoogleTest is fetched and GoogleTest module included from the root
# CMakeLists.txt (via cmake/f4_deps.cmake). This file only registers
# test executables via gtest_discover_tests().

function(f4_ai_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE GTest::gtest_main f4-ai)
    gtest_discover_tests(${name})
endfunction()

f4_ai_add_test(test_ai_sensor_fusion test_sensor_fusion.cpp)
