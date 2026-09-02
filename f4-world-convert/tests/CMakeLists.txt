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
