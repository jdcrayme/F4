// f4-world-viewer/src/viewer_state.hpp
//
// PRIVATE HEADER — internal to the f4-world-viewer library. Not installed,
// not visible to consumers. Every viewer .cpp file includes this so it can
// access ViewerApp::Impl (the pimpl struct that holds all render-loop
// state) and the inline color helpers used by both the canvas and the
// ImGui legend panel.
//
// The split of the original 1920-LoC viewer_app.cpp god-file (item #5 of
// the architecture review) keeps the public API in viewer_app.hpp and
// moves the Impl struct + per-concern implementations into:
//   viewer_state.hpp    — this file (Impl struct + color helpers)
//   viewer_app.cpp      — lifecycle (ctor/dtor/run) + small helpers
//   icons.cpp           — Impl icon-table + draw_icon + icon_for_*
//   camera.cpp          — Impl world<->screen transforms + fit_to_world
//   file_ops.cpp        — ViewerApp::load_*_json / import_*
//   install_flow.cpp    — ViewerApp::set_install_path* / open_campaign_dialog
//                         / load_campaign_from_install
//   diagnostics.cpp     — build_install_diagnostics / build_campaign_load_error
//                         free functions + ViewerApp::install_diagnostics_text
//                         / open_install_diagnostics
//   canvas.cpp          — ViewerApp::handle_input / draw_canvas
//   imgui_panels.cpp    — ViewerApp::draw_imgui / open_file_dialog
//
// Cross-file internal helpers (the diagnostics builders) live in
// diagnostics.hpp alongside this header.

#pragma once

#include <f4/viewer/viewer_app.hpp>
#include <f4/viewer/hex_inspector.hpp>
#include <f4/viewer/settings.hpp>

#include <f4/install/installation.hpp>
#include <f4/terrain/terrain_data.hpp>
#include <f4/world/world_state.hpp>

#include <raylib.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace f4::viewer {

// ---------------------------------------------------------------------------
// Color helpers — keyed by owner (Control) byte. Same scheme as the
// late f4-world-vis SVG renderer (which we deleted in favor of this viewer).
// Defined inline here because they're used by both canvas.cpp (terrain
// tile rendering, objective icon tinting, unit fill colors) and
// imgui_panels.cpp (the legend panel swatches).
// ---------------------------------------------------------------------------
struct RlColor { unsigned char r, g, b, a; };

