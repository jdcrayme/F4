# GoogleTest is fetched and GoogleTest module included from the root
# CMakeLists.txt (via cmake/f4_deps.cmake). This file only registers
# test executables via gtest_discover_tests().

# Each test target links ONLY against f4-convert. f4-convert transitively
# pulls in nlohmann_json, but no other f4-* library. This is the
# zero-domain-coupling invariant: the parser is fully testable in isolation.
function(f4_convert_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE GTest::gtest_main f4-convert)
    gtest_discover_tests(${name})
endfunction()

f4_convert_add_test(test_dat_parser  test_dat_parser.cpp)
f4_convert_add_test(test_json_io     test_json_io.cpp)
f4_convert_add_test(test_round_trip  test_round_trip.cpp)
f4_convert_add_test(test_mnvr_parser   test_mnvr_parser.cpp)
f4_convert_add_test(test_brain_parser  test_brain_parser.cpp)
f4_convert_add_test(test_formation_parser test_formation_parser.cpp)
f4_convert_add_test(test_veh_parser       test_veh_parser.cpp)
f4_convert_add_test(test_sensor_parser    test_sensor_parser.cpp)
f4_convert_add_test(test_signature_parser test_signature_parser.cpp)

# Pass paths to test executables via compile definitions.
target_compile_definitions(test_dat_parser PRIVATE
    F4_CONVERT_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
target_compile_definitions(test_json_io PRIVATE
    F4_CONVERT_TEST_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}")
target_compile_definitions(test_round_trip PRIVATE
    F4_CONVERT_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    F4_CONVERT_TEST_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}")
foreach(t test_mnvr_parser test_brain_parser test_formation_parser)
    target_compile_definitions(${t} PRIVATE
        F4_CONVERT_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
        F4_CONVERT_TEST_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}")
endforeach()
foreach(t test_veh_parser test_sensor_parser test_signature_parser)
    target_compile_definitions(${t} PRIVATE
        F4_CONVERT_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
        F4_CONVERT_TEST_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}")
endforeach()

# ---------------------------------------------------------------------------
# Build-time fixture generation: MOVED to the parent f4-convert/
# CMakeLists.txt (library level). Those JSONs are RUNTIME inputs for the
# interactive apps (world viewer campaign session, scenario player,
# campaign_qc) — not test-only byproducts — so a single-target build of
# any app must produce them. Target names (convert_golden_fixtures,
# simdata_golden_fixtures, simdata_wave2_golden_fixtures), output paths
# (build/generated_fixtures/...), and generation commands are unchanged;
# only their defining file moved. The trailing duplicate
# F4_GENERATED_FIXTURES_DIR re-set that used to close this file is gone
# too (the ROOT CMakeLists owns that cache var, FORCE-set before any
# add_subdirectory()).
# ---------------------------------------------------------------------------
