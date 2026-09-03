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
# Build-time fixture generation: parse each .dat in fixtures/ and emit a
# corresponding JSON in ${CMAKE_BINARY_DIR}/generated_fixtures/.
#
# These generated JSONs are the contract surface for future f4-data tests:
# when f4-data is built, its tests will load these JSONs (via a CMake
# dependency on the convert_golden_fixtures target) to exercise
# AircraftConfig validation, table lookups, etc.
#
# We generate JSONs at build time (rather than checking them in) so that
# parser improvements automatically propagate to f4-data's test inputs —
# no risk of stale JSONs silently hiding parser regressions.
# ---------------------------------------------------------------------------

# Collect all .dat fixtures
file(GLOB DAT_FIXTURES
    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/*.dat"
)

set(GENERATED_FIXTURES_DIR "${CMAKE_BINARY_DIR}/generated_fixtures")
file(MAKE_DIRECTORY ${GENERATED_FIXTURES_DIR})

# Generate one JSON per .dat
set(generated_jsons "")
foreach(dat_file ${DAT_FIXTURES})
    get_filename_component(base ${dat_file} NAME_WE)
    set(json_file "${GENERATED_FIXTURES_DIR}/${base}.json")
    list(APPEND generated_jsons ${json_file})
    add_custom_command(
        OUTPUT  ${json_file}
        COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED_FIXTURES_DIR}"
        COMMAND dat2json ${dat_file} ${json_file}
        DEPENDS dat2json ${dat_file}
        COMMENT "Converting fixture ${base}.dat -> ${base}.json"
        VERBATIM
    )
endforeach()

# Aggregate target that depends on all generated JSONs (aircraft .dat
# fixtures + the SimData AI fixtures below — f4-data's loader tests
# consume both through this single target).
add_custom_target(convert_golden_fixtures ALL DEPENDS ${generated_jsons})

# ---------------------------------------------------------------------------
# SimData (AI) fixture generation: the mnvrdata/brain/formation fixtures
# (fixtures/simdata/, extracted from the game install's SimData.zip — the
# same conversion path any Falcon 4.0\Zips\SimData.zip takes) are parsed
# into the same generated_fixtures/ directory. Consumers:
#   - f4-data's loader tests (f4.braindata / f4.formdata / f4.mnvrdata)
#   - f4-simulation's formation/brain-archetype scenario wiring (the
#     generated JSONs are the engine-agnostic Data/ side of the pipeline)
# ---------------------------------------------------------------------------
add_custom_command(
    OUTPUT  ${GENERATED_FIXTURES_DIR}/simdata/mnvrdata.json
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED_FIXTURES_DIR}/simdata"
    COMMAND mnvr2json
            ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/simdata/mnvrdata.dat
            ${GENERATED_FIXTURES_DIR}/simdata/mnvrdata.json
    DEPENDS mnvr2json
            ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/simdata/mnvrdata.dat
    COMMENT "Converting fixture simdata/mnvrdata.dat -> simdata/mnvrdata.json"
    VERBATIM
)
add_custom_command(
    OUTPUT  ${GENERATED_FIXTURES_DIR}/simdata/braindata.json
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED_FIXTURES_DIR}/simdata"
    COMMAND brain2json
            ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/simdata/BRAINDAT.brn
            ${GENERATED_FIXTURES_DIR}/simdata/braindata.json
    DEPENDS brain2json
            ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/simdata/BRAINDAT.brn
    COMMENT "Converting fixture simdata/BRAINDAT.brn -> simdata/braindata.json"
    VERBATIM
)
add_custom_command(
    OUTPUT  ${GENERATED_FIXTURES_DIR}/simdata/formdat.json
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED_FIXTURES_DIR}/simdata"
    COMMAND form2json
            ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/simdata/FORMDAT.FIL
            ${GENERATED_FIXTURES_DIR}/simdata/formdat.json
    DEPENDS form2json
            ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/simdata/FORMDAT.FIL
    COMMENT "Converting fixture simdata/FORMDAT.FIL -> simdata/formdat.json"
    VERBATIM
)
add_custom_target(simdata_golden_fixtures ALL
    DEPENDS ${GENERATED_FIXTURES_DIR}/simdata/mnvrdata.json
            ${GENERATED_FIXTURES_DIR}/simdata/braindata.json
            ${GENERATED_FIXTURES_DIR}/simdata/formdat.json)
add_dependencies(convert_golden_fixtures simdata_golden_fixtures)

# ---------------------------------------------------------------------------
# SimData wave-2 fixtures: the VehDef class table, the SENSDATA sensor
# authoring files, and the SIGDATA signature grids (also from the shipped
# SimData.zip — same conversion path). Consumers:
#   - f4-data's loader tests (f4.vehdef / f4.irstdata / f4.rwrdata /
#     f4.visualdata / f4.sigdata)
#   - f4-sensors' SimData consumer tests (RCS grids + RWR parameters)
# ---------------------------------------------------------------------------
add_custom_command(
    OUTPUT  ${GENERATED_FIXTURES_DIR}/simdata/vehdef.json
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED_FIXTURES_DIR}/simdata"
    COMMAND veh2json
            ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/simdata/VehDef/Vehicle.lst
            ${GENERATED_FIXTURES_DIR}/simdata/vehdef.json
    DEPENDS veh2json
            ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/simdata/VehDef/Vehicle.lst
    COMMENT "Converting fixture simdata/VehDef/Vehicle.lst -> simdata/vehdef.json"
    VERBATIM
)
add_custom_command(
    OUTPUT  ${GENERATED_FIXTURES_DIR}/simdata/irstdata.json
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED_FIXTURES_DIR}/simdata"
    COMMAND sens2json irst
            ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/simdata/SENSDATA/IRST/IRST.LST
            ${GENERATED_FIXTURES_DIR}/simdata/irstdata.json
    DEPENDS sens2json
            ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/simdata/SENSDATA/IRST/IRST.LST
    COMMENT "Converting fixture simdata/SENSDATA/IRST -> simdata/irstdata.json"
    VERBATIM
)
add_custom_command(
    OUTPUT  ${GENERATED_FIXTURES_DIR}/simdata/rwrdata.json
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED_FIXTURES_DIR}/simdata"
    COMMAND sens2json rwr
            ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/simdata/SENSDATA/RWR/RWR.LST
            ${GENERATED_FIXTURES_DIR}/simdata/rwrdata.json
    DEPENDS sens2json
            ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/simdata/SENSDATA/RWR/RWR.LST
    COMMENT "Converting fixture simdata/SENSDATA/RWR -> simdata/rwrdata.json"
    VERBATIM
)
add_custom_command(
    OUTPUT  ${GENERATED_FIXTURES_DIR}/simdata/visualdata.json
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED_FIXTURES_DIR}/simdata"
    COMMAND sens2json visual
            ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/simdata/SENSDATA/VISUAL/VISUAL.LST
            ${GENERATED_FIXTURES_DIR}/simdata/visualdata.json
    DEPENDS sens2json
            ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/simdata/SENSDATA/VISUAL/VISUAL.LST
    COMMENT "Converting fixture simdata/SENSDATA/VISUAL -> simdata/visualdata.json"
    VERBATIM
)
add_custom_command(
    OUTPUT  ${GENERATED_FIXTURES_DIR}/simdata/sigdata.json
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED_FIXTURES_DIR}/simdata"
    COMMAND sig2json
            ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/simdata/SIGDATA
            ${GENERATED_FIXTURES_DIR}/simdata/sigdata.json
    DEPENDS sig2json
            ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/simdata/SIGDATA/SIGDATA.LST
    COMMENT "Converting fixture simdata/SIGDATA -> simdata/sigdata.json"
    VERBATIM
)
add_custom_target(simdata_wave2_golden_fixtures ALL
    DEPENDS ${GENERATED_FIXTURES_DIR}/simdata/vehdef.json
            ${GENERATED_FIXTURES_DIR}/simdata/irstdata.json
            ${GENERATED_FIXTURES_DIR}/simdata/rwrdata.json
            ${GENERATED_FIXTURES_DIR}/simdata/visualdata.json
            ${GENERATED_FIXTURES_DIR}/simdata/sigdata.json)
add_dependencies(simdata_golden_fixtures simdata_wave2_golden_fixtures)

# Export the path to f4-data (when that library lands) via a cache variable.
set(F4_GENERATED_FIXTURES_DIR "${GENERATED_FIXTURES_DIR}" CACHE PATH
    "Path to JSON fixtures generated by f4-convert at build time")
