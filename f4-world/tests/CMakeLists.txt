# GoogleTest is fetched and GoogleTest module included from the root
# CMakeLists.txt (via cmake/f4_deps.cmake). This file only registers
# test executables via gtest_discover_tests().

# Generate the real world JSON fixture from save1.cam at build time, so the
# f4-world tests can validate end-to-end (.cam -> JSON -> typed WorldState)
# against real campaign data.
set(WORLD_CAM_FIXTURE "${CMAKE_CURRENT_SOURCE_DIR}/../../f4-world-convert/tests/fixtures/save1.cam")
set(WORLD_JSON_FIXTURE "${CMAKE_BINARY_DIR}/generated_world_fixtures/save1.json")
add_custom_command(
    OUTPUT ${WORLD_JSON_FIXTURE}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/generated_world_fixtures
    COMMAND cam2json ${WORLD_CAM_FIXTURE} ${WORLD_JSON_FIXTURE}
    DEPENDS cam2json ${WORLD_CAM_FIXTURE}
    COMMENT "Generating save1.json from save1.cam (end-to-end fixture)"
)

function(f4_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE GTest::gtest_main f4-world)
    target_compile_definitions(${name} PRIVATE
        WORLD_JSON_FIXTURE="${WORLD_JSON_FIXTURE}")
    gtest_discover_tests(${name})
endfunction()

f4_add_test(test_world_state  test_world_state.cpp)
f4_add_test(test_world_loader test_world_loader.cpp)
f4_add_test(test_interface_bridge test_interface_bridge.cpp)

# The asset-pipeline test exercises load_terrain_via_assets() — it links
# f4-assets directly (f4-world links it PRIVATE, since WorldState only
# forward-declares AssetRoot in the header) and uses the AssetRoot API
# to set up a synthetic Data/ tree in the temp dir.
add_executable(test_world_assets test_world_assets.cpp)
target_link_libraries(test_world_assets PRIVATE GTest::gtest_main f4-world f4-assets)
target_compile_definitions(test_world_assets PRIVATE
    WORLD_JSON_FIXTURE="${WORLD_JSON_FIXTURE}")
gtest_discover_tests(test_world_assets)

# Ensure the JSON fixture is generated before the state tests run.
add_custom_target(world_json_fixture ALL DEPENDS ${WORLD_JSON_FIXTURE})
add_dependencies(test_world_state world_json_fixture)
