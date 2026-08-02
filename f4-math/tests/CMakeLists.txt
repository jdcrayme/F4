include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()
include(GoogleTest)

# Each test target links ONLY against f4-math. If a test ever needs f4-units
# or any other library, that is a coupling bug and the test should be moved
# to a higher-level library's test suite.
function(f4_math_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE GTest::gtest_main f4-math)
    gtest_discover_tests(${name})
endfunction()

f4_math_add_test(test_scalar       test_scalar.cpp)
f4_math_add_test(test_table        test_table.cpp)
f4_math_add_test(test_integration  test_integration.cpp)
f4_math_add_test(test_filters     test_filters.cpp)
f4_math_add_test(test_vec3        test_vec3.cpp)
f4_math_add_test(test_quat        test_quat.cpp)
f4_math_add_test(test_solver      test_solver.cpp)
