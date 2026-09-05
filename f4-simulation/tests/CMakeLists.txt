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
f4_simulation_add_test(test_simdata_wiring         test_simdata_wiring.cpp)
f4_simulation_add_test(test_campaign_bridge        test_campaign_bridge.cpp)
f4_simulation_add_test(test_campaign_spawner      test_campaign_spawner.cpp)
f4_simulation_add_test(test_campaign_result_sink  test_campaign_result_sink.cpp)
f4_simulation_add_test(test_feature_spawning       test_feature_spawning.cpp)
f4_simulation_add_test(test_vehicle_spawning       test_vehicle_spawning.cpp)
f4_simulation_add_test(test_bubble_manager         test_bubble_manager.cpp)
f4_simulation_add_test(test_frames                 test_frames.cpp)
f4_simulation_add_test(test_real_layout_derivation test_real_layout_derivation.cpp)
f4_simulation_add_test(test_digi_mission           test_digi_mission.cpp)
f4_simulation_add_test(test_intercept_convergence    test_intercept_convergence.cpp)
f4_simulation_add_test(test_fcs_trace_pipeline     test_fcs_trace_pipeline.cpp)
f4_simulation_add_test(test_combat_integration     test_combat_integration.cpp)
f4_simulation_add_test(test_combat_transcript      test_combat_transcript.cpp)
f4_simulation_add_test(test_register_aircraft      test_register_aircraft.cpp)
f4_simulation_add_test(test_campaign_session       test_campaign_session.cpp)

# V-THREAD + V-3DLIVE (the campaign's own thread + the camera bubble):
# the runner test drives a real session from the worker thread and pins
# the view-bubble deaggregation contract (paused session, camera-driven).
f4_simulation_add_test(test_campaign_session_runner test_campaign_session_runner.cpp)
target_compile_definitions(test_campaign_session_runner PRIVATE
    F4_SIMULATION_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    F4_SOURCE_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/f4-world-convert/tests/fixtures"
    F4_GENERATED_FIXTURES_DIR="${F4_GENERATED_FIXTURES_DIR}"
    F4_MISSION_PROFILES_JSON="${F4_MISSION_PROFILES_JSON}"
)
add_dependencies(test_campaign_session_runner mission_profiles_fixture)

# REGRESSION (Start Session access violation): the Simulation owns the
# ClassTable the BubbleManager borrows for its lifetime — the old code
# handed it a stack local that died at init_bubble_manager() return.
# Needs FALCON4.ct, the generated F-16 config, and KoreaObj.HDR/LOD for
# the vehicle models the deagg path resolves.
f4_simulation_add_test(test_simulation_lifetime  test_simulation_lifetime.cpp)
target_compile_definitions(test_simulation_lifetime PRIVATE
    F4_SOURCE_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/f4-world-convert/tests/fixtures"
    F4_GENERATED_FIXTURES_DIR="${F4_GENERATED_FIXTURES_DIR}"
    F4_MODELS_DIR="${CMAKE_SOURCE_DIR}/f4-models/tests/fixtures"
)
add_dependencies(test_simulation_lifetime convert_golden_fixtures)

# The register_aircraft test hand-initializes a FlightModelComponent from
# the generated F-16 config fixture (real aero tables).
target_compile_definitions(test_register_aircraft PRIVATE
    F4_GENERATED_FIXTURES_DIR="${F4_GENERATED_FIXTURES_DIR}"
)

# The campaign session test (V-CAMP): the live-loop composition over the
# kunsan fixture + the same generated fixtures campaign_qc consumes.
target_compile_definitions(test_campaign_session PRIVATE
    F4_SIMULATION_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    F4_SOURCE_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/f4-world-convert/tests/fixtures"
    F4_GENERATED_FIXTURES_DIR="${F4_GENERATED_FIXTURES_DIR}"
    F4_MISSION_PROFILES_JSON="${F4_MISSION_PROFILES_JSON}"
)
add_dependencies(test_campaign_session mission_profiles_fixture)

# C5 — the 24-hour war harness: the long-horizon acceptance rig over
# the routed kunsan fixture (compressed horizons; the same generated
# fixtures the session and campaign_qc tests consume).
f4_simulation_add_test(test_campaign_war_harness test_campaign_war_harness.cpp)
target_compile_definitions(test_campaign_war_harness PRIVATE
    F4_SIMULATION_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    F4_SOURCE_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/f4-world-convert/tests/fixtures"
    F4_GENERATED_FIXTURES_DIR="${F4_GENERATED_FIXTURES_DIR}"
    F4_MISSION_PROFILES_JSON="${F4_MISSION_PROFILES_JSON}"
)
add_dependencies(test_campaign_war_harness mission_profiles_fixture)

# C6 — arming the campaign flights: the arm mechanics (role doctrine,
# component set, policy handoff, idempotence), the Simulation surface
# (late-spawn arm + counters), and the ARMED war over the routed kunsan
# rig (compressed acceptance: verdicts green + determinism + armed).
# Consumes the same generated fixtures as the war harness test plus
# the simdata braindata.json the doctrine's defensive roles read.
f4_simulation_add_test(test_campaign_combat test_campaign_combat.cpp)
target_compile_definitions(test_campaign_combat PRIVATE
    F4_SIMULATION_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    F4_SOURCE_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/f4-world-convert/tests/fixtures"
    F4_GENERATED_FIXTURES_DIR="${F4_GENERATED_FIXTURES_DIR}"
    F4_MISSION_PROFILES_JSON="${F4_MISSION_PROFILES_JSON}"
)
add_dependencies(test_campaign_combat mission_profiles_fixture
                                   simdata_golden_fixtures)

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
target_compile_definitions(test_intercept_convergence PRIVATE
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

# The B.3 campaign spawner test drives the emit_flight_intents → bus →
# CampaignSimSpawner loop and needs the same generated F-16 config
# fixture for FlightModelComponent::init.
target_compile_definitions(test_campaign_spawner PRIVATE
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

# The SimData wiring test drives the scenario JSON's brain_profile /
# formation through initialize() into the spawned brains: it needs the
# generated F-16 config AND the generated SimData JSONs (braindata.json
# / formdat.json — f4-convert converts the shipped BRAINDAT.brn /
# FORMDAT.FIL fixtures at build time). The target dependency guarantees
# those files exist before the test binary runs.
target_compile_definitions(test_simdata_wiring PRIVATE
    F4_GENERATED_FIXTURES_DIR="${F4_GENERATED_FIXTURES_DIR}"
)
if(TARGET simdata_golden_fixtures)
    add_dependencies(test_simdata_wiring simdata_golden_fixtures)
endif()
if(TARGET convert_golden_fixtures)
    add_dependencies(test_simdata_wiring convert_golden_fixtures)
endif()

# The C1 result-sink test needs the generated f16.json fixture (the
# spawn-path origin-stamp case) — same define as the combat tests.
target_compile_definitions(test_campaign_result_sink PRIVATE
    F4_GENERATED_FIXTURES_DIR="${F4_GENERATED_FIXTURES_DIR}")
