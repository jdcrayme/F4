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

# Each test target links against f4-flight-model (which transitively pulls
# in f4-data, f4-math, and nlohmann_json).
function(f4_fm_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE GTest::gtest_main f4-flight-model)
    gtest_discover_tests(${name})
endfunction()

# Pass the path to the generated JSON fixtures (produced by f4-convert at
# build time) to the test executables that need them.
f4_fm_add_test(test_atmosphere   test_atmosphere.cpp)
f4_fm_add_test(test_flight_model test_flight_model.cpp)

target_compile_definitions(test_flight_model PRIVATE
    F4_GENERATED_FIXTURES_DIR="${F4_GENERATED_FIXTURES_DIR}")
add_dependencies(test_flight_model convert_golden_fixtures)
