cmake_minimum_required(VERSION 3.20)

# ── ImGui sources (needed by f4-renderer's symbols.cpp and orbit_camera.cpp) ───
# These must be compiled into each test executable because f4-renderer is a
# static library that references ImGui symbols but doesn't bundle the sources.
# This matches the pattern used by all viewer apps.
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
)

# ── Pure-math tests (no Raylib GPU context needed) ─────────────────────────────
# These still need imgui sources linked because f4-renderer.a contains
# unresolved ImGui symbols from symbols.cpp and orbit_camera.cpp.

add_executable(test_coord_transform test_coord_transform.cpp ${IMGUI_SOURCES})
target_include_directories(test_coord_transform PRIVATE ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_link_libraries(test_coord_transform PRIVATE f4-renderer GTest::gtest_main)
add_test(NAME coord_transform COMMAND test_coord_transform)

add_executable(test_symbol_mapping test_symbol_mapping.cpp ${IMGUI_SOURCES})
target_include_directories(test_symbol_mapping PRIVATE ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_link_libraries(test_symbol_mapping PRIVATE f4-renderer GTest::gtest_main)
add_test(NAME symbol_mapping COMMAND test_symbol_mapping)

add_executable(test_mesh_builder test_mesh_builder.cpp ${IMGUI_SOURCES})
target_include_directories(test_mesh_builder PRIVATE ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_link_libraries(test_mesh_builder PRIVATE f4-renderer GTest::gtest_main)
add_test(NAME mesh_builder COMMAND test_mesh_builder)

# ── GPU-context tests (require InitWindow / OpenGL) ────────────────────────────

add_executable(test_orbit_camera test_orbit_camera.cpp ${IMGUI_SOURCES})
target_include_directories(test_orbit_camera PRIVATE ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_link_libraries(test_orbit_camera PRIVATE f4-renderer GTest::gtest_main)
add_test(NAME orbit_camera COMMAND test_orbit_camera)

add_executable(test_lit_shader test_lit_shader.cpp ${IMGUI_SOURCES})
target_include_directories(test_lit_shader PRIVATE ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_link_libraries(test_lit_shader PRIVATE f4-renderer GTest::gtest_main)
add_test(NAME lit_shader COMMAND test_lit_shader)

add_executable(test_texture_cache test_texture_cache.cpp ${IMGUI_SOURCES})
target_include_directories(test_texture_cache PRIVATE ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_link_libraries(test_texture_cache PRIVATE f4-renderer GTest::gtest_main)
add_test(NAME texture_cache COMMAND test_texture_cache)

add_executable(test_draw_3d test_draw_3d.cpp ${IMGUI_SOURCES})
target_include_directories(test_draw_3d PRIVATE ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_link_libraries(test_draw_3d PRIVATE f4-renderer GTest::gtest_main)
add_test(NAME draw_3d COMMAND test_draw_3d)

# test_feature_mesh uses the bundled KoreaObj + FALCON4.ct fixtures; the
# GPU-context tests auto-skip if those fixtures aren't available.
add_executable(test_feature_mesh test_feature_mesh.cpp ${IMGUI_SOURCES})
target_include_directories(test_feature_mesh PRIVATE ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_link_libraries(test_feature_mesh PRIVATE f4-renderer GTest::gtest_main)
add_test(NAME feature_mesh COMMAND test_feature_mesh)

# test_entity_render — entity-level rendering: entity_icon_info(), RenderEntity()
# Pure tests (no GPU) for component-inspection logic; RenderEntity with
# null resources is safe to call without a GPU context.
add_executable(test_entity_render test_entity_render.cpp ${IMGUI_SOURCES})
target_include_directories(test_entity_render PRIVATE ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_link_libraries(test_entity_render PRIVATE f4-renderer GTest::gtest_main)
add_test(NAME entity_render COMMAND test_entity_render)
