# GoogleTest is fetched and GoogleTest module included from the root
# CMakeLists.txt (via cmake/f4_deps.cmake). This file only registers
# test executables via gtest_discover_tests().

function(f4_lzss_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE GTest::gtest_main f4-lzss)
    gtest_discover_tests(${name})
endfunction()

f4_lzss_add_test(test_lzss_core test_lzss.cpp)
f4_lzss_add_test(test_lzss_compress test_lzss_compress.cpp)
