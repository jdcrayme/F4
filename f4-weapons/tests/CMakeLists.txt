# GoogleTest is fetched and the module included from the root CMakeLists.txt
# (via cmake/f4_deps.cmake). This file only registers test executables via
# gtest_discover_tests().
#
# Each test target links ONLY against f4-weapons (plus gtest_main). If a test
# ever needs f4-flight-model or f4-ai, that is a coupling bug: f4-weapons is
# a leaf library of the combat chain and its tests must prove that.

function(f4_weapons_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE GTest::gtest_main f4-weapons)
    gtest_discover_tests(${name})
endfunction()

f4_weapons_add_test(test_damage        test_damage.cpp)
f4_weapons_add_test(test_missile       test_missile.cpp)
f4_weapons_add_test(test_weapon_store  test_weapon_store.cpp)
f4_weapons_add_test(test_weapon_table  test_weapon_table.cpp)
f4_weapons_add_test(test_gun           test_gun.cpp)
f4_weapons_add_test(test_engagement    test_engagement.cpp)
f4_weapons_add_test(test_bomb          test_bomb.cpp)
