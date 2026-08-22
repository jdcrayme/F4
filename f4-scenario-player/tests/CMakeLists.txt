# f4-scenario-player/tests/CMakeLists.txt
#
# Unit tests for the scenario player.
#
# The previous test_airport_geometry and test_coordinate_transform targets
# have been removed:
#   - airport_geometry tests verified the old synthetic builder, which is
#     now replaced by f4::renderer::build_airfield_geometry_3d() (shared
#     with f4-world-viewer). The shared builder is unit-tested by
#     f4-renderer/tests/test_ground_layout_models.cpp.
#   - coordinate_transform tests verified a deprecated wrapper header that
#     delegated to f4::renderer::coord_transform.hpp. The wrapper has been
#     deleted; the actual coord-transform tests live in
#     f4-renderer/tests/test_coord_transform.cpp.
#
# The full screenshot smoke test still runs via the --screenshot CLI flag
# in CI (see f4-scenario-player-renderer-refactor.patch's CHANGES.md).

cmake_minimum_required(VERSION 3.20)

# No unit tests currently — the scenario player is exercised via the
# --screenshot integration test. To add new unit tests here, follow the
# pattern:
#
#   function(f4_scenario_player_add_test name source)
#       add_executable(${name} ${source})
#       target_link_libraries(${name} PRIVATE GTest::gtest_main f4_scenario_player_lib)
#       target_include_directories(${name} PRIVATE
#           ${CMAKE_CURRENT_SOURCE_DIR}/../src)  # for viewer_state.hpp (private)
#       target_compile_features(${name} PRIVATE cxx_std_20)
#       gtest_discover_tests(${name})
#   endfunction()
#
#   f4_scenario_player_add_test(test_airfield_overlays test_airfield_overlays.cpp)
