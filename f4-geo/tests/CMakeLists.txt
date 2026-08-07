# GoogleTest is fetched and GoogleTest module included from the root
# CMakeLists.txt (via cmake/f4_deps.cmake). This file only registers
# test executables via gtest_discover_tests().

# Helper — matches the pattern used by f4-units/f4-math test CMakeLists.
# Target names are prefixed (geo_) because CMake target names are global
# across the build and several bare names (e.g. test_conversions) are
# already taken by f4-units.
function(f4_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE GTest::gtest_main f4-geo)
    gtest_discover_tests(${name})
endfunction()

f4_add_test(test_geo_position    test_position.cpp)
f4_add_test(test_geo_conversions test_conversions.cpp)
f4_add_test(test_geo_datum       test_datum.cpp)
f4_add_test(test_geo_relative    test_relative.cpp)
