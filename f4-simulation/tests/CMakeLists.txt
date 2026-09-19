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
    # Tier 2.2: every f4-simulation test gets a label by default so
    # `ctest -L f4-simulation` / `ctest -LE slow` work. Pass LABELS/TIMEOUT
    # to override for the slow harness binaries (see test_campaign_init_wars
    # + test_campaign_verdict below).
    cmake_parse_arguments(ARG "" "TIMEOUT" "LABELS" ${ARGN})
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE GTest::gtest_main f4-simulation)
    target_compile_features(${name} PRIVATE cxx_std_20)
    if(NOT ARG_LABELS)
        set(ARG_LABELS "f4-simulation")
    endif()
    if(NOT ARG_TIMEOUT)
        set(ARG_TIMEOUT 60)
    endif()
    gtest_discover_tests(${name} PROPERTIES LABELS "${ARG_LABELS}" TIMEOUT ${ARG_TIMEOUT})
endfunction()

f4_simulation_add_test(test_weather_system          test_weather_system.cpp)
f4_simulation_add_test(test_environment_wiring      test_environment_wiring.cpp)
target_compile_definitions(test_environment_wiring PRIVATE
    F4_SOURCE_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/Data/Aircraft"
)
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
f4_simulation_add_test(test_aar_e2e                 test_aar_e2e.cpp)   # Tranche D (AAR)
f4_simulation_add_test(test_formation_acceptance   test_formation_acceptance.cpp)  # Tranche C
f4_simulation_add_test(test_intercept_convergence    test_intercept_convergence.cpp)
f4_simulation_add_test(test_fcs_trace_pipeline     test_fcs_trace_pipeline.cpp)
f4_simulation_add_test(test_fidelity_tiers         test_fidelity_tiers.cpp)   # FID-1..4
target_compile_definitions(test_fidelity_tiers PRIVATE
    F4_SOURCE_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/f4-world-convert/tests/fixtures"
    F4_GENERATED_FIXTURES_DIR="${F4_GENERATED_FIXTURES_DIR}"
    F4_MISSION_PROFILES_JSON="${F4_MISSION_PROFILES_JSON}"
)
f4_simulation_add_test(test_campaign_session_host  test_campaign_session_host.cpp)  # CAMP-HOST-1/2: the engine-backed host
target_compile_definitions(test_campaign_session_host PRIVATE
    F4_SIMULATION_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    F4_SOURCE_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/f4-world-convert/tests/fixtures"
    F4_GENERATED_FIXTURES_DIR="${F4_GENERATED_FIXTURES_DIR}"
    F4_MISSION_PROFILES_JSON="${F4_MISSION_PROFILES_JSON}"
)
add_dependencies(test_campaign_session_host mission_profiles_fixture)
f4_simulation_add_test(test_campaign_commands      test_campaign_commands.cpp)  # CAMP-CMD-2: the retask/abort/priority gates
target_compile_definitions(test_campaign_commands PRIVATE
    F4_SIMULATION_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    F4_SOURCE_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/f4-world-convert/tests/fixtures"
    F4_GENERATED_FIXTURES_DIR="${F4_GENERATED_FIXTURES_DIR}"
    F4_MISSION_PROFILES_JSON="${F4_MISSION_PROFILES_JSON}"
)
add_dependencies(test_campaign_commands mission_profiles_fixture)
f4_simulation_add_test(test_fidelity_combat       test_fidelity_combat.cpp)  # FID-5
target_compile_definitions(test_fidelity_combat PRIVATE
    F4_SOURCE_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/f4-world-convert/tests/fixtures"
    F4_GENERATED_FIXTURES_DIR="${F4_GENERATED_FIXTURES_DIR}"
    F4_MISSION_PROFILES_JSON="${F4_MISSION_PROFILES_JSON}"
)
f4_simulation_add_test(test_aggregate_fm_divergence test_aggregate_fm_divergence.cpp)  # FID-5 (the FID-2 deferral)
target_compile_definitions(test_aggregate_fm_divergence PRIVATE
    F4_SOURCE_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/f4-world-convert/tests/fixtures"
    F4_GENERATED_FIXTURES_DIR="${F4_GENERATED_FIXTURES_DIR}"
    F4_MISSION_PROFILES_JSON="${F4_MISSION_PROFILES_JSON}"
)
f4_simulation_add_test(test_combat_integration     test_combat_integration.cpp)
# Task 64: the real-data signature fixture (the aspect-grid stem the
# binding tests resolve against).
target_compile_definitions(test_combat_integration PRIVATE
    F4_SIMULATION_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)
