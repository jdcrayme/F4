cmake_minimum_required(VERSION 3.20)
project(f4-models-viewer VERSION 0.1.0 LANGUAGES CXX DESCRIPTION "Interactive Raylib + Dear ImGui 3D model viewer for F4 KoreaObj data")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ============================================================================
# Third-party dependencies (Raylib + Dear ImGui + rlImGui bindings).
# Consolidated into a shared cmake module in the 2026 cleanup pass.
# ============================================================================
include(f4_raylib_deps)

# tinyfiledialogs — reference the existing vendored copy from f4-world-viewer.
set(TINYFILEDIALOGS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../f4-world-viewer/third_party/tinyfiledialogs)

# Build Dear ImGui sources directly into our viewer library.
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
)
set(RLIMGUI_SOURCES
    ${RLIMGUI_DIR}/rlImGui.cpp
)

set(TINYFILEDIALOGS_SOURCES
    ${TINYFILEDIALOGS_DIR}/tinyfiledialogs.c
)

# ============================================================================
# Static library: f4_models_viewer
# ============================================================================
add_library(f4_models_viewer STATIC
    src/viewer_app.cpp
    src/camera3d.cpp
    src/scene.cpp
    src/legacy_mesh_builder.cpp
    src/canvas3d.cpp
    src/file_ops.cpp
    src/imgui_panels.cpp
    ${IMGUI_SOURCES}
    ${RLIMGUI_SOURCES}
    ${TINYFILEDIALOGS_SOURCES}
)
target_link_libraries(f4_models_viewer PUBLIC f4_warnings)
target_include_directories(f4_models_viewer
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${IMGUI_DIR}
        ${IMGUI_DIR}/backends
        ${RLIMGUI_DIR}
        ${TINYFILEDIALOGS_DIR}
)
target_link_libraries(f4_models_viewer PUBLIC
    f4-models
    f4-renderer
    f4-install
    f4-json
)
target_link_libraries(f4_models_viewer PRIVATE raylib)
set_source_files_properties(${TINYFILEDIALOGS_SOURCES} PROPERTIES LANGUAGE C)

set_target_properties(f4_models_viewer PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
)

add_library(F4::ModelsViewer ALIAS f4_models_viewer)

# ============================================================================
# CLI tool: f4-models-viewer [options] [KoreaObj.HDR] [KoreaObj.LOD]
# ============================================================================
add_executable(f4-models-viewer cli/main.cpp)
target_link_libraries(f4-models-viewer PRIVATE f4_models_viewer)
set_target_properties(f4-models-viewer PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
)
