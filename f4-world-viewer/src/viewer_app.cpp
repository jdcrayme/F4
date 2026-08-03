// f4-world-viewer/src/viewer_app.cpp
//
// Interactive Raylib + Dear ImGui application for inspecting F4 world data.
//
// Layout:
//   ┌──────────────────────────────────────────────────────────────────┐
//   │ Menu bar: File | View | Help                                    │
//   ├──────────────────────────────────────────────────┬───────────────┤
//   │ Raylib canvas (2D top-down)                      │ ImGui: layers │
//   │   • Color-coded terrain tiles                    │   + inspector │
//   │   • Objective circles by team                    │   + status    │
//   │   • Unit squares by team                         │               │
//   │   Pan: drag  Zoom: wheel  Click: select          │               │
//   └──────────────────────────────────────────────────┴───────────────┘
//
// The viewer wraps the cam2json and terrain2json CLIs in-process (calls
// the libraries directly), so the user can import raw FreeFalcon binary
// files from the File menu without leaving the app. This is the starting
// point for a future world editor: the same load/render pipeline will
// gain edit/save capabilities as new systems come online.

#include <f4/viewer/viewer_app.hpp>

#include <f4/convert/cam_archive.hpp>
#include <f4/convert/world_json.hpp>
#include <f4/convert/terrain_converter.hpp>
#include <f4/terrain/terrain_data.hpp>
#include <f4/world/world_state.hpp>

#include <imgui.h>
#include <rlImGui.h>
#include <raylib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace f4::viewer {

namespace {

// ---------------------------------------------------------------------------
// Team color palette — keyed by owner (Control) byte. Same scheme as the
// late f4-world-vis SVG renderer (which we deleted in favor of this viewer).
// ---------------------------------------------------------------------------
struct RlColor { unsigned char r, g, b, a; };

RlColor color_for_owner(uint8_t owner) {
    switch (owner) {
        case 0:  return {128, 128, 128, 255}; // neutral — gray
        case 1:  return {220,  60,  60, 255}; // enemy — red
        case 2:  return { 60, 140, 220, 255}; // friendly — blue
        case 3:  return { 80, 180,  80, 255}; // ROK — green
        case 4:  return {180, 180,  80, 255}; // Japan — yellow
        case 5:  return {220, 120,  60, 255}; // DPRK — orange
        case 6:  return {180,  80, 180, 255}; // PRC — magenta
        default: return {200, 200, 200, 255};
    }
}

RlColor to_rl(const f4::terrain::Color4& c) {
    return {c.r, c.g, c.b, c.a};
}

} // namespace

// ---------------------------------------------------------------------------
// Viewer state — everything the render loop needs.
// ---------------------------------------------------------------------------
struct ViewerApp::Impl {
    // Window / camera
    int window_w = 1400;
    int window_h = 900;
    float cam_x = 0.0f;          // world-space center x (grid units)
    float cam_y = 0.0f;          // world-space center y (grid units)
    float cam_zoom = 4.0f;       // pixels per grid unit
    bool dragging = false;
    Vector2 drag_start = {0, 0};
    float drag_cam_x0 = 0, drag_cam_y0 = 0;

    // Data
    f4::world::WorldState world;
    bool world_loaded = false;
    std::string world_path_display;

    // Selection
    enum class SelectionKind { None, Objective, Unit };
    SelectionKind sel_kind = SelectionKind::None;
    int sel_index = -1;          // index into world.objectives / world.units

    // Layer toggles
    bool show_terrain = true;
    bool show_objectives = true;
    bool show_units = true;
    bool show_grid = false;
    bool show_legend = true;

    // Status
    std::string status_msg;
    std::string last_error;

    // Conversion cache — paths used by the most recent import operation.
    std::filesystem::path last_world_json_path;
    std::filesystem::path last_terrain_json_path;

    // Pending file dialog (Raylib doesn't ship a native picker, so we use
    // a simple ImGui text-input modal. A real file browser will replace
    // this in a future pass — likely via tinyfiledialogs.)
    bool pending_dialog_open = false;
    char pending_dialog_path[1024] = {0};
    std::string pending_dialog_title;
    std::string pending_dialog_filters;
    std::function<void(const std::string&)> pending_dialog_callback;

