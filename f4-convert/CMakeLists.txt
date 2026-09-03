cmake_minimum_required(VERSION 3.20)
project(f4-convert VERSION 0.1.0 LANGUAGES CXX DESCRIPTION "Legacy .dat to JSON aircraft data converter for the F4 libraries")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# f4-convert library (parser + JSON I/O).
#
# nlohmann/json is now fetched in cmake/f4_deps.cmake (at the root level)
# so that f4-data can link it too, despite being configured before f4-convert.
#
# Depends on f4-data for the AircraftConfig struct AND for the canonical
# JSON writer/reader (f4-data::writeConfig / loadConfig). f4-convert's
# json_io.cpp is a thin adapter that delegates to f4-data for the actual
# serialization; only diffConfigs() is unique to f4-convert (f4-data does
# no comparison). The previous duplication (two copies of the JSON
# writer/reader that "MUST stay in sync") was eliminated in the 2025
# cleanup pass.
# ---------------------------------------------------------------------------
add_library(f4-convert STATIC
    src/dat_parser.cpp
    src/brain_parser.cpp
    src/formation_parser.cpp
    src/mnvr_parser.cpp
    src/sensor_parser.cpp
    src/signature_parser.cpp
    src/veh_parser.cpp
    src/json_io.cpp
)
target_link_libraries(f4-convert PUBLIC f4_warnings)

