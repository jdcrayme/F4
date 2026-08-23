# GoogleTest is fetched and GoogleTest module included from the root
# CMakeLists.txt (via cmake/f4_deps.cmake). This file only registers
# test executables via gtest_discover_tests().

function(f4_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE GTest::gtest_main f4-terrain)
    target_compile_definitions(${name} PRIVATE
        TERRAIN_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/")
    gtest_discover_tests(${name})
endfunction()

f4_add_test(test_terrain test_terrain.cpp)

# Path B1: TerrainDataAdapter tests (bilinear interpolation + coord mapping).
# Needs access to the world-convert fixtures (korea.terrain.json) for the
# real-Korea integration test.
add_executable(test_terrain_adapter test_terrain_adapter.cpp)
target_link_libraries(test_terrain_adapter PRIVATE GTest::gtest_main f4-terrain)
target_compile_definitions(test_terrain_adapter PRIVATE
    TERRAIN_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/"
    FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../f4-world-convert/tests/fixtures/"
)
gtest_discover_tests(test_terrain_adapter)