    // Scheduled screenshot (for headless smoke tests)
    bool screenshot_pending = false;
    double screenshot_at = 0.0;    // GetTime() value
    std::string screenshot_path;

    // --- Helpers ---

    // Convert world (grid) coordinates to screen pixels.
    Vector2 world_to_screen(float gx, float gy) const {
        const float cx = window_w * 0.5f;
        const float cy = window_h * 0.5f;
        return {
            cx + (gx - cam_x) * cam_zoom,
            cy - (gy - cam_y) * cam_zoom   // flip y: world y=north = screen up
        };
    }

    // Convert screen pixels to world (grid) coordinates.
    void screen_to_world(float sx, float sy, float* gx, float* gy) const {
        const float cx = window_w * 0.5f;
        const float cy = window_h * 0.5f;
        *gx = cam_x + (sx - cx) / cam_zoom;
        *gy = cam_y - (sy - cy) / cam_zoom;   // un-flip y
    }

    // Fit camera to show the entire theater grid (1024x1024 by default).
    void fit_to_world() {
        const float grid_size = 1024.0f;
        cam_x = grid_size * 0.5f;
        cam_y = grid_size * 0.5f;
        const float zoom_x = static_cast<float>(window_w) / grid_size;
        const float zoom_y = static_cast<float>(window_h) / grid_size;
        cam_zoom = std::min(zoom_x, zoom_y) * 0.95f;
    }
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
ViewerApp::ViewerApp()  : impl_(std::make_unique<Impl>()) {}
ViewerApp::~ViewerApp() = default;

void ViewerApp::run() {
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
    InitWindow(impl_->window_w, impl_->window_h, "F4 World Viewer");
    SetTargetFPS(60);
    rlImGuiSetup(true);

    // Default to a fit-to-world view.
    impl_->fit_to_world();

    while (!WindowShouldClose()) {
        // Handle window resize
        const int new_w = GetScreenWidth();
        const int new_h = GetScreenHeight();
        if (new_w != impl_->window_w || new_h != impl_->window_h) {
            impl_->window_w = new_w;
            impl_->window_h = new_h;
        }

        handle_input();

        // F2 = screenshot (useful for headless smoke tests)
        if (IsKeyPressed(KEY_F2)) {
            const std::string path = "f4_viewer_screenshot.png";
            TakeScreenshot(path.c_str());
            impl_->status_msg = "Saved screenshot: " + path;
        }

        // Scheduled screenshot (used by schedule_screenshot — for headless tests)
        if (impl_->screenshot_pending && GetTime() >= impl_->screenshot_at) {
            TakeScreenshot(impl_->screenshot_path.c_str());
            impl_->status_msg = "Saved screenshot: " + impl_->screenshot_path;
            impl_->screenshot_pending = false;
        }

        BeginDrawing();
        ClearBackground(Color{20, 22, 28, 255});
        draw_canvas();
        draw_imgui();
        EndDrawing();
    }

    rlImGuiShutdown();
    CloseWindow();
}

// ---------------------------------------------------------------------------
// File operations
// ---------------------------------------------------------------------------
void ViewerApp::load_terrain_json(const std::filesystem::path& path) {
    impl_->world.terrain.load_terrain_json(path);
    impl_->world.terrain_loaded = true;
    impl_->last_terrain_json_path = path;
    impl_->status_msg = "Loaded terrain: " + path.string();
    impl_->fit_to_world();
}

void ViewerApp::load_world_json(const std::filesystem::path& path) {
    impl_->world.load(path);
    impl_->world_loaded = true;
    impl_->world_path_display = path.string();
    impl_->last_world_json_path = path;
    impl_->status_msg = "Loaded world: " + path.string() +
        "  (" + std::to_string(impl_->world.objectives.size()) + " objectives, " +
        std::to_string(impl_->world.units.size()) + " units)";

    // Try to auto-load the referenced terrain file.
    if (!impl_->world.terrain_file.empty()) {
        try {
            impl_->world.load_terrain();
            impl_->status_msg += "  + terrain: " + impl_->world.terrain_file;
        } catch (const std::exception& e) {
            impl_->last_error = "Auto-load terrain failed: " + std::string(e.what());
        }
    }
    impl_->fit_to_world();
}

void ViewerApp::import_terrain_binary(const std::filesystem::path& terrain_dir) {
    // Use the in-process library so we don't shell out to terrain2json.
    const auto out = terrain_dir / "terrain.json";
    f4::convert::convert_terrain_dir(terrain_dir, out, "korea");
    load_terrain_json(out);
}

void ViewerApp::import_cam_archive(const std::filesystem::path& cam_path) {
    f4::convert::CamArchive cam;
    cam.load(cam_path);
    f4::convert::WorldJsonOptions opts;
    opts.theater = "korea";
    opts.terrain_file = "korea.terrain.json";
    const std::string json = f4::convert::to_world_json(cam, opts);

    // Write to a temp file next to the .cam, then load via the normal path.
    auto out = cam_path;
    out.replace_extension(".world.json");
    {
        std::ofstream f(out);
        if (!f) throw std::runtime_error("cannot write " + out.string());
        f << json;
    }
    load_world_json(out);
}

void ViewerApp::schedule_screenshot(float delay_sec, const std::string& path) {
    impl_->screenshot_pending = true;
    impl_->screenshot_at = GetTime() + delay_sec;
    impl_->screenshot_path = path;
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
void ViewerApp::handle_input() {
    // Pan: middle-mouse or left-mouse-drag on empty canvas
    const Vector2 mouse = GetMousePosition();
    if (IsMouseButtonPressed(MOUSE_BUTTON_MIDDLE) ||
        (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !ImGui::GetIO().WantCaptureMouse)) {
        impl_->dragging = true;
        impl_->drag_start = mouse;
        impl_->drag_cam_x0 = impl_->cam_x;
        impl_->drag_cam_y0 = impl_->cam_y;
    }
    if (impl_->dragging &&
        (IsMouseButtonReleased(MOUSE_BUTTON_MIDDLE) || IsMouseButtonReleased(MOUSE_BUTTON_LEFT))) {
        impl_->dragging = false;
    }
    if (impl_->dragging) {
        const float dx = (mouse.x - impl_->drag_start.x) / impl_->cam_zoom;
        const float dy = (mouse.y - impl_->drag_start.y) / impl_->cam_zoom;
        impl_->cam_x = impl_->drag_cam_x0 - dx;
        impl_->cam_y = impl_->drag_cam_y0 + dy;   // y flipped
    }

    // Zoom: mouse wheel
    const float wheel = GetMouseWheelMove();
    if (wheel != 0.0f && !ImGui::GetIO().WantCaptureMouse) {
        // Zoom toward the cursor.
        float gx_before, gy_before;
        impl_->screen_to_world(mouse.x, mouse.y, &gx_before, &gy_before);
        impl_->cam_zoom *= (wheel > 0) ? 1.15f : (1.0f / 1.15f);
        impl_->cam_zoom = std::clamp(impl_->cam_zoom, 0.05f, 50.0f);
        float gx_after, gy_after;
        impl_->screen_to_world(mouse.x, mouse.y, &gx_after, &gy_after);
        impl_->cam_x += gx_before - gx_after;
        impl_->cam_y += gy_before - gy_after;
    }

    // Click: select nearest objective/unit within a tolerance radius
    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && !ImGui::GetIO().WantCaptureMouse) {
        float gx, gy;
        impl_->screen_to_world(mouse.x, mouse.y, &gx, &gy);
        const float tol = 8.0f / impl_->cam_zoom;  // 8-pixel pick radius

        // Try objectives first (drawn on top of terrain).
        if (impl_->show_objectives && impl_->world_loaded) {
            int best = -1;
            float best_d2 = tol * tol;
            for (int i = 0; i < static_cast<int>(impl_->world.objectives.size()); ++i) {
                const auto& o = impl_->world.objectives[i];
                const float dx = o.x - gx, dy = o.y - gy;
                const float d2 = dx * dx + dy * dy;
                if (d2 < best_d2) { best_d2 = d2; best = i; }
            }
            if (best >= 0) {
                impl_->sel_kind = Impl::SelectionKind::Objective;
                impl_->sel_index = best;
                return;
            }
        }
        // Then units.
        if (impl_->show_units && impl_->world_loaded) {
            int best = -1;
            float best_d2 = tol * tol;
            for (int i = 0; i < static_cast<int>(impl_->world.units.size()); ++i) {
                const auto& u = impl_->world.units[i];
                const float dx = u.x - gx, dy = u.y - gy;
                const float d2 = dx * dx + dy * dy;
                if (d2 < best_d2) { best_d2 = d2; best = i; }
            }
            if (best >= 0) {
                impl_->sel_kind = Impl::SelectionKind::Unit;
                impl_->sel_index = best;
                return;
            }
        }
        // Nothing hit — clear selection.
        impl_->sel_kind = Impl::SelectionKind::None;
        impl_->sel_index = -1;
    }
}

// ---------------------------------------------------------------------------
// Canvas (Raylib 2D drawing)
// ---------------------------------------------------------------------------
void ViewerApp::draw_canvas() {
    // --- Terrain tiles ---
    if (impl_->show_terrain && impl_->world.terrain_loaded) {
        const auto& td = impl_->world.terrain;
        const uint32_t gw = td.header.width;
        const uint32_t gh = td.header.height;
        // Theater grid is 1024x1024; terrain is 128x128 by default — each
        // terrain cell maps to (1024/width) theater grid units.
        const float scale = 1024.0f / static_cast<float>(gw);

        for (uint32_t y = 0; y < gh; ++y) {
            for (uint32_t x = 0; x < gw; ++x) {
                const auto t = td.tile_type_at(x, y);
                const auto c = f4::terrain::TerrainData::color_for_tile_type(t);
                const RlColor rc = to_rl(c);
                // Terrain cell (x, y) in sim coords maps to world grid
                // (x*scale, y*scale) ... ((x+1)*scale, (y+1)*scale).
                const Vector2 p0 = impl_->world_to_screen(x * scale, y * scale);
                const Vector2 p1 = impl_->world_to_screen((x + 1) * scale, (y + 1) * scale);
                const Rectangle rect = {
                    p0.x, p1.y,
                    p1.x - p0.x, p0.y - p1.y
                };
                DrawRectangleRec(rect, Color{rc.r, rc.g, rc.b, rc.a});
            }
        }
    } else {
        // No terrain — show a hint.
        DrawText("No terrain loaded. Use File > Import Terrain Binary...",
                 impl_->window_w / 2 - 200, impl_->window_h / 2, 18,
                 Color{180, 180, 180, 255});
    }

    // --- Grid (optional) ---
    if (impl_->show_grid) {
        const Color gc = {80, 80, 100, 128};
        const float step = 64.0f;   // grid lines every 64 units
        for (float g = 0; g <= 1024; g += step) {
            const Vector2 a = impl_->world_to_screen(g, 0);
            const Vector2 b = impl_->world_to_screen(g, 1024);
            DrawLineV(a, b, gc);
            const Vector2 c = impl_->world_to_screen(0, g);
            const Vector2 d = impl_->world_to_screen(1024, g);
            DrawLineV(c, d, gc);
        }
    }

    // --- Objectives ---
    if (impl_->show_objectives && impl_->world_loaded) {
        const float base_r = std::max(2.0f, 6.0f * (impl_->cam_zoom / 4.0f));
        for (int i = 0; i < static_cast<int>(impl_->world.objectives.size()); ++i) {
            const auto& o = impl_->world.objectives[i];
            const Vector2 p = impl_->world_to_screen(o.x, o.y);
            const RlColor c = color_for_owner(o.owner);
            const float r = base_r + o.priority * 0.5f;
            DrawCircleV(p, r, Color{c.r, c.g, c.b, 220});
            // Outline selected.
            if (impl_->sel_kind == Impl::SelectionKind::Objective &&
                impl_->sel_index == i) {
                DrawCircleLines(static_cast<int>(p.x), static_cast<int>(p.y),
                                static_cast<int>(r + 3),
                                Color{255, 255, 0, 255});
            }
        }
    }

    // --- Units ---
    // Render with different shapes per unit class so they're visually
    // distinguishable on the map:
    //   Battalion  → square
    //   Brigade    → diamond (rotated square)
    //   Squadron   → circle
    //   TaskForce  → triangle
    //   Flight     → small circle (hollow)
    //   Package    → plus/cross
    if (impl_->show_units && impl_->world_loaded) {
        const float s = std::max(3.0f, 8.0f * (impl_->cam_zoom / 4.0f));
        for (int i = 0; i < static_cast<int>(impl_->world.units.size()); ++i) {
            const auto& u = impl_->world.units[i];
            const Vector2 p = impl_->world_to_screen(u.x, u.y);
            const RlColor c = color_for_owner(u.owner);
            const Color fill = {c.r, c.g, c.b, 220};

            switch (u.unit_class) {
                case f4::world::UnitClass::Battalion: {
                    const Rectangle rect = {p.x - s * 0.5f, p.y - s * 0.5f, s, s};
                    DrawRectangleRec(rect, fill);
                    break;
                }
                case f4::world::UnitClass::Brigade: {
                    // Diamond = square rotated 45°.
                    Vector2 verts[4] = {
                        {p.x,         p.y - s * 0.7f},
                        {p.x + s*0.7f,p.y         },
                        {p.x,         p.y + s * 0.7f},
                        {p.x - s*0.7f,p.y         },
                    };
                    DrawTriangle(verts[0], verts[1], verts[2], fill);
                    DrawTriangle(verts[0], verts[2], verts[3], fill);
                    break;
                }
                case f4::world::UnitClass::Squadron: {
                    DrawCircleV(p, s * 0.6f, fill);
                    break;
                }
                case f4::world::UnitClass::TaskForce: {
                    // Triangle pointing up.
                    Vector2 verts[3] = {
                        {p.x,         p.y - s * 0.7f},
                        {p.x + s*0.7f,p.y + s * 0.5f},
                        {p.x - s*0.7f,p.y + s * 0.5f},
                    };
                    DrawTriangle(verts[0], verts[1], verts[2], fill);
                    break;
                }
                case f4::world::UnitClass::Flight: {
                    DrawCircleLines(static_cast<int>(p.x), static_cast<int>(p.y),
                                    static_cast<int>(s * 0.6f), fill);
                    break;
                }
                case f4::world::UnitClass::Package: {
                    // Plus/cross.
                    DrawLineEx({p.x - s, p.y}, {p.x + s, p.y}, s * 0.4f, fill);
                    DrawLineEx({p.x, p.y - s}, {p.x, p.y + s}, s * 0.4f, fill);
                    break;
                }
                case f4::world::UnitClass::Unknown: {
                    const Rectangle rect = {p.x - s * 0.5f, p.y - s * 0.5f, s, s};
                    DrawRectangleRec(rect, fill);
                    break;
                }
            }

            // Destination line (only if moved)
            if (u.dest_x != u.x || u.dest_y != u.y) {
                const Vector2 d = impl_->world_to_screen(u.dest_x, u.dest_y);
                DrawLineEx(p, d, 1.0f, Color{c.r, c.g, c.b, 160});
            }

            // Selection outline (yellow, drawn around any shape)
            if (impl_->sel_kind == Impl::SelectionKind::Unit &&
                impl_->sel_index == i) {
                DrawCircleLines(static_cast<int>(p.x), static_cast<int>(p.y),
                                static_cast<int>(s + 4),
                                Color{255, 255, 0, 255});
            }
        }
    }
}

// ---------------------------------------------------------------------------
// ImGui panels
// ---------------------------------------------------------------------------
void ViewerApp::draw_imgui() {
    rlImGuiBegin();

    // --- Menu bar ---
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open World JSON...")) {
                open_file_dialog("Open World JSON", "*.world.json\0*.json\0",
                                 [this](const std::string& path) { load_world_json(path); });
            }
            if (ImGui::MenuItem("Open Terrain JSON...")) {
                open_file_dialog("Open Terrain JSON", "*.terrain.json\0*.json\0",
                                 [this](const std::string& path) { load_terrain_json(path); });
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Import .cam Archive...")) {
                open_file_dialog("Import .cam", "*.cam\0*.*\0",
                                 [this](const std::string& path) { import_cam_archive(path); });
            }
            if (ImGui::MenuItem("Import THEATER.* Binary...")) {
                // For directory selection we fall back to picking any file
                // in the directory — Raylib doesn't ship a folder picker.
                open_file_dialog("Pick any file in THEATER dir", "*.*\0",
                                 [this](const std::string& path) {
                                     import_terrain_binary(std::filesystem::path(path).parent_path());
                                 });
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit", "Alt+F4")) {
                // Raylib's WindowShouldClose() will pick this up — but we
                // need to actually break the loop. Simplest: close window.
                // ( rlImGui shutdown happens after the loop.)
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            ImGui::Checkbox("Terrain",     &impl_->show_terrain);
            ImGui::Checkbox("Objectives",  &impl_->show_objectives);
            ImGui::Checkbox("Units",       &impl_->show_units);
            ImGui::Checkbox("Grid",        &impl_->show_grid);
            ImGui::Checkbox("Legend",      &impl_->show_legend);
            ImGui::Separator();
            if (ImGui::MenuItem("Fit to World")) impl_->fit_to_world();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            ImGui::TextDisabled("F4 World Viewer");
            ImGui::TextDisabled("Pan: drag  Zoom: wheel  Select: click");
            ImGui::TextDisabled("Engine-agnostic F4 world inspector");
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    // --- Layers panel (left side) ---
    ImGui::SetNextWindowPos(ImVec2(10, 30), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(240, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Layers", nullptr,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Checkbox("Terrain",     &impl_->show_terrain);
        ImGui::Checkbox("Objectives",  &impl_->show_objectives);
        ImGui::Checkbox("Units",       &impl_->show_units);
        ImGui::Checkbox("Grid",        &impl_->show_grid);
        ImGui::Checkbox("Legend",      &impl_->show_legend);

        ImGui::Separator();
        ImGui::Text("Camera");
        ImGui::SliderFloat("Zoom", &impl_->cam_zoom, 0.1f, 30.0f, "%.2f");
        if (ImGui::Button("Fit to World")) impl_->fit_to_world();

        ImGui::Separator();
        ImGui::Text("Status");
        if (!impl_->status_msg.empty()) {
            ImGui::TextWrapped("%s", impl_->status_msg.c_str());
        }
        if (!impl_->last_error.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
            ImGui::TextWrapped("Error: %s", impl_->last_error.c_str());
            ImGui::PopStyleColor();
        }
    }
    ImGui::End();

    // --- Legend (right side, when toggled) ---
    if (impl_->show_legend) {
        ImGui::SetNextWindowPos(ImVec2(impl_->window_w - 230, 30), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Legend", &impl_->show_legend, ImGuiWindowFlags_NoCollapse)) {
            ImGui::TextUnformatted("Terrain");
            for (int t = 0; t <= 5; ++t) {
                const auto c = f4::terrain::TerrainData::color_for_tile_type(
                    static_cast<f4::terrain::TileType>(t));
                ImGui::PushStyleColor(ImGuiCol_Text,
                    ImVec4(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, 1.0f));
                ImGui::TextUnformatted("##");
                ImGui::PopStyleColor();
                ImGui::SameLine();
                ImGui::TextUnformatted(f4::terrain::tile_type_name(
                    static_cast<f4::terrain::TileType>(t)));
            }
            ImGui::Separator();
            ImGui::TextUnformatted("Teams (color)");
            const char* team_names[] = {
                "0 Neutral", "1 Enemy", "2 Friendly", "3 ROK",
                "4 Japan", "5 DPRK", "6 PRC", "7 Other"
            };
            for (int i = 0; i < 8; ++i) {
                const auto c = color_for_owner(static_cast<uint8_t>(i));
                ImGui::PushStyleColor(ImGuiCol_Text,
                    ImVec4(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, 1.0f));
                ImGui::TextUnformatted("##");
                ImGui::PopStyleColor();
                ImGui::SameLine();
                ImGui::TextUnformatted(team_names[i]);
            }
            ImGui::Separator();
            ImGui::TextUnformatted("Unit shapes");
            ImGui::TextUnformatted("[] Battalion");
            ImGui::TextUnformatted("<> Brigade");
            ImGui::TextUnformatted("o  Squadron");
            ImGui::TextUnformatted("^  TaskForce");
            ImGui::TextUnformatted("() Flight");
            ImGui::TextUnformatted("+  Package");
        }
        ImGui::End();
    }

    // --- Inspector (right side, below legend) ---
    ImGui::SetNextWindowPos(ImVec2(impl_->window_w - 320, 250), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(310, 380), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Inspector", nullptr, ImGuiWindowFlags_NoCollapse)) {
        if (impl_->sel_kind == Impl::SelectionKind::None || impl_->sel_index < 0) {
            ImGui::TextDisabled("Nothing selected");
            ImGui::TextDisabled("Click an objective or unit to inspect.");
        } else if (impl_->sel_kind == Impl::SelectionKind::Objective) {
            const auto& o = impl_->world.objectives[impl_->sel_index];
            ImGui::Text("Objective #%d", impl_->sel_index);
            ImGui::Separator();
            ImGui::Text("Type:      %d", o.type);
            ImGui::Text("Position:   (%d, %d, %.0f ft)", o.x, o.y, o.z);
            ImGui::Text("Owner:      %d", o.owner);
            ImGui::Text("Priority:   %d", o.priority);
            ImGui::Text("Name ID:    %d", o.nameid);
            ImGui::Text("Camp ID:    %d", o.camp_id);
            ImGui::Text("Entity:     %d", o.entity_type);
            ImGui::Text("VU_ID:      0x%08x/0x%08x", o.id_creator, o.id_num);
        } else if (impl_->sel_kind == Impl::SelectionKind::Unit) {
            const auto& u = impl_->world.units[impl_->sel_index];
            ImGui::Text("Unit #%d", impl_->sel_index);
            ImGui::Separator();
            ImGui::Text("Class:     %s (type %d)",
                        f4::world::unit_class_name(u.unit_class), u.type);
            ImGui::Text("Position:  (%d, %d, %.0f ft)", u.x, u.y, u.z);
            ImGui::Text("Owner:     %d", u.owner);
            ImGui::Text("Destination:(%d, %d)", u.dest_x, u.dest_y);
            ImGui::Text("Name ID:   %d", u.name_id);
            ImGui::Text("Camp ID:   %d", u.camp_id);
            ImGui::Text("Reinforc.: %d", u.reinforcement);
            ImGui::Text("Waypoints: %d", u.wp_count);
            ImGui::Text("Losses:    %d", u.losses);
            ImGui::Text("Entity:    %d", u.entity_type);
            ImGui::Text("VU_ID:     0x%08x/0x%08x", u.id_creator, u.id_num);
            // Subclass-specific fields:
            ImGui::Separator();
            switch (u.unit_class) {
                case f4::world::UnitClass::Battalion:
                case f4::world::UnitClass::Brigade:
                    ImGui::Text("Supply:    %d%%", u.supply);
                    ImGui::Text("Morale:    %d%%", u.morale);
                    ImGui::Text("Fatigue:   %d%%", u.fatigue);
                    if (u.unit_class == f4::world::UnitClass::Brigade) {
                        ImGui::Text("Elements:  %d", u.elements);
                    }
                    break;
                case f4::world::UnitClass::Squadron:
                    ImGui::Text("Fuel:      %d lbs", u.fuel);
                    break;
                case f4::world::UnitClass::TaskForce:
                    ImGui::Text("Supply:    %d%%", u.supply);
                    break;
                case f4::world::UnitClass::Flight:
                case f4::world::UnitClass::Package:
                case f4::world::UnitClass::Unknown:
                    break;
            }
        }
    }
    ImGui::End();

    // --- Status bar (bottom) ---
    ImGui::SetNextWindowPos(ImVec2(0, impl_->window_h - 24));
    ImGui::SetNextWindowSize(ImVec2(impl_->window_w, 24));
    if (ImGui::Begin("##status", nullptr,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoBringToFrontOnFocus)) {
        const Vector2 mouse = GetMousePosition();
        float gx, gy;
        impl_->screen_to_world(mouse.x, mouse.y, &gx, &gy);
        ImGui::Text("Cursor: (%.1f, %.1f)  Zoom: %.2fx  FPS: %d",
                    gx, gy, impl_->cam_zoom, GetFPS());
        if (impl_->world_loaded) {
            ImGui::SameLine();
            ImGui::TextDisabled("|  %d objectives  %d units",
                                impl_->world.objectives.size(),
                                impl_->world.units.size());
        }
    }
    ImGui::End();

    // --- Pending file dialog modal ---
    // NOTE: must be inside the rlImGuiBegin/End block — calling ImGui
    // functions after rlImGuiEnd() crashes because the ImGui frame is
    // already finalized (the ID stack is empty, GetID() dereferences
    // an empty ImVector).
    if (impl_->pending_dialog_open) {
        if (!ImGui::IsPopupOpen("FilePicker") && !impl_->pending_dialog_title.empty()) {
            ImGui::OpenPopup("FilePicker");
        }
        ImGui::SetNextWindowSize(ImVec2(500, 140), ImGuiCond_FirstUseEver);
        if (ImGui::BeginPopupModal(impl_->pending_dialog_title.c_str(),
                                    &impl_->pending_dialog_open,
                                    ImGuiWindowFlags_NoResize)) {
            ImGui::TextUnformatted("Path:");
            ImGui::SameLine();
            ImGui::PushItemWidth(-120);
            ImGui::InputText("##path", impl_->pending_dialog_path,
                             sizeof(impl_->pending_dialog_path));
            ImGui::PopItemWidth();
            ImGui::SameLine();
            if (ImGui::Button("OK")) {
                std::string p = impl_->pending_dialog_path;
                if (!p.empty()) {
                    auto cb = std::move(impl_->pending_dialog_callback);
                    impl_->pending_dialog_callback = nullptr;
                    impl_->pending_dialog_open = false;
                    impl_->pending_dialog_title.clear();
                    try {
                        cb(p);
                    } catch (const std::exception& e) {
                        impl_->last_error = e.what();
                    }
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                impl_->pending_dialog_open = false;
                impl_->pending_dialog_title.clear();
                impl_->pending_dialog_callback = nullptr;
                ImGui::CloseCurrentPopup();
            }
            if (!impl_->last_world_json_path.empty()) {
                ImGui::Separator();
                ImGui::TextDisabled("Last world JSON: %s",
                                    impl_->last_world_json_path.string().c_str());
            }
            ImGui::EndPopup();
        }
    }

    rlImGuiEnd();
}

// ---------------------------------------------------------------------------
// File dialog helper — Raylib doesn't ship a native picker, so we use a
// simple ImGui text-input modal. The user can paste a path or type one in.
// A real file browser will replace this in a future pass — likely via
// tinyfiledialogs (which uses the OS native dialog on Windows/macOS/Linux).
// ---------------------------------------------------------------------------
void ViewerApp::open_file_dialog(const char* title, const char* filters,
                                  std::function<void(const std::string&)> on_ok) {
    impl_->pending_dialog_title = title;
    impl_->pending_dialog_filters = filters ? filters : "";
    impl_->pending_dialog_callback = std::move(on_ok);
    impl_->pending_dialog_path[0] = '\0';
    impl_->pending_dialog_open = true;
}

} // namespace f4::viewer
