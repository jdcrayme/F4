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

# test_symbol_library — pure data-model + JSON I/O tests for the new
# data-driven symbol library. No GPU context required (the render helpers
# are exercised at runtime by the Symbol Creator tool in f4-world-viewer).
add_executable(test_symbol_library test_symbol_library.cpp ${IMGUI_SOURCES})
target_include_directories(test_symbol_library PRIVATE ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_link_libraries(test_symbol_library PRIVATE f4-renderer GTest::gtest_main)
add_test(NAME symbol_library COMMAND test_symbol_library)

# test_svg_import — SVG (authoring format) <-> SymbolDefinition round-trips:
# subset parsing, transforms, curve flattening, evenodd holes, earcut
# caches, loud failure on out-of-subset input, and a full corpus
# round-trip over the repo's f4_symbols.json. Pure data tests, no GPU.
# f4-xml linked explicitly: f4-renderer consumes it PRIVATEly, so its
# pugixml symbols don't propagate to this executable's link line.
add_executable(test_svg_import test_svg_import.cpp ${IMGUI_SOURCES})
target_include_directories(test_svg_import PRIVATE ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_compile_definitions(test_svg_import PRIVATE
    F4_SYMBOLS_JSON_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../../f4_symbols.json")
target_link_libraries(test_svg_import PRIVATE f4-renderer f4-xml GTest::gtest_main)
add_test(NAME svg_import COMMAND test_svg_import)

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

# ── Ground-layout geometry builder (moved from f4-world-viewer) ─────────────
# Pure data-model tests: no GL context, but the executable still needs the
# ImGui sources because f4-renderer.a references ImGui symbols.
add_executable(test_ground_layout_models test_ground_layout_models.cpp ${IMGUI_SOURCES})
target_include_directories(test_ground_layout_models PRIVATE ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_link_libraries(test_ground_layout_models PRIVATE f4-renderer GTest::gtest_main)
add_test(NAME ground_layout_models COMMAND test_ground_layout_models)

# ── World camera (pure math: heading vectors, camera construction) ─────────
add_executable(test_world_camera test_world_camera.cpp ${IMGUI_SOURCES})
target_include_directories(test_world_camera PRIVATE ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_link_libraries(test_world_camera PRIVATE f4-renderer GTest::gtest_main)
add_test(NAME world_camera COMMAND test_world_camera)

# ── World renderer (SceneDescription defaults, entity dispatch, GPU smoke) ─
add_executable(test_world_renderer test_world_renderer.cpp ${IMGUI_SOURCES})
target_include_directories(test_world_renderer PRIVATE ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_link_libraries(test_world_renderer PRIVATE f4-renderer GTest::gtest_main f4-entities)
add_test(NAME world_renderer COMMAND test_world_renderer)
