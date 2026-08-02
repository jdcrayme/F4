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
    target_link_libraries(${name} PRIVATE GTest::gtest_main f4-terrain)
    target_compile_definitions(${name} PRIVATE
        TERRAIN_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/")
    gtest_discover_tests(${name})
endfunction()

f4_add_test(test_terrain test_terrain.cpp)
