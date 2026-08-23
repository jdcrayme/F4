# GoogleTest is fetched and GoogleTest module included from the root
# CMakeLists.txt (via cmake/f4_deps.cmake). This file only registers
# test executables via gtest_discover_tests().

# Each test target links against f4-flight-model (which transitively pulls
# in f4-data, f4-math, f4-state-machine, f4-messaging, and nlohmann_json).
function(f4_fm_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE GTest::gtest_main f4-flight-model)
    gtest_discover_tests(${name})
endfunction()

# Pass the path to the generated JSON fixtures (produced by f4-convert at
# build time) to the test executables that need them.
f4_fm_add_test(test_atmosphere           test_atmosphere.cpp)
f4_fm_add_test(test_flight_model         test_flight_model.cpp)
f4_fm_add_test(test_flight_model_component test_flight_model_component.cpp)
f4_fm_add_test(test_stall_sm_integration test_stall_sm_integration.cpp)
f4_fm_add_test(test_stall_bus_integration test_stall_bus_integration.cpp)
f4_fm_add_test(test_engine               test_engine.cpp)
f4_fm_add_test(test_aerodynamics         test_aerodynamics.cpp)
f4_fm_add_test(test_gear                 test_gear.cpp)
f4_fm_add_test(test_fcs                  test_fcs.cpp)
f4_fm_add_test(test_eom                  test_eom.cpp)
f4_fm_add_test(test_angle                test_angle.cpp)

# Closed-loop control tests — wire AirSteering (AI cascade) into the full
# FlightModel (FCS + EOM + aero) and assert closed-loop convergence +
# stability. Tests are DISABLED_ until the corresponding fixes in
# FLIGHT_CONTROL_STABILITY_PLAN.md land.
f4_fm_add_test(test_control_loops        test_control_loops.cpp)
target_link_libraries(test_control_loops PRIVATE f4-ai)
target_compile_definitions(test_control_loops PRIVATE
    F4_GENERATED_FIXTURES_DIR="${F4_GENERATED_FIXTURES_DIR}")
add_dependencies(test_control_loops convert_golden_fixtures)

target_compile_definitions(test_flight_model PRIVATE
    F4_GENERATED_FIXTURES_DIR="${F4_GENERATED_FIXTURES_DIR}")
target_compile_definitions(test_flight_model_component PRIVATE
    F4_GENERATED_FIXTURES_DIR="${F4_GENERATED_FIXTURES_DIR}")
target_compile_definitions(test_stall_sm_integration PRIVATE
    F4_GENERATED_FIXTURES_DIR="${F4_GENERATED_FIXTURES_DIR}")
target_compile_definitions(test_stall_bus_integration PRIVATE
    F4_GENERATED_FIXTURES_DIR="${F4_GENERATED_FIXTURES_DIR}")
add_dependencies(test_flight_model convert_golden_fixtures)
add_dependencies(test_flight_model_component convert_golden_fixtures)
add_dependencies(test_stall_sm_integration convert_golden_fixtures)
add_dependencies(test_stall_bus_integration convert_golden_fixtures)
