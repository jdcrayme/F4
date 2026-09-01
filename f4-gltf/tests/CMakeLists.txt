function(f4_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE GTest::gtest_main f4-gltf)
    gtest_discover_tests(${name})
endfunction()

f4_add_test(test_gltf_loader test_gltf_loader.cpp)
