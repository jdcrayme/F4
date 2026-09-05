# Fixtures live next to the tests. f4-import tests build a synthetic
# Data/ tree at runtime (in a per-test temp dir via
# std::filesystem::temp_directory_path) so the suite is hermetic.

function(f4_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE GTest::gtest_main f4-import)
    target_compile_definitions(${name} PRIVATE
        FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/"
    )
    gtest_discover_tests(${name})
endfunction()

f4_add_test(test_doctor test_doctor.cpp)

# The models round-trip test needs the KoreaObj fixtures + links f4-gltf
# for the round-trip load verification.
add_executable(test_models_gltf test_models_gltf.cpp)
target_link_libraries(test_models_gltf PRIVATE GTest::gtest_main f4-import f4-gltf f4-models)
target_compile_definitions(test_models_gltf PRIVATE
    KOREAOBJ_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/f4-models/tests/fixtures"
)
gtest_discover_tests(test_models_gltf)

# Tranche 0c: material emission + texture PNG export. The PNG tests need
# KoreaObj.TEX — optional (the test skips when missing), falling back to
# the repo's temp/ copy.
add_executable(test_textures_gltf test_textures_gltf.cpp)
target_link_libraries(test_textures_gltf PRIVATE GTest::gtest_main f4-import f4-gltf f4-models)
target_compile_definitions(test_textures_gltf PRIVATE
    KOREAOBJ_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/f4-models/tests/fixtures"
    KOREAOBJ_TEX_FALLBACK="${CMAKE_SOURCE_DIR}/temp/KoreaObj.TEX"
)
gtest_discover_tests(test_textures_gltf)

# The CLI test spawns the f4import binary as a subprocess — it needs the
# path baked in at compile time.
add_executable(test_f4import_cli test_f4import_cli.cpp)
target_link_libraries(test_f4import_cli PRIVATE GTest::gtest_main f4-import)
target_compile_definitions(test_f4import_cli PRIVATE
    FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/"
    F4IMPORT_PATH="$<TARGET_FILE:f4import>"
)
gtest_discover_tests(test_f4import_cli)
