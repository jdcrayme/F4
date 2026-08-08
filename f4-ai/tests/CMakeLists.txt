# GoogleTest is fetched and GoogleTest module included from the root
# CMakeLists.txt (via cmake/f4_deps.cmake). This file only registers
# test executables via gtest_discover_tests().

function(f4_ai_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE GTest::gtest_main f4-ai)
    gtest_discover_tests(${name})
endfunction()

f4_ai_add_test(test_ai_sensor_fusion test_sensor_fusion.cpp)
f4_ai_add_test(test_takeoff_module   test_takeoff_module.cpp)
f4_ai_add_test(test_brain_component  test_brain_component.cpp)

# test_brain_component uses the F16 aircraft config from generated fixtures.
target_compile_definitions(test_brain_component PRIVATE
    F4_GENERATED_FIXTURES_DIR="${F4_GENERATED_FIXTURES_DIR}")
add_dependencies(test_brain_component convert_golden_fixtures)
