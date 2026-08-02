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

# Each test target links against f4-data (which transitively pulls in
# f4-math and nlohmann_json).
function(f4_data_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE GTest::gtest_main f4-data)
    gtest_discover_tests(${name})
endfunction()

f4_data_add_test(test_validation      test_validation.cpp)
f4_data_add_test(test_config_loader   test_config_loader.cpp)
f4_data_add_test(test_table_accessors test_table_accessors.cpp)

# Pass the path to the generated JSON fixtures (produced by f4-convert at
# build time) to the test executables that need them.
target_compile_definitions(test_config_loader PRIVATE
    F4_GENERATED_FIXTURES_DIR="${F4_GENERATED_FIXTURES_DIR}")
target_compile_definitions(test_table_accessors PRIVATE
    F4_GENERATED_FIXTURES_DIR="${F4_GENERATED_FIXTURES_DIR}")

# f4-data tests depend on the generated fixtures being present.
add_dependencies(test_config_loader  convert_golden_fixtures)
add_dependencies(test_table_accessors convert_golden_fixtures)
