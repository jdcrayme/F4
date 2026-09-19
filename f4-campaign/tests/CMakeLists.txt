# f4-campaign tests
#
# GoogleTest is fetched and the GoogleTest module included from the root
# CMakeLists.txt (via cmake/f4_deps.cmake). This file only registers
# test executables via gtest_discover_tests(), matching every other
# f4-* tests dir.

function(f4_campaign_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE GTest::gtest_main f4-campaign)
    target_compile_features(${name} PRIVATE cxx_std_20)
    # The generated profile fixture (root CMakeLists / f4-campaign
    # CMakeLists own the generation + the cache variable).
    target_compile_definitions(${name} PRIVATE
        F4_MISSION_PROFILES_JSON="${F4_MISSION_PROFILES_JSON}")
    add_dependencies(${name} mission_profiles_fixture)
    gtest_discover_tests(${name} PROPERTIES LABELS "f4-campaign" TIMEOUT 60)
endfunction()

f4_campaign_add_test(test_mission_type      test_mission_type.cpp)
f4_campaign_add_test(test_mission_profiles  test_mission_profiles.cpp)
f4_campaign_add_test(test_flight_intents    test_flight_intents.cpp)

# The golden-summary test drives the M4.7 headless tick over the kunsan
# campaign world fixture (the save1.cam-derived two-team world from
# f4-simulation's tests dir). It also round-trips the summary through
# f4-json's Reader (a PRIVATE dep of f4-campaign — link it explicitly).
f4_campaign_add_test(test_campaign_tick      test_campaign_tick.cpp)
target_link_libraries(test_campaign_tick PRIVATE f4-json)
target_compile_definitions(test_campaign_tick PRIVATE
    F4_SIMULATION_TEST_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/f4-simulation/tests/fixtures")

# C1 — the result ledger (write model, war-loop feedback, write-back).
# Links f4-json explicitly (to_json round-trip, same as the tick test).
f4_campaign_add_test(test_result_ledger   test_result_ledger.cpp)
target_link_libraries(test_result_ledger PRIVATE f4-json)
target_compile_definitions(test_result_ledger PRIVATE
    F4_SIMULATION_TEST_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/f4-simulation/tests/fixtures")

# C3 — the route tranche: threat map (ScoreThreatFast port), the grid
# A* (asearch port), and the route builder (BuildPathToTarget port).
f4_campaign_add_test(test_threat_map      test_threat_map.cpp)
f4_campaign_add_test(test_path_finder     test_path_finder.cpp)
f4_campaign_add_test(test_route_builder   test_route_builder.cpp)

# C4 — the ATM pipeline: the 7 phases (each independently testable —
# M4.2's deliverable), FindBestAir scoring (M4.6), takeoff slot
# scheduling, mission recovery, and the Campaign mode switch. Links
# f4-messaging explicitly (the bus-backed CampaignRig) and carries the
# kunsan fixture path (the mode-switch tests).
f4_campaign_add_test(test_atm             test_atm.cpp)
target_link_libraries(test_atm PRIVATE f4-messaging)
target_compile_definitions(test_atm PRIVATE
    F4_SIMULATION_TEST_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/f4-simulation/tests/fixtures")

# G1 — the ground war engine: war pair, movement, engagement, capture,
# resupply, air-loss pullback, determinism, write-back, ledger block.
# Links f4-json explicitly (the ground-block structural walk).
f4_campaign_add_test(test_ground_war     test_ground_war.cpp)
f4_campaign_add_test(test_flight_aggregate     test_flight_aggregate.cpp)
f4_campaign_add_test(test_ground_targeting     test_ground_targeting.cpp)
# DOM-1 — the verdict over the books (territorial census + the ledger's
# team rows). Links f4-json explicitly (the DTO-side encoder is the
# api's; here only the pure model is pinned).
f4_campaign_add_test(test_war_verdict    test_war_verdict.cpp)
target_link_libraries(test_war_verdict PRIVATE f4-json)
target_link_libraries(test_ground_war PRIVATE f4-json)

# DOM-3 — the personnel layer: AssignPilots (the crew pick + the
# pick-time gate), the rating-decay arm, the ledger's personnel books,
# and the write-back's roster face. Links f4-json (the ledger's to_json
# rides the same lib set).
f4_campaign_add_test(test_campaign_personnel test_campaign_personnel.cpp)
target_link_libraries(test_campaign_personnel PRIVATE f4-json)

# DOM-4 — the airbase-scheduling depth: the Campaign books the ATM's
# schedule denials into the ledger (the saturated pick gate, the
# summary's arm-gated counters, the books' slot_denials total). Links
# f4-json (the summary + the artifact).
f4_campaign_add_test(test_campaign_scheduling test_campaign_scheduling.cpp)
target_link_libraries(test_campaign_scheduling PRIVATE f4-json)

# DOM-5 — the naval tasking wrap: the sea-domain target pool (the
# ranker's hostility/roster/wire-order rules), the family split (the
# pool's ASHIP-only decision vs the strike shape), the ATM arm's
# targeted ASHIP requests, and the Campaign's per-target filing books
# over the kunsan fixture's own 2 task forces. Links f4-json (the
# summary).
f4_campaign_add_test(test_naval_tasking test_naval_tasking.cpp)
target_link_libraries(test_naval_tasking PRIVATE f4-json)
target_compile_definitions(test_naval_tasking PRIVATE
    F4_SIMULATION_TEST_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/f4-simulation/tests/fixtures")

# DOM-6 — the task-force movement engine (the naval GroundWar sibling):
# the snapshot filter, the war-pair gate, the fixed-point walk, the
# arrival snap + hold, the cadence/determinism pins, and the
# WorldState sync's activity gate.
f4_campaign_add_test(test_naval_war test_naval_war.cpp)
