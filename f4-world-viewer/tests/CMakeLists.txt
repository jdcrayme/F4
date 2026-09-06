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
    f4-world-types
        f4-terrain
)
target_compile_definitions(test_hex_model PRIVATE
    FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../f4-world-convert/tests/fixtures/"
    TERRAIN_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../f4-terrain/tests/fixtures/"
)
gtest_discover_tests(test_hex_model)

# --- Replay mode tests (Path B2) ---
# Exercises the ReplayState data model + load_replay + fit_replay_camera.
# Pure data-layer tests — no raylib/imgui dep. The full render path is
# exercised by the screenshot smoke test (cli/main.cpp --replay ...).
# We compile replay_mode.cpp directly into the test target (rather than
# linking against f4_world_viewer) because the viewer library pulls in
# raylib + imgui, which we don't want in a unit test.
add_executable(test_replay_mode
    test_replay_mode.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/replay_mode.cpp
)
target_include_directories(test_replay_mode PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
target_link_libraries(test_replay_mode PRIVATE
    GTest::gtest_main
    f4-recorder
)
gtest_discover_tests(test_replay_mode)


# --- Enum text tests (B.3 QC tranche) ---
# enum_text.hpp is header-only: the WP_ACTION anchor pins (campwp.h) and
# the shared campaign-clock formatter. No library link beyond gtest.
add_executable(test_enum_text
    test_enum_text.cpp
)
target_include_directories(test_enum_text PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
target_link_libraries(test_enum_text PRIVATE GTest::gtest_main f4-world-convert f4-world-types)
gtest_discover_tests(test_enum_text)
