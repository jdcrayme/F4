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
f4_simulation_add_test(test_vehicle_spawning       test_vehicle_spawning.cpp)
f4_simulation_add_test(test_bubble_manager         test_bubble_manager.cpp)
f4_simulation_add_test(test_frames                 test_frames.cpp)
f4_simulation_add_test(test_real_layout_derivation test_real_layout_derivation.cpp)
f4_simulation_add_test(test_digi_mission           test_digi_mission.cpp)
f4_simulation_add_test(test_fcs_trace_pipeline     test_fcs_trace_pipeline.cpp)
f4_simulation_add_test(test_combat_integration     test_combat_integration.cpp)
f4_simulation_add_test(test_combat_transcript      test_combat_transcript.cpp)

# The combat integration test drives the full M3 chain (radar detect ->
# track -> lock -> RWR -> launch -> kill -> sweep) through Simulation. It
# needs the generated F-16 aircraft config (FlightModelComponent::init
# requires real aero tables) exactly like the campaign-bridge test.
# It also plays the SHIPPED <build>/scenarios/bvr_intercept.json, so it
# needs the configured scenario dir too.
target_compile_definitions(test_combat_integration PRIVATE
    F4_GENERATED_FIXTURES_DIR="${F4_GENERATED_FIXTURES_DIR}"
    F4_SCENARIOS_DIR="${CMAKE_BINARY_DIR}/scenarios"
)

# The combat transcript test runs the same AI-vs-AI fight to verify the
# brevity narration (FOX 3 / spike / splash calls).
target_compile_definitions(test_combat_transcript PRIVATE
    F4_GENERATED_FIXTURES_DIR="${F4_GENERATED_FIXTURES_DIR}"
)

# The DIGI end-to-end mission test runs the build-configured
# digi_full_mission.json scenario (paths inside it are absolute).
target_compile_definitions(test_digi_mission PRIVATE
    F4_SCENARIOS_DIR="${CMAKE_BINARY_DIR}/scenarios"
)

# The FCS trace pipeline test runs the isolated `takeoff_only` and
# `landing_only` scenarios headlessly for a few hundred ticks and asserts
# the CSV trace is produced with the expected structure. Uses the same
# build-configured scenario dir as test_digi_mission.
target_compile_definitions(test_fcs_trace_pipeline PRIVATE
    F4_SCENARIOS_DIR="${CMAKE_BINARY_DIR}/scenarios"
    F4_GENERATED_FIXTURES_DIR="${F4_GENERATED_FIXTURES_DIR}"
    F4_MODELS_DIR="${CMAKE_SOURCE_DIR}/f4-models/tests/fixtures"
)
# The scenario must exist before the test runs.
# Only wire the dependency when the scenario player is actually built
# (it is skipped when F4_BUILD_SCENARIO_PLAYER=OFF — e.g. in headless CI).
if(TARGET f4_scenario_player_lib)
    add_dependencies(test_digi_mission f4_scenario_player_lib)
endif()

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

# The vehicle-spawning test uses the F-16 aircraft config for
# spawn_aircraft_from_squadrons() (FlightModelComponent::init requires
# valid aero tables — empty config would throw).
target_compile_definitions(test_vehicle_spawning PRIVATE
    F4_GENERATED_FIXTURES_DIR="${F4_GENERATED_FIXTURES_DIR}"
)
