function(f4_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE GTest::gtest_main f4-assets)
    gtest_discover_tests(${name})
endfunction()

f4_add_test(test_asset_id       test_asset_id.cpp)
f4_add_test(test_manifest        test_manifest.cpp)
f4_add_test(test_asset_root      test_asset_root.cpp)
f4_add_test(test_hash            test_hash.cpp)

# F4_SOURCE_DIR lets the hash tests verify the COMMITTED Data/
# fingerprints (the generator↔runtime contract).
target_compile_definitions(test_hash PRIVATE F4_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