target_include_directories(f4-convert PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(f4-convert PUBLIC
    f4-data
PRIVATE
    f4-math
)

# ---------------------------------------------------------------------------
# CLI tools. Each is a thin main() that calls into the f4-convert library.
# Built by default; can be disabled with -DF4_CONVERT_BUILD_CLI=OFF.
#
# NOTE: declared BEFORE the fixture generation below — the generation
# custom commands run these tools (dat2json, mnvr2json, ...).
# ---------------------------------------------------------------------------
option(F4_CONVERT_BUILD_CLI "Build f4-convert CLI tools" ON)
if(F4_CONVERT_BUILD_CLI)
    add_subdirectory(cli)
endif()

# ---------------------------------------------------------------------------
# Build-time golden-fixture generation — HOISTED out of tests/ (V-CAMP
# follow-up): these JSONs stopped being test-only byproducts the moment
# the interactive apps consumed them at RUNTIME:
#
#   generated_fixtures/f16.json        — the aircraft config every
#                                        campaign-session / scenario
#                                        aircraft flies (world viewer
#                                        "Start Session", scenario
#                                        player, campaign_qc)
#   generated_fixtures/simdata/*.json — brain/formation/mnvr/veh/sensor
#                                        data (scenario defaults, loader
#                                        tests)
#
# A single-target build of any consumer app ("cmake --build build
# --target f4-world-viewer", or F5 on the viewer project in Visual
# Studio) skips ALL custom targets — the fixture generation used to
# live under tests/ (itself skipped with -DF4_CONVERT_BUILD_TESTS=OFF),
# so the apps launched without their runtime inputs and "Start
# Session" failed with "aircraft config not found". Generating the
# fixtures at the LIBRARY level + wiring add_dependencies() on the app
# targets (see f4-world-viewer / f4-scenario-player / f4-simulation
# CMakeLists) makes building the app the ONLY preparation step.
#
# Target names, output paths, and the generation commands are unchanged
# from the old tests/ block — every existing add_dependencies() consumer
# (f4-data / f4-flight-model / f4-ai / f4-simulation tests) keeps
# working as-is.
#
# Gated on the CLI tools: they ARE the generators (same coupling the old
# tests/ block had — a config with CLI OFF never produced fixtures
# either). CLI is ON by default; every app consumer below guards its
# dependency with if(TARGET ...).
# ---------------------------------------------------------------------------
if(F4_CONVERT_BUILD_CLI)

# Collect all .dat fixtures
file(GLOB DAT_FIXTURES
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/*.dat"
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
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/simdata/mnvrdata.dat
            ${GENERATED_FIXTURES_DIR}/simdata/mnvrdata.json
    DEPENDS mnvr2json
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/simdata/mnvrdata.dat
    COMMENT "Converting fixture simdata/mnvrdata.dat -> simdata/mnvrdata.json"
    VERBATIM
)
add_custom_command(
    OUTPUT  ${GENERATED_FIXTURES_DIR}/simdata/braindata.json
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED_FIXTURES_DIR}/simdata"
    COMMAND brain2json
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/simdata/BRAINDAT.brn
            ${GENERATED_FIXTURES_DIR}/simdata/braindata.json
    DEPENDS brain2json
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/simdata/BRAINDAT.brn
    COMMENT "Converting fixture simdata/BRAINDAT.brn -> simdata/braindata.json"
    VERBATIM
)
add_custom_command(
    OUTPUT  ${GENERATED_FIXTURES_DIR}/simdata/formdat.json
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED_FIXTURES_DIR}/simdata"
    COMMAND form2json
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/simdata/FORMDAT.FIL
            ${GENERATED_FIXTURES_DIR}/simdata/formdat.json
    DEPENDS form2json
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/simdata/FORMDAT.FIL
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
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/simdata/VehDef/Vehicle.lst
            ${GENERATED_FIXTURES_DIR}/simdata/vehdef.json
    DEPENDS veh2json
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/simdata/VehDef/Vehicle.lst
    COMMENT "Converting fixture simdata/VehDef -> simdata/vehdef.json"
    VERBATIM
)
add_custom_command(
    OUTPUT  ${GENERATED_FIXTURES_DIR}/simdata/irstdata.json
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED_FIXTURES_DIR}/simdata"
    COMMAND sens2json irst
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/simdata/SENSDATA/IRST/IRST.LST
            ${GENERATED_FIXTURES_DIR}/simdata/irstdata.json
    DEPENDS sens2json
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/simdata/SENSDATA/IRST/IRST.LST
    COMMENT "Converting fixture simdata/SENSDATA/IRST -> simdata/irstdata.json"
    VERBATIM
)
add_custom_command(
    OUTPUT  ${GENERATED_FIXTURES_DIR}/simdata/rwrdata.json
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED_FIXTURES_DIR}/simdata"
    COMMAND sens2json rwr
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/simdata/SENSDATA/RWR/RWR.LST
            ${GENERATED_FIXTURES_DIR}/simdata/rwrdata.json
    DEPENDS sens2json
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/simdata/SENSDATA/RWR/RWR.LST
    COMMENT "Converting fixture simdata/SENSDATA/RWR -> simdata/rwrdata.json"
    VERBATIM
)
add_custom_command(
    OUTPUT  ${GENERATED_FIXTURES_DIR}/simdata/visualdata.json
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED_FIXTURES_DIR}/simdata"
    COMMAND sens2json visual
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/simdata/SENSDATA/VISUAL/VISUAL.LST
            ${GENERATED_FIXTURES_DIR}/simdata/visualdata.json
    DEPENDS sens2json
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/simdata/SENSDATA/VISUAL/VISUAL.LST
    COMMENT "Converting fixture simdata/SENSDATA/VISUAL -> simdata/visualdata.json"
    VERBATIM
)
add_custom_command(
    OUTPUT  ${GENERATED_FIXTURES_DIR}/simdata/sigdata.json
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED_FIXTURES_DIR}/simdata"
    COMMAND sig2json
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/simdata/SIGDATA
            ${GENERATED_FIXTURES_DIR}/simdata/sigdata.json
    DEPENDS sig2json
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/simdata/SIGDATA/SIGDATA.LST
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

# F4_GENERATED_FIXTURES_DIR is FORCE-set in the ROOT CMakeLists before
# any add_subdirectory() (first-configure visibility for f4-data/tests);
# the old duplicate re-set that lived at the bottom of the tests/ block
# is gone — one owner, same value.

endif() # F4_CONVERT_BUILD_CLI

# ---------------------------------------------------------------------------
# Tests. The build-time fixture generation USED to live in tests/
# CMakeLists.txt — it now lives above (library level; see the hoist
# comment). This file registers test executables only.
# ---------------------------------------------------------------------------
option(F4_CONVERT_BUILD_TESTS "Build f4-convert unit tests" ON)
if(F4_CONVERT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Install the Rosetta Stone as a data file alongside the library.
# This is the contract between f4-convert and f4-data: both load this JSON
# to know what each AuxAeroData key means.
# ---------------------------------------------------------------------------
install(FILES rosetta/auxaero_field_map.json
        DESTINATION share/f4/rosetta)
