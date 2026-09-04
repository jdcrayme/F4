# GoogleTest is fetched and GoogleTest module included from the root
# CMakeLists.txt (via cmake/f4_deps.cmake). This file only registers
# test executables via gtest_discover_tests().

function(f4_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE GTest::gtest_main f4-world-convert)
    target_compile_definitions(${name} PRIVATE
        FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/"
        # REPO_ROOT: the repo root, where the v71 mid-campaign sample
        # TestCamp.cam lives (pushed as "Temporary data"). Used by the
        # v71 decoder tests; the file is skipped gracefully when absent
        # (e.g. shallow exports) via a file-exists guard.
        REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../../")
    gtest_discover_tests(${name})
endfunction()

f4_add_test(test_lzss         test_lzss.cpp)
f4_add_test(test_cam_archive  test_cam_archive.cpp)
f4_add_test(test_campaign     test_campaign.cpp)
f4_add_test(test_class_table  test_class_table.cpp)
f4_add_test(test_aii_config   test_aii_config.cpp)
f4_add_test(test_objectives   test_objectives.cpp)
f4_add_test(test_units        test_units.cpp)
f4_add_test(test_theater_data test_theater_data.cpp)

# Save-write tranche tests. test_cmp_encoder links f4-lzss directly because
# it exercises f4::lzss::compress on the real .cmp payload (f4-world-convert
# links f4-lzss PRIVATE, so the include path doesn't propagate to tests).
add_executable(test_cmp_encoder test_cmp_encoder.cpp)
target_link_libraries(test_cmp_encoder PRIVATE GTest::gtest_main f4-world-convert f4-lzss)
target_compile_definitions(test_cmp_encoder PRIVATE
    FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/"
    REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../../")
gtest_discover_tests(test_cmp_encoder)

add_executable(test_cam_writer test_cam_writer.cpp)
target_link_libraries(test_cam_writer PRIVATE GTest::gtest_main f4-world-convert)
target_compile_definitions(test_cam_writer PRIVATE
    FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/"
    REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../../")
gtest_discover_tests(test_cam_writer)

# Save-write tranche: campaign JSON round-trip + objective encoder.
add_executable(test_campaign_json test_campaign_json.cpp)
target_link_libraries(test_campaign_json PRIVATE GTest::gtest_main f4-world-convert)
target_compile_definitions(test_campaign_json PRIVATE
    FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/"
    REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../../")
gtest_discover_tests(test_campaign_json)

add_executable(test_objective_encoder test_objective_encoder.cpp)
target_link_libraries(test_objective_encoder PRIVATE GTest::gtest_main f4-world-convert f4-lzss)
target_compile_definitions(test_objective_encoder PRIVATE
    FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/"
    REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../../")
gtest_discover_tests(test_objective_encoder)

# Save-write tranche: .tea team sub-file encoder.
add_executable(test_team_encoder test_team_encoder.cpp)
target_link_libraries(test_team_encoder PRIVATE GTest::gtest_main f4-world-convert)
target_compile_definitions(test_team_encoder PRIVATE
    FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/"
    REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../../")
gtest_discover_tests(test_team_encoder)

# Save-write tranche: .uni unit sub-file encoder.
add_executable(test_unit_encoder test_unit_encoder.cpp)
target_link_libraries(test_unit_encoder PRIVATE GTest::gtest_main f4-world-convert f4-lzss)
target_compile_definitions(test_unit_encoder PRIVATE
    FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/"
    REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../../")
gtest_discover_tests(test_unit_encoder)

# Save-write tranche: CampaignSaver integration bridge.
add_executable(test_campaign_saver test_campaign_saver.cpp)
target_link_libraries(test_campaign_saver PRIVATE GTest::gtest_main f4-world-convert)
target_compile_definitions(test_campaign_saver PRIVATE
    FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/"
    REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../../")
gtest_discover_tests(test_campaign_saver)
