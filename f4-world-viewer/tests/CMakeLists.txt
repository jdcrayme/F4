# GoogleTest is fetched and GoogleTest module included from the root
# CMakeLists.txt (via cmake/f4_deps.cmake). This file only registers
# test executables via gtest_discover_tests().

# --- Settings tests ---
# Link against settings.cpp + f4-json (settings.cpp uses f4::json::Reader
# and f4::json::Writer for the on-disk settings format). No raylib dep.
add_executable(test_settings
    test_settings.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/settings.cpp
)
target_include_directories(test_settings PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
target_link_libraries(test_settings PRIVATE GTest::gtest_main f4-json)
gtest_discover_tests(test_settings)

# --- Hex model + decoders tests ---
# Link against hex_model.cpp + decoders.cpp + f4-world-convert (decoders
# call into CamArchive). Still no raylib dep — the model + decoders are
# pure data-manipulation code.
add_executable(test_hex_model
    test_hex_model.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/hex/hex_model.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/hex/decoders.cpp
)
target_include_directories(test_hex_model PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_link_libraries(test_hex_model PRIVATE
    GTest::gtest_main
    f4-world-convert
        f4-terrain
)
target_compile_definitions(test_hex_model PRIVATE
    FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../f4-world-convert/tests/fixtures/"
    TERRAIN_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../f4-terrain/tests/fixtures/"
)
gtest_discover_tests(test_hex_model)