f4_simulation_add_test(test_countermeasure_e2e     test_countermeasure_e2e.cpp)
target_compile_definitions(test_countermeasure_e2e PRIVATE
    F4_GENERATED_FIXTURES_DIR="${F4_GENERATED_FIXTURES_DIR}"
    F4_SOURCE_DATA_DIR="${CMAKE_SOURCE_DIR}/Data"
)
f4_simulation_add_test(test_combat_transcript      test_combat_transcript.cpp)
f4_simulation_add_test(test_register_aircraft      test_register_aircraft.cpp)
f4_simulation_add_test(test_spinner_roster         test_spinner_roster.cpp)
f4_simulation_add_test(test_campaign_session       test_campaign_session.cpp)

# NOTE (CAMP-HOST-3): the V-THREAD runner left the engine — pacing is a
# HOST concern (plan §2.2), and the runner is host-side composition. It
# lives in f4-world-viewer now (campaign_client_runner), over the
# ICampaignSession contract, with its tests beside it. The engine no
# longer ships a thread wrapper.

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

# CAMP-OPT-1: the spinner-roster pins — same fixture wiring as
# test_register_aircraft (the FM-dependent cases load the generated
# F-16 config; without it the tests skip rather than fail).
target_compile_definitions(test_spinner_roster PRIVATE
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

# CAMP-INIT-1 — the war gates over GENERATED saves: each world is the
# decode of a campinit-generated .cam (build-time fixtures), so the C5
# harness runs on exactly the bytes the Task-70 encoder stack produced.
# The 24-hour run on the small war is the acceptance gate; medium /
# large / twinwars pin determinism and the two-war-pair bed.
f4_simulation_add_test(test_campaign_init_wars test_campaign_init_wars.cpp
    LABELS slow TIMEOUT 900)  # Tier 3.1: 170 s real-time — run via `ctest -L slow`
target_compile_definitions(test_campaign_init_wars PRIVATE
    F4_CAMPINIT_FIXTURES_DIR="${F4_CAMPINIT_FIXTURES_DIR}"
    F4_SOURCE_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/f4-world-convert/tests/fixtures"
    F4_GENERATED_FIXTURES_DIR="${F4_GENERATED_FIXTURES_DIR}"
    F4_MISSION_PROFILES_JSON="${F4_MISSION_PROFILES_JSON}"
)
add_dependencies(test_campaign_init_wars campinit_fixtures mission_profiles_fixture)

# Tier 3.1 — the FAST tier counterparts: the same 4 wars at 60x accel.
# Same fixtures, same generated worlds, same expect_green pins — but
# WarHarnessOptions::speed = 60.0 (the acceptance preset from FID_OPT /
# FIDELITY_TIERS), so the 170 s / 25 s / 23 s / 25 s real-time tests
# become ~3 s each. Run via `ctest -LE slow` (the default fast tier);
# the nightly 1x variants above run via `ctest -L slow`.
f4_simulation_add_test(test_campaign_init_wars_fast test_campaign_init_wars_fast.cpp
    LABEL "f4-simulation" TIMEOUT 120)  # 4 wars at 60x accel (~5s each, 120s headroom under -j4 load)
target_compile_definitions(test_campaign_init_wars_fast PRIVATE
    F4_CAMPINIT_FIXTURES_DIR="${F4_CAMPINIT_FIXTURES_DIR}"
    F4_SOURCE_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/f4-world-convert/tests/fixtures"
    F4_GENERATED_FIXTURES_DIR="${F4_GENERATED_FIXTURES_DIR}"
    F4_MISSION_PROFILES_JSON="${F4_MISSION_PROFILES_JSON}"
)
add_dependencies(test_campaign_init_wars_fast campinit_fixtures mission_profiles_fixture)

# DOM-1 — the verdict's war gates: the generated small war over the
# HOST (ground war armed) — a capture raises the verdict event, and the
# query answers with the stream's last coarse state.
f4_simulation_add_test(test_campaign_verdict test_campaign_verdict.cpp
    LABELS slow TIMEOUT 300)  # 56 s real-time — no speed knob (EngineSessionHost path)
target_compile_definitions(test_campaign_verdict PRIVATE
    F4_CAMPINIT_FIXTURES_DIR="${F4_CAMPINIT_FIXTURES_DIR}"
    F4_SOURCE_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/f4-world-convert/tests/fixtures"
    F4_GENERATED_FIXTURES_DIR="${F4_GENERATED_FIXTURES_DIR}"
    F4_MISSION_PROFILES_JSON="${F4_MISSION_PROFILES_JSON}"
)
add_dependencies(test_campaign_verdict campinit_fixtures mission_profiles_fixture)

# DOM-2 — the supply chain's war gates: the live objectives query (the
# DOM-1 seam closes), the sourced resupply pool, the repair cadence end
# to end (seeded damage heals, events publish, the entity mirror joins,
# the write-backs land), and the golden identity with every knob off.
f4_simulation_add_test(test_campaign_supply test_campaign_supply.cpp)
target_compile_definitions(test_campaign_supply PRIVATE
    F4_SIMULATION_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    F4_SOURCE_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/f4-world-convert/tests/fixtures"
    F4_GENERATED_FIXTURES_DIR="${F4_GENERATED_FIXTURES_DIR}"
    F4_MISSION_PROFILES_JSON="${F4_MISSION_PROFILES_JSON}"
)
add_dependencies(test_campaign_supply mission_profiles_fixture)

# DOM-3 — the personnel layer's sim-side gates: the crews ride the
# intents/books/events one-for-one, the squadrons query serves the
# personnel face, the golden identity with the arms off, and the
# write-back gate (a draws-only run writes no roster).
f4_simulation_add_test(test_campaign_personnel_session test_campaign_personnel_session.cpp)
target_compile_definitions(test_campaign_personnel_session PRIVATE
    F4_SIMULATION_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    F4_SOURCE_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/f4-world-convert/tests/fixtures"
    F4_GENERATED_FIXTURES_DIR="${F4_GENERATED_FIXTURES_DIR}"
    F4_MISSION_PROFILES_JSON="${F4_MISSION_PROFILES_JSON}"
)
add_dependencies(test_campaign_personnel_session mission_profiles_fixture)

# DOM-4 — the scheduling depth's sim-side gates: the slots ride the
# intents and the airfield-ops gate arms on them, the airfields query
# serves the schedule books, the golden identity with the arm off, and
# the event/books parity (plus the armed run's determinism pin).
f4_simulation_add_test(test_campaign_scheduling_session test_campaign_scheduling_session.cpp)
target_compile_definitions(test_campaign_scheduling_session PRIVATE
    F4_SIMULATION_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    F4_SOURCE_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/f4-world-convert/tests/fixtures"
    F4_GENERATED_FIXTURES_DIR="${F4_GENERATED_FIXTURES_DIR}"
    F4_MISSION_PROFILES_JSON="${F4_MISSION_PROFILES_JSON}"
)
add_dependencies(test_campaign_scheduling_session mission_profiles_fixture)

# DOM-5 — the naval wrap's sim-side gates: the taskforces query serves
# the wire's own rows with the armed run's filing books, the filings
# ride the mission_filed stream one-for-one, the golden identity with
# the arm off (the same rows, the honest zeros), and the armed run's
# determinism pin.
f4_simulation_add_test(test_campaign_naval_session test_campaign_naval_session.cpp)
target_compile_definitions(test_campaign_naval_session PRIVATE
    F4_SIMULATION_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    F4_SOURCE_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/f4-world-convert/tests/fixtures"
    F4_GENERATED_FIXTURES_DIR="${F4_GENERATED_FIXTURES_DIR}"
    F4_MISSION_PROFILES_JSON="${F4_MISSION_PROFILES_JSON}"
)
add_dependencies(test_campaign_naval_session mission_profiles_fixture)

# P7 — the strategy layer's sim-side integration: the station contract
# ride (route → plan → brain), the post-arm RoE gates, and the
# strategy-armed session (stationed CAPs spawn with racetrack plans;
# the ON-state determinism pin).
f4_simulation_add_test(test_strategy_layer test_strategy_layer.cpp)
target_compile_definitions(test_strategy_layer PRIVATE
    F4_SIMULATION_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    F4_SOURCE_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/f4-world-convert/tests/fixtures"
    F4_GENERATED_FIXTURES_DIR="${F4_GENERATED_FIXTURES_DIR}"
    F4_MISSION_PROFILES_JSON="${F4_MISSION_PROFILES_JSON}"
)
add_dependencies(test_strategy_layer mission_profiles_fixture)

# M4 — the BVR intercept acceptance harness: runs the shipped
# bvr_intercept.json scenario headless through BvrInterceptHarness,
# pins the four verdicts (deterministic / engagement_completed /
# roster_bounded / fight_alive), the MD5 certificate, and the
# engagement_summary block. Consumes the build-configured scenario
# (F4_SCENARIOS_DIR) + the generated F-16 config (the scenario's
# @F4_AIRCRAFT_CONFIG@ resolves to it at configure time). See
# Docs/archive/COMBAT_CHAIN_M4_PLAN.md §4 item 9 for the test rig.
f4_simulation_add_test(test_bvr_intercept_harness test_bvr_intercept_harness.cpp)
target_compile_definitions(test_bvr_intercept_harness PRIVATE
    F4_SCENARIOS_DIR="${CMAKE_BINARY_DIR}/scenarios"
    F4_GENERATED_FIXTURES_DIR="${F4_GENERATED_FIXTURES_DIR}"
)
if(TARGET convert_golden_fixtures)
    add_dependencies(test_bvr_intercept_harness convert_golden_fixtures)
endif()

# M5a — the WVR / guns merge acceptance harness: runs the shipped
# wvr_merge.json + guns_merge.json scenarios headless through
# WvrMergeHarness, pins the four verdicts + the MD5 certificate + the
# band-event round-trip, the refusal contract, the band-boundary
# fight_alive diagnostic, and the 2v2 multi-flight acceptance (M4 §6's
# deferral). See Docs/archive/COMBAT_CHAIN_M5_PLAN.md for the test rig.
f4_simulation_add_test(test_wvr_merge_harness test_wvr_merge_harness.cpp)
target_compile_definitions(test_wvr_merge_harness PRIVATE
    F4_SCENARIOS_DIR="${CMAKE_BINARY_DIR}/scenarios"
    F4_GENERATED_FIXTURES_DIR="${F4_GENERATED_FIXTURES_DIR}"
)
if(TARGET convert_golden_fixtures)
    add_dependencies(test_wvr_merge_harness convert_golden_fixtures)
endif()

# M5b — the air-to-ground strike acceptance harness (the A/G rung both
# COMBAT_CHAIN_M4_PLAN §6 and COMBAT_CHAIN_M5_PLAN §6 deferred). Pins the
# five verdicts + the MD5 certificate + the ledger cross-check over the
# shipped ground_strike.json scenario, the refusal contract, the
# no-ordnance arming stall, the no-delivery-waypoint abort, and the
# single-run determinism skip. See test_ground_strike_harness.cpp.
f4_simulation_add_test(test_ground_strike_harness test_ground_strike_harness.cpp)
target_compile_definitions(test_ground_strike_harness PRIVATE
    F4_SCENARIOS_DIR="${CMAKE_BINARY_DIR}/scenarios"
    F4_GENERATED_FIXTURES_DIR="${F4_GENERATED_FIXTURES_DIR}"
)
if(TARGET convert_golden_fixtures)
    add_dependencies(test_ground_strike_harness convert_golden_fixtures)
endif()

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
# Tranche D (AAR): the E2E test runs the build-configured
# tanker_track.json scenario (same scenarios dir as digi_full_mission).
target_compile_definitions(test_aar_e2e PRIVATE
    F4_SCENARIOS_DIR="${CMAKE_BINARY_DIR}/scenarios"
)
# Tranche C (formation): the acceptance test runs the build-configured
# formation_acceptance.json scenario. The wingmen reference FORMDAT
# formations (trail/wedge/ladder), so the test depends on the generated
# formdat.json fixture (simdata_golden_fixtures) like test_simdata_wiring.
target_compile_definitions(test_formation_acceptance PRIVATE
    F4_SCENARIOS_DIR="${CMAKE_BINARY_DIR}/scenarios"
    F4_GENERATED_FIXTURES_DIR="${F4_GENERATED_FIXTURES_DIR}"
)
if(TARGET simdata_golden_fixtures)
    add_dependencies(test_formation_acceptance simdata_golden_fixtures)
endif()
if(TARGET convert_golden_fixtures)
    add_dependencies(test_formation_acceptance convert_golden_fixtures)
endif()
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
    add_dependencies(test_aar_e2e f4_scenario_player_lib)
    add_dependencies(test_formation_acceptance f4_scenario_player_lib)
endif()

# Point the scenario loader test at the bundled fixture so it can load a real
# JSON file without depending on the install layout. Tranche 0d: also expose
# F4_SOURCE_DIR so the AssetModelRefSkipsBinaryLoad test can resolve
# Data/Aircraft/f16.json from the source tree.
target_compile_definitions(test_scenario_loader PRIVATE
    F4_SIMULATION_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    F4_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
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

add_executable(run_scenario run_scenario.cpp)
target_link_libraries(run_scenario PRIVATE f4-simulation GTest::gtest)
target_compile_definitions(run_scenario PRIVATE
    F4_SCENARIOS_DIR="${CMAKE_BINARY_DIR}/scenarios")