inline RlColor color_for_owner(uint8_t owner) {
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

inline RlColor to_rl(const f4::terrain::Color4& c) {
    return {c.r, c.g, c.b, c.a};
}

// ---------------------------------------------------------------------------
// ViewerApp::Impl — all state the render loop touches, in one struct.
//
// The fields are grouped by concern (window/camera, data, selection, layer
// toggles, status, install-aware state, modals, hex inspector, screenshots)
// so a reader can find what they need without scanning the whole struct.
//
// Member-function definitions that need to touch this struct (load_icons,
// draw_icon, icon_for_*, world_to_screen, screen_to_world, fit_to_world,
// rebuild_objective_index) live in icons.cpp and camera.cpp — declared
// here, defined there. The free functions in diagnostics.cpp take a
// const Installation& and don't need Impl access.
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
    bool initial_camera_set = false;  // true if set_initial_camera() was called

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
    bool show_routes = true;       // road/rail network from objective link_data

    // Status
    std::string status_msg;
    std::string last_error;

    // Conversion cache — paths used by the most recent import operation.
    std::filesystem::path last_world_json_path;
    std::filesystem::path last_terrain_json_path;

    // --- Install-aware state (new primary flow) ---

    // The Falcon 4.0 installation the user pointed at. std::nullopt until
    // they pick one (or until we restore it from settings on startup).
    std::optional<f4::install::Installation> install;

    // Persisted viewer settings — install path, last theater/campaign, etc.
    // Loaded on construction, saved on every change.
    ViewerSettings settings;

    // Theater + Campaign picker modal state. We track the selected indices
    // (into install->theaters() and the filtered campaigns list) so the
    // ImGui Combo can show the current selection. Recomputed on open.
    bool campaign_dialog_open = false;
    int campaign_dialog_theater_idx = 0;   // index into install->theaters()
    int campaign_dialog_campaign_idx = 0;  // index into filtered list
    std::vector<f4::install::Campaign> campaign_dialog_campaigns;  // for current theater

    // Install summary modal state — shown after Set Install Path to
    // confirm what was detected (theaters, campaigns, class table).
    bool install_summary_open = false;
    std::string install_summary_text;

    // Install diagnostics modal state — shown via Tools > Install
    // Diagnostics. More detailed than the summary modal: includes every
    // path probed for FALCON4.ct, every theater dir probed, etc.
    bool install_diagnostics_open = false;
    std::string install_diagnostics_text;

    // Campaign load error modal state. When load_campaign_from_install
    // throws, we capture the exception message + diagnostic context
    // (theater complete?, .cam file exists?, class table found?) into
    // this string and show it in a proper modal so the user can copy
    // the full text. More useful than a native message box because the
    // text is selectable and scrollable.
    bool campaign_load_error_open = false;
    std::string campaign_load_error_text;

    // Hex Inspector panel — owned by the viewer, opened via Tools menu.
    HexInspector hex_inspector;

    // Pending file dialog (legacy fallback — used by File > Advanced >
    // ... menu items when tinyfiledialogs is unavailable or for ad-hoc
    // path entry. We keep it around because the native picker doesn't
    // support filter overrides the way the old modal did, and it's
    // useful as a back door.)
    bool pending_dialog_open = false;
    char pending_dialog_path[1024] = {0};
    std::string pending_dialog_title;
    std::string pending_dialog_filters;
    std::function<void(const std::string&)> pending_dialog_callback;

    // Scheduled screenshot (for headless smoke tests)
    bool screenshot_pending = false;
    double screenshot_at = 0.0;    // GetTime() value
    std::string screenshot_path;

    // VU_ID.num → objective index lookup. Built when a world is loaded.
    // Used by the routes layer to resolve link neighbor VU_IDs to
    // objective positions (so we can draw lines between them).
    std::unordered_map<uint32_t, int> obj_id_to_index;

    /// VU_ID.num → unit index lookup. Built when a world is loaded.
    /// Used to resolve Squadron→Airbase link lines and Battalion→Brigade
    /// hierarchy lines (when the parent_id / airbase_id refers to another
    /// entity by VU_ID.num).
    std::unordered_map<uint32_t, int> unit_id_to_index;

    /// Rebuild the VU_ID → objective index map. Call after loading a world.
    /// Defined in camera.cpp.
    void rebuild_objective_index();

    // Icon textures (loaded from f4-world-viewer/assets/icons/*.png).
    // The spritesheet has 24 icons across 4 rows × 6 cols:
    //   Row 1: bridge, village, town, city, factory, road_intersection
    //   Row 2: armybase, sam_site, airbase, airstrip, port, road
    //   Row 3: harts, armor, artillery, supply, infantry, engineering
    //   Row 4: fighter, bomber, transport, helicopter, naval_surface, carrier
    // Plus legacy icons: powerplant, radar, railroad, square, diamond, circle, triangle
    //
    // Icon name → index mapping. Loaded by name; -1 = not loaded.
    enum IconIndex : int {
        // Row 1 (objectives):
        ICON_BRIDGE = 0,
        ICON_VILLAGE,
        ICON_TOWN,
        ICON_CITY,
        ICON_FACTORY,
        ICON_ROAD_INTERSECTION,
        // Row 2 (objectives):
        ICON_ARMYBASE,
        ICON_SAM_SITE,
        ICON_AIRBASE,
        ICON_AIRSTRIP,
        ICON_PORT,
        ICON_ROAD,
        // Row 3 (ground unit subtypes):
        ICON_HARTS,
        ICON_ARMOR,
        ICON_ARTILLERY,
        ICON_SUPPLY,
        ICON_INFANTRY,
        ICON_ENGINEERING,
        // Row 4 (air/naval unit subtypes):
        ICON_FIGHTER,
        ICON_BOMBER,
        ICON_TRANSPORT,
        ICON_HELICOPTER,
        ICON_NAVAL_SURFACE,
        ICON_CARRIER,
        // Legacy icons (kept for backward compat):
        ICON_POWERPLANT,
        ICON_RADAR,
        ICON_RAILROAD,
        ICON_SQUARE,
        ICON_DIAMOND,
        ICON_CIRCLE,
        ICON_TRIANGLE,
        ICON_COUNT
    };

    Texture2D icons[ICON_COUNT] = {};
    bool icons_loaded = false;

    /// Load all icon PNGs from the assets/icons/ directory. Called once at
    /// startup. Falls back to drawn shapes if not found.
    /// Defined in icons.cpp.
    void load_icons();

    /// Draw an icon centered at screen position (sx, sy) with the given
    /// pixel size. The icon is tinted by the owner color so team affiliation
    /// is still visible. If no icon is loaded, falls back to a small drawn
    /// circle (sized independently of priority so unknown objectives don't
    /// become giant discs). Defined in icons.cpp.
    void draw_icon(int icon_idx, float sx, float sy, float size_px,
                   const RlColor& tint);

    /// Map an ObjectiveType (enum 1-39, from the class table) to an icon.
    /// Returns -1 if no icon exists for this type. Defined in icons.cpp.
    static int icon_for_objective_type(uint8_t obj_type);

    /// Map a unit_class + unit_subtype to an icon. Uses the subtype to pick
    /// a specific icon (armor/infantry/fighter/bomber/...) when available;
    /// falls back to the generic shape icon (square/diamond/circle/triangle)
    /// if no subtype-specific icon exists. Defined in icons.cpp.
    int icon_for_unit(f4::world::UnitClass cls, uint8_t subtype) const;

    // --- Camera transforms (defined in camera.cpp) ---

    /// Convert world (grid) coordinates to screen pixels.
    Vector2 world_to_screen(float gx, float gy) const;

    /// Convert screen pixels to world (grid) coordinates.
    void screen_to_world(float sx, float sy, float* gx, float* gy) const;

    /// Fit camera to show the entire theater grid (1024x1024 by default).
    void fit_to_world();
};

} // namespace f4::viewer
