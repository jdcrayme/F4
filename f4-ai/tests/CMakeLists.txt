# GoogleTest is fetched and GoogleTest module included from the root
# CMakeLists.txt (via cmake/f4_deps.cmake). This file only registers
# test executables via gtest_discover_tests().

function(f4_ai_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE GTest::gtest_main f4-ai)
    gtest_discover_tests(${name})
endfunction()

f4_ai_add_test(test_ai_sensor_fusion test_sensor_fusion.cpp)
f4_ai_add_test(test_ground_steering  test_ground_steering.cpp)
f4_ai_add_test(test_air_steering     test_air_steering.cpp)
f4_ai_add_test(test_takeoff_module   test_takeoff_module.cpp)
f4_ai_add_test(test_navigation_module test_navigation_module.cpp)
f4_ai_add_test(test_landing_module   test_landing_module.cpp)
f4_ai_add_test(test_brain_component  test_brain_component.cpp)

# test_brain_component creates FlightModelComponent instances to test the
# brain ↔ flight model integration. It links f4-flight-model PRIVATELY
# (the f4-ai library itself does NOT depend on f4-flight-model — only
# the test does, for concrete component creation).
target_link_libraries(test_brain_component PRIVATE f4-flight-model)

# test_brain_component uses the F16 aircraft config from generated fixtures.
target_compile_definitions(test_brain_component PRIVATE
    F4_GENERATED_FIXTURES_DIR="${F4_GENERATED_FIXTURES_DIR}")
add_dependencies(test_brain_component convert_golden_fixtures)
f4_ai_add_test(test_missile_module test_missile_module.cpp)
f4_ai_add_test(test_bvr_module      test_bvr_module.cpp)
f4_ai_add_test(test_wvr_module      test_wvr_module.cpp)
f4_ai_add_test(test_wingman_module test_wingman_module.cpp)
