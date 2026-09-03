# GoogleTest is fetched and GoogleTest module included from the root
# CMakeLists.txt (via cmake/f4_deps.cmake). This file only registers
# test executables via gtest_discover_tests().

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
f4_data_add_test(test_engine_rpm_schedule test_engine_rpm_schedule.cpp)
f4_data_add_test(test_simdata_loaders test_simdata_loaders.cpp)

# Pass the path to the generated JSON fixtures (produced by f4-convert at
# build time) to the test executables that need them.
target_compile_definitions(test_config_loader PRIVATE
    F4_GENERATED_FIXTURES_DIR="${F4_GENERATED_FIXTURES_DIR}")
target_compile_definitions(test_table_accessors PRIVATE
    F4_GENERATED_FIXTURES_DIR="${F4_GENERATED_FIXTURES_DIR}")
target_compile_definitions(test_simdata_loaders PRIVATE
    F4_GENERATED_FIXTURES_DIR="${F4_GENERATED_FIXTURES_DIR}")

# f4-data tests depend on the generated fixtures being present.
add_dependencies(test_config_loader  convert_golden_fixtures)
add_dependencies(test_table_accessors convert_golden_fixtures)
add_dependencies(test_simdata_loaders convert_golden_fixtures)
