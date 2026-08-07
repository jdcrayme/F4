# GoogleTest is fetched and GoogleTest module included from the root
# CMakeLists.txt (via cmake/f4_deps.cmake). This file only registers
# test executables via gtest_discover_tests().

function(f4_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE GTest::gtest_main f4-messaging)
    gtest_discover_tests(${name})
endfunction()

f4_add_test(test_messaging_bus   test_bus.cpp)
f4_add_test(test_messaging_queue test_queue.cpp)
f4_add_test(test_messaging_thread test_thread.cpp)
