# GoogleTest is fetched and GoogleTest module included from the root
# CMakeLists.txt (via cmake/f4_deps.cmake). This file only registers
# test executables via gtest_discover_tests().

function(f4_add_anim_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE GTest::gtest_main f4-anim)
    gtest_discover_tests(${name} PROPERTIES LABELS "f4-anim" TIMEOUT 60)
endfunction()

f4_add_anim_test(test_anim_channels test_channels.cpp)
f4_add_anim_test(test_anim_rig test_rig.cpp)
