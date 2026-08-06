cmake_minimum_required(VERSION 3.20)
project(f4-models-viewer VERSION 0.1.0 LANGUAGES CXX DESCRIPTION "Interactive Raylib + Dear ImGui 3D model viewer for F4 KoreaObj data")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ============================================================================
# Third-party dependencies (Raylib + Dear ImGui + rlImGui bindings).
# These may already be fetched by f4-world-viewer in the same build tree.
# Guard with FetchContent_GetProperties to avoid re-declaring.
# ============================================================================
include(FetchContent)

# Raylib — only declare if not already available
FetchContent_GetProperties(raylib)
if(NOT raylib_POPULATED)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_GAMES    OFF CACHE BOOL "" FORCE)
    set(MACOS_FATLIBRARY OFF CACHE BOOL "" FORCE)
    set(USE_AUDIO OFF CACHE BOOL "" FORCE)
    set(USE_WAYLAND OFF CACHE BOOL "" FORCE)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
    FetchContent_Declare(
        raylib
        GIT_REPOSITORY https://github.com/raysan5/raylib.git
        GIT_TAG        5.0
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(raylib)
endif()

# Dear ImGui — only populate if not already available
set(IMGUI_DISABLE_OBSOLETE_FUNCTIONS OFF CACHE BOOL "" FORCE)
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
    FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG        v1.91.5
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(imgui)
endif()
set(IMGUI_DIR ${imgui_SOURCE_DIR})

# rlImGui — only populate if not already available
FetchContent_GetProperties(rlimgui)
if(NOT rlimgui_POPULATED)
    FetchContent_Declare(
        rlimgui
        GIT_REPOSITORY https://github.com/raylib-extras/rlImGui.git
        GIT_TAG        9acdbbf
    )
    FetchContent_MakeAvailable(rlimgui)
endif()
set(RLIMGUI_DIR ${rlimgui_SOURCE_DIR})

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
    src/canvas3d.cpp
    src/file_ops.cpp
    src/imgui_panels.cpp
    ${IMGUI_SOURCES}
    ${RLIMGUI_SOURCES}
    ${TINYFILEDIALOGS_SOURCES}
)
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
