function(f4_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE GTest::gtest_main f4-assets)
    gtest_discover_tests(${name})
endfunction()

f4_add_test(test_asset_id       test_asset_id.cpp)
f4_add_test(test_manifest        test_manifest.cpp)
f4_add_test(test_asset_root      test_asset_root.cpp)
