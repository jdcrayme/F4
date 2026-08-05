cmake_minimum_required(VERSION 3.20)

include(FetchContent)
FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()
include(GoogleTest)

function(f4_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE GTest::gtest_main f4-models)

    # The KoreaObj.HDR / KoreaObj.LOD fixtures live in the repo-wide temp/
    # directory (committed ground-truth binaries for the legacy LOD format).
    # The historical ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/ path pointed at a
    # non-existent directory, causing all 13 ModelsFixtureTest cases to
    # GTEST_SKIP() silently on every platform.
    #
    # Resolve the temp/ directory relative to the project source root. Two
    # candidates cover both the in-tree build and the standalone f4-models
    # build.
    set(_f4_temp_dir "${CMAKE_SOURCE_DIR}/temp")
    if(NOT EXISTS "${_f4_temp_dir}/KoreaObj.HDR")
        # Fall back to a per-test fixtures/ directory if the user has one
        # (allows shipping a minimal HDR/LOD pair without the rest of temp/).
        set(_f4_temp_dir "${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
    endif()

    target_compile_definitions(${name} PRIVATE
        MODELS_FIXTURE_DIR="${_f4_temp_dir}/"
    )
    gtest_discover_tests(${name})
endfunction()

f4_add_test(test_models test_models.cpp)
