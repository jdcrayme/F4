include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()
include(GoogleTest)

function(f4_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE GTest::gtest_main f4-world-convert)
    target_compile_definitions(${name} PRIVATE
        FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/")
    gtest_discover_tests(${name})
endfunction()

f4_add_test(test_lzss         test_lzss.cpp)
f4_add_test(test_cam_archive  test_cam_archive.cpp)
f4_add_test(test_campaign     test_campaign.cpp)
f4_add_test(test_objectives   test_objectives.cpp)
f4_add_test(test_units        test_units.cpp)
f4_add_test(test_theater_data test_theater_data.cpp)
