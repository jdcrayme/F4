# GoogleTest is fetched and GoogleTest module included from the root
# CMakeLists.txt (via cmake/f4_deps.cmake). This file only registers
# test executables via gtest_discover_tests().

# Helper function
function(f4_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE GTest::gtest_main f4-units)
    gtest_discover_tests(${name})
endfunction()

f4_add_test(test_dimension    test_dimension.cpp)
f4_add_test(test_conversions test_conversions.cpp)
f4_add_test(test_arithmetic  test_arithmetic.cpp)
f4_add_test(test_temperature test_temperature.cpp)
f4_add_test(test_aviation    test_aviation.cpp)
f4_add_test(test_derived     test_derived.cpp)
