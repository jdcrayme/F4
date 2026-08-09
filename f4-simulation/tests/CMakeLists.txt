cmake_minimum_required(VERSION 3.20)

# f4-simulation tests
# Two test executables:
#   - test_visual_model_component  — the new component (add/remove/get, null safety)
#   - test_scenario_loader         — JSON scenario parsing + validation
#
# GoogleTest is fetched and the GoogleTest module included from the root
# CMakeLists.txt (via cmake/f4_deps.cmake). This file only registers test
# executables via gtest_discover_tests(), matching the pattern used by
# f4-ai/tests/CMakeLists.txt and every other f4-* tests dir.

function(f4_simulation_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE GTest::gtest_main f4-simulation)
    target_compile_features(${name} PRIVATE cxx_std_20)
    gtest_discover_tests(${name})
endfunction()

f4_simulation_add_test(test_visual_model_component test_visual_model_component.cpp)
f4_simulation_add_test(test_scenario_loader        test_scenario_loader.cpp)
f4_simulation_add_test(test_campaign_bridge        test_campaign_bridge.cpp)
f4_simulation_add_test(test_feature_spawning       test_feature_spawning.cpp)

# Point the scenario loader test at the bundled fixture so it can load a real
# JSON file without depending on the install layout.
target_compile_definitions(test_scenario_loader PRIVATE
    F4_SIMULATION_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)

# Point the campaign bridge test at the generated F-16 aircraft config
# fixture (built by f4-convert from f4-convert/tests/fixtures/f16.dat).
# The test uses this to construct a valid AircraftConfig for
# FlightModelComponent::init() — empty aero tables would throw.
target_compile_definitions(test_campaign_bridge PRIVATE
    F4_GENERATED_FIXTURES_DIR="${F4_GENERATED_FIXTURES_DIR}"
)

# Point the feature-spawning test at the KoreaObj fixtures (for ModelDatabase)
# and the generated F-16 aircraft config fixture (for AircraftConfig).
target_compile_definitions(test_feature_spawning PRIVATE
    F4_MODELS_DIR="${CMAKE_SOURCE_DIR}/f4-models/tests/fixtures"
    F4_GENERATED_FIXTURES_DIR="${F4_GENERATED_FIXTURES_DIR}"
)
