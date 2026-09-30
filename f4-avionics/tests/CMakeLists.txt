# GoogleTest is fetched and the GoogleTest module included from the root
# CMakeLists.txt (via cmake/f4_deps.cmake). This file only registers
# test executables via gtest_discover_tests().

function(f4_avionics_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE GTest::gtest_main f4-avionics)
    gtest_discover_tests(${name} PROPERTIES LABELS "f4-avionics" TIMEOUT 60)
endfunction()

f4_avionics_add_test(test_avionics_ins test_ins.cpp)
f4_avionics_add_test(test_avionics_steer test_steerpoint.cpp)
f4_avionics_add_test(test_avionics_fcr test_fcr_page.cpp)
