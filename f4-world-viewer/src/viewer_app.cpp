// f4-world-viewer/src/viewer_app.cpp
//
// Interactive Raylib + Dear ImGui application for inspecting F4 world data.
//
// Layout:
//   ┌──────────────────────────────────────────────────────────────────┐
//   │ Menu bar: File | View | Help                                    │
//   ├──────────────────────────────────────────┬───────────────────────┤
//   │ Raylib canvas (2D top-down)              │ ImGui: layers         │
//   │   • Color-coded terrain tiles            │   + inspector         │
//   │   • Objective icons by type + team       │   + status            │
//   │   • Unit squares by team                 │   + install info      │
//   │   Pan: drag  Zoom: wheel  Click: select  │                       │
//   └──────────────────────────────────────────┴───────────────────────┘
//
// Primary user flow (install-aware):
//
//   File > Set Install Path...   → native folder picker → detect()
//   File > Open Campaign...      → Theater + Campaign dropdowns
//                                  → in-process terrain2json + cam2json
//                                    with FALCON4.ct auto-resolved
//
// The viewer wraps the cam2json and terrain2json CLIs in-process (calls
// the libraries directly), so the user can import raw FreeFalcon binary
// files without leaving the app. This is the starting point for a future
// world editor: the same load/render pipeline will gain edit/save
// capabilities as new systems come online.

#include <f4/viewer/viewer_app.hpp>

#include <f4/world_convert/cam_archive.hpp>
#include <f4/world_convert/world_json.hpp>
#include <f4/terrain_convert/terrain_converter.hpp>
#include <f4/world_convert/class_table.hpp>
#include <f4/install/installation.hpp>
#include <f4/terrain/terrain_data.hpp>
#include <f4/viewer/file_dialog.hpp>
#include <f4/viewer/hex_inspector.hpp>
#include <f4/viewer/settings.hpp>
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
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
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

    /// Rebuild the VU_ID → objective index map. Call after loading a world.
    void rebuild_objective_index() {
        obj_id_to_index.clear();
        obj_id_to_index.reserve(world.objectives.size());
        for (int i = 0; i < static_cast<int>(world.objectives.size()); ++i) {
            obj_id_to_index[world.objectives[i].id_num] = i;
        }
    }

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
    void load_icons() {
        if (icons_loaded) return;
        const char* names[] = {
            "bridge", "village", "town", "city", "factory", "road_intersection",
            "armybase", "sam_site", "airbase", "airstrip", "port", "road",
            "harts", "armor", "artillery", "supply", "infantry", "engineering",
            "fighter", "bomber", "transport", "helicopter", "naval_surface", "carrier",
            "powerplant", "radar", "railroad",
            "square", "diamond", "circle", "triangle"
        };
        static_assert(sizeof(names)/sizeof(names[0]) == ICON_COUNT,
                      "icon name table size mismatch");
        const char* search_dirs[] = {
            "assets/icons",
            "../assets/icons",
            "../../assets/icons",
            "../../../f4-world-viewer/assets/icons",
        };
        for (const char* dir : search_dirs) {
            bool found_any = false;
            for (int i = 0; i < ICON_COUNT; ++i) {
                std::string path = std::string(dir) + "/" + names[i] + ".png";
                if (FileExists(path.c_str())) {
                    icons[i] = LoadTexture(path.c_str());
                    SetTextureFilter(icons[i], TEXTURE_FILTER_BILINEAR);
                    found_any = true;
                }
            }
            if (found_any) break;
        }
        icons_loaded = true;
    }

    /// Draw an icon centered at screen position (sx, sy) with the given
    /// pixel size. The icon is tinted by the owner color so team affiliation
    /// is still visible. If no icon is loaded, falls back to a small drawn
    /// circle (sized independently of priority so unknown objectives don't
    /// become giant discs).
    void draw_icon(int icon_idx, float sx, float sy, float size_px,
                   const RlColor& tint) {
        if (icon_idx < 0 || icon_idx >= ICON_COUNT || icons[icon_idx].id == 0) {
            // Fallback circle: fixed small radius so unknown objectives
            // stay readable when zoomed out, instead of giant discs.
            const float fallback_radius = std::min(size_px * 0.4f, 5.0f);
            DrawCircleV({sx, sy}, fallback_radius,
                        Color{tint.r, tint.g, tint.b, 220});
            return;
        }
        const Texture2D& tex = icons[icon_idx];
        const Rectangle src = {0, 0,
                               static_cast<float>(tex.width),
                               static_cast<float>(tex.height)};
        const Rectangle dst = {sx - size_px * 0.5f, sy - size_px * 0.5f,
                               size_px, size_px};
        const Vector2 origin = {0, 0};
        DrawTexturePro(tex, src, dst, origin, 0.0f,
                       Color{tint.r, tint.g, tint.b, 255});
    }

    /// Map an ObjectiveType (enum 1-39, from the class table) to an icon.
    /// Returns -1 if no icon exists for this type.
    static int icon_for_objective_type(uint8_t obj_type) {
        switch (obj_type) {
            case 1:  return ICON_AIRBASE;            // TYPE_AIRBASE
            case 2:  return ICON_AIRSTRIP;           // TYPE_AIRSTRIP
            case 3:  return ICON_ARMYBASE;           // TYPE_ARMYBASE
            case 6:  return ICON_BRIDGE;             // TYPE_BRIDGE
            case 8:  return ICON_CITY;               // TYPE_CITY
            case 11: return ICON_FACTORY;            // TYPE_FACTORY
            case 15: return ICON_ROAD_INTERSECTION;  // TYPE_INTERSECT
            case 19: return ICON_PORT;               // TYPE_PORT
            case 26: return ICON_ROAD;               // TYPE_ROAD
            case 28: return ICON_TOWN;               // TYPE_TOWN
            case 29: return ICON_VILLAGE;            // TYPE_VILLAGE
            case 30: return ICON_HARTS;              // TYPE_HARTS
            case 31: return ICON_SAM_SITE;           // TYPE_SAM_SITE
            // Legacy icons (kept for types without a dedicated new icon):
            case 20: return ICON_POWERPLANT;         // TYPE_POWERPLANT
            case 21: return ICON_RADAR;              // TYPE_RADAR
            case 24: return ICON_RAILROAD;           // TYPE_RAILROAD
            case 23: return ICON_RAILROAD;           // TYPE_RAIL_TERMINAL (reuse)
            // Types without icons — fall back to circle:
            // 4=BEACH, 5=BORDER, 7=CHEMICAL, 9=COM_CONTROL, 10=DEPOT,
            // 12=FORD, 13=FORTIFICATION, 14=HILL_TOP, 17=NUCLEAR, 18=PASS,
            // 22=RADIO_TOWER, 25=REFINERY, 39=AIR_TERMINAL
            default: return -1;
        }
    }

    /// Map a unit_class + unit_subtype to an icon. Uses the subtype to pick
    /// a specific icon (armor/infantry/fighter/bomber/...) when available;
    /// falls back to the generic shape icon (square/diamond/circle/triangle)
    /// if no subtype-specific icon exists.
    int icon_for_unit(f4::world::UnitClass cls, uint8_t subtype) const {
        // Land battalions/brigades: use subtype-specific ground icons.
        if (cls == f4::world::UnitClass::Battalion ||
            cls == f4::world::UnitClass::Brigade) {
            switch (subtype) {
                case 3:  return ICON_ARMOR;        // STYPE_LAND_ARMOR
                case 5:  return ICON_ENGINEERING;  // STYPE_LAND_ENGINEER
                case 7:  return ICON_INFANTRY;     // STYPE_LAND_INFANTRY
                case 11: return ICON_ARTILLERY;    // STYPE_LAND_SP_ARTILLERY
                case 13: return ICON_SUPPLY;       // STYPE_LAND_SUPPLY
                case 14: return ICON_ARTILLERY;    // STYPE_LAND_TOWED_ARTILLERY (reuse)
                // No dedicated icon for: 1=AIR_DEFENSE, 2=AIRMOBILE, 4=ARMORED_CAV,
                // 6=HQ, 8=MARINE, 9=MECHANIZED, 10=ROCKET, 12=SS_MISSILE
                default: break;
            }
            // Fall back to generic shape.
            return (cls == f4::world::UnitClass::Battalion) ? ICON_SQUARE : ICON_DIAMOND;
        }
        // Squadrons: use subtype-specific air icons.
        if (cls == f4::world::UnitClass::Squadron) {
            switch (subtype) {
                case 1:  return ICON_TRANSPORT;    // STYPE_AIR_AIR_TRANSPORT
                case 4:  return ICON_HELICOPTER;   // STYPE_AIR_ATTACK_HELO
                case 6:  return ICON_BOMBER;       // STYPE_AIR_BOMBER
                case 8:  return ICON_FIGHTER;      // STYPE_AIR_FIGHTER
                case 9:  return ICON_FIGHTER;      // STYPE_AIR_FIGHTER_BOMBER (reuse)
                case 13: return ICON_TRANSPORT;    // STYPE_AIR_TANKER (reuse transport)
                case 14: return ICON_HELICOPTER;   // STYPE_AIR_TRANSPORT_HELO
                // No dedicated icon for: 2=ASW, 3=ATTACK, 5=AWACS, 7=ECM,
                // 10=JSTAR, 11=RECON, 12=RECON_HELO
                default: break;
            }
            return ICON_CIRCLE;
        }
        // Task forces: use subtype-specific naval icons.
        if (cls == f4::world::UnitClass::TaskForce) {
            switch (subtype) {
                case 3:  return ICON_CARRIER;        // STYPE_SEA_CARRIER
                // No dedicated icon for: 1=AMPHIBIOUS, 2=BATTLESHIP, 4=CRUISER,
                // 5=DESTROYER, 6=FRIGATE, 7=PATROL, 8/9/10=supply/tanker/transport
                default: return ICON_TRIANGLE;
            }
        }
        return -1;
    }

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
ViewerApp::ViewerApp()  : impl_(std::make_unique<Impl>()) {
    // Restore the last install path from persisted settings. If the user
    // has already pointed at a Falcon install, we don't make them do it
    // again on every launch — detect() runs in ~50ms, fast enough that
    // there's no perceptible startup delay.
    impl_->settings = load_settings();
    if (!impl_->settings.install_path.empty()) {
        try {
            auto inst = f4::install::Installation::detect(impl_->settings.install_path);
            if (inst.valid()) {
                impl_->install = std::move(inst);
            }
        } catch (const std::exception&) {
            // Settings file may point at a path that no longer exists.
            // Leave install as std::nullopt; the user will be prompted
            // to set a new path when they try to open a campaign.
        }
    }
}

ViewerApp::~ViewerApp() = default;

void ViewerApp::run() {
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
    InitWindow(impl_->window_w, impl_->window_h, "F4 World Viewer");
    SetTargetFPS(60);
    rlImGuiSetup(true);

    // Load icon textures (falls back to drawn shapes if not found).
    impl_->load_icons();

    // Default to a fit-to-world view — UNLESS the caller already set an
    // initial camera via set_initial_camera() (e.g. via --zoom/--center
    // CLI flags). In that case, respect the user's choice.
    if (!impl_->initial_camera_set) {
        impl_->fit_to_world();
    }

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
    impl_->rebuild_objective_index();

    // Try to auto-load the referenced terrain file — but only if terrain
    // isn't already loaded. This prevents a spurious "auto-load failed"
    // error when load_campaign_from_install() has already loaded terrain
    // before calling us: the world JSON's terrain_file field is relative
    // to the .cam's directory (e.g. "terrain.json"), and that path may
    // not resolve correctly from CWD.
    if (!impl_->world.terrain_file.empty() && !impl_->world.terrain_loaded) {
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
    f4::terrain_convert::convert_terrain_dir(terrain_dir, out, "korea");
    load_terrain_json(out);
}

void ViewerApp::import_cam_archive(const std::filesystem::path& cam_path) {
    f4::world_convert::CamArchive cam;
    cam.load(cam_path);
    f4::world_convert::WorldJsonOptions opts;
    opts.theater = "korea";
    opts.terrain_file = "korea.terrain.json";

    // Auto-search for FALCON4.ct (the class table) in a few standard
    // locations. Without it, objectives carry only their raw entity_type
    // and the viewer can't pick icons — they all fall back to circles.
    // The class table is a binary file shipped with the game data; the
    // repo bundles a copy in f4-world-convert/tests/fixtures/FALCON4.ct
    // so we can resolve types out-of-the-box.
    f4::world_convert::ClassTable class_table;
    const auto ct_path = f4::world_convert::find_class_table(cam_path);
    if (!ct_path.empty()) {
        try {
            class_table.load(ct_path);
            opts.class_table = &class_table;
            impl_->status_msg = "Loaded class table: " + ct_path.string() +
                " (" + std::to_string(class_table.size()) + " entries)";
        } catch (const std::exception& e) {
            impl_->last_error = "Class table load failed: " + std::string(e.what());
        }
    } else {
        impl_->last_error =
            "FALCON4.ct not found — objectives will render as fallback circles "
            "(no icon mapping). Place FALCON4.ct next to the .cam or in assets/.";
    }

    const std::string json = f4::world_convert::to_world_json(cam, opts);

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

void ViewerApp::set_initial_camera(float center_x, float center_y, float zoom) {
    impl_->cam_x = center_x;
    impl_->cam_y = center_y;
    impl_->cam_zoom = zoom;
    impl_->initial_camera_set = true;
}

// ---------------------------------------------------------------------------
// Install-aware API (new primary flow)
// ---------------------------------------------------------------------------
bool ViewerApp::set_install_path_dialog() {
    // Use the current install path (or last world JSON dir) as the
    // starting point for the folder picker — saves navigation.
    std::filesystem::path start = impl_->settings.install_path;
    if (start.empty() && !impl_->last_world_json_path.empty()) {
        start = impl_->last_world_json_path.parent_path();
    }

    auto path = pick_folder("Select Falcon 4.0 Install Directory", start);
    if (path.empty()) return false;  // user cancelled

    return set_install_path(path);
}

bool ViewerApp::set_install_path(const std::filesystem::path& path) {
    try {
        auto inst = f4::install::Installation::detect(path);
        if (!inst.valid()) {
            impl_->last_error = "Not a Falcon 4.0 install: " + path.string() +
                "\n\nExpected: a directory containing FALCON4.ct and/or a terrdata/ subdirectory.";
            show_message_box("Invalid Install Path", impl_->last_error, "warning");
            return false;
        }
        impl_->install = std::move(inst);
        impl_->settings.install_path = path;
        save_settings(impl_->settings);

        // Build a summary for the confirmation modal.
        std::ostringstream ss;
        ss << "Install detected successfully.\n\n";
        ss << "Root: " << impl_->install->root().string() << "\n\n";

        ss << "Theaters: " << impl_->install->theaters().size() << "\n";
        for (const auto& t : impl_->install->theaters()) {
            ss << "  - " << t.display_name << " (" << t.key << ")";
            ss << (t.complete() ? "" : " [INCOMPLETE]");
            ss << "\n";
            // Show which THEATER.* files are present (helps diagnose
            // incomplete theaters — missing THEATER.MAP or .MEA breaks
            // campaign loading for every campaign in that theater).
            ss << "      files: ";
            if (t.theater_files.empty()) {
                ss << "(none)";
            } else {
                bool first = true;
                for (const auto& f : t.theater_files) {
                    if (!first) ss << ", ";
                    ss << f.filename().string();
                    first = false;
                }
            }
            ss << "\n";
        }

        ss << "\nCampaigns: " << impl_->install->campaigns().size() << "\n";
        // Show the first few campaigns with their paths so the user can
        // verify the layout (flat vs. nested) is what we expect.
        const std::size_t max_camp_show = 5;
        for (std::size_t i = 0;
             i < std::min(impl_->install->campaigns().size(), max_camp_show); ++i) {
            const auto& c = impl_->install->campaigns()[i];
            ss << "  - " << c.stem;
            if (!c.theater_key.empty()) ss << "  [" << c.theater_key << "]";
            ss << "\n      " << c.cam.string() << "\n";
        }
        if (impl_->install->campaigns().size() > max_camp_show) {
            ss << "  ... and " << (impl_->install->campaigns().size() - max_camp_show)
               << " more\n";
        }

        ss << "\nClass table: ";
        if (impl_->install->class_table().empty()) {
            ss << "NOT FOUND\n";
            ss << "  Searched:\n";
            for (const auto& p : impl_->install->diagnostics().class_table_searched) {
                ss << "    " << p.string() << "\n";
            }
            ss << "  (Objectives will lack icons. Use Tools > Install Diagnostics\n"
               << "   for more detail, or place FALCON4.ct in one of these locations.)\n";
        } else {
            ss << impl_->install->class_table().string() << "\n";
        }
        impl_->install_summary_text = ss.str();
        impl_->install_summary_open = true;
        impl_->status_msg = "Install: " + path.string();
        return true;
    } catch (const std::exception& e) {
        impl_->last_error = std::string("Install detection failed: ") + e.what();
        show_message_box("Install Detection Failed", impl_->last_error, "error");
        return false;
    }
}

const std::optional<f4::install::Installation>&
ViewerApp::installation() const noexcept {
    return impl_->install;
}

void ViewerApp::open_campaign_dialog() {
    if (!impl_->install || !impl_->install->valid()) {
        // No install set — prompt the user to pick one first.
        show_message_box("No Install Set",
                          "You need to set the Falcon 4.0 install path first.\n"
                          "Use File > Set Install Path... to pick the directory.",
                          "warning");
        return;
    }
    if (impl_->install->theaters().empty()) {
        show_message_box("No Theaters Found",
                          "The install at " + impl_->install->root().string() +
                          "\ncontains no theaters under terrdata/.\n"
                          "Make sure the install is intact.",
                          "warning");
        return;
    }

    // Pre-select the last theater the user picked, if it's still present.
    impl_->campaign_dialog_theater_idx = 0;
    if (!impl_->settings.last_theater_key.empty()) {
        for (size_t i = 0; i < impl_->install->theaters().size(); ++i) {
            if (impl_->install->theaters()[i].key == impl_->settings.last_theater_key) {
                impl_->campaign_dialog_theater_idx = static_cast<int>(i);
                break;
            }
        }
    }

    // Populate the campaigns list for the selected theater.
    const auto& theater = impl_->install->theaters()[impl_->campaign_dialog_theater_idx];
    impl_->campaign_dialog_campaigns = impl_->install->campaigns_for(theater.key);
    impl_->campaign_dialog_campaign_idx = 0;

    // Pre-select last campaign stem, if still present.
    if (!impl_->settings.last_campaign_stem.empty()) {
        for (size_t i = 0; i < impl_->campaign_dialog_campaigns.size(); ++i) {
            if (impl_->campaign_dialog_campaigns[i].stem == impl_->settings.last_campaign_stem) {
                impl_->campaign_dialog_campaign_idx = static_cast<int>(i);
                break;
            }
        }
    }

    impl_->campaign_dialog_open = true;
}

void ViewerApp::load_campaign_from_install(const std::string& theater_key,
                                             const std::string& campaign_stem) {
    if (!impl_->install) {
        throw std::runtime_error("load_campaign_from_install: no install set");
    }
    const auto* theater = impl_->install->find_theater(theater_key);
    if (!theater) {
        throw std::runtime_error("theater not found: " + theater_key);
    }
    if (!theater->complete()) {
        throw std::runtime_error("theater '" + theater_key +
            "' is incomplete (missing THEATER.MAP or .MEA)");
    }

    // Find the campaign in this theater with the matching stem.
    auto camps = impl_->install->campaigns_for(theater_key);
    const f4::install::Campaign* camp = nullptr;
    for (const auto& c : camps) {
        if (c.stem == campaign_stem) { camp = &c; break; }
    }
    if (!camp) {
        throw std::runtime_error("campaign '" + campaign_stem +
            "' not found in theater '" + theater_key + "'");
    }

    // Step 1: convert THEATER.* → terrain JSON in a temp file next to
    // the theater dir. We use a temp file rather than in-memory because
    // f4-terrain's loader expects a path.
    const auto terrain_json = theater->dir / "terrain.json";
    f4::terrain_convert::convert_terrain_dir(theater->dir, terrain_json, theater_key);
    impl_->world.terrain.load_terrain_json(terrain_json);
    impl_->world.terrain_loaded = true;
    impl_->last_terrain_json_path = terrain_json;
    impl_->status_msg = "Terrain: " + theater_key + " (" +
        std::to_string(impl_->world.terrain.header.width) + "x" +
        std::to_string(impl_->world.terrain.header.height) + ")";

    // Step 2: convert .cam → world JSON using the install's class table.
    // Use the install-aware resolver — finds FALCON4.ct automatically.
    f4::world_convert::CamArchive cam;
    cam.load(camp->cam);
    f4::world_convert::WorldJsonOptions opts;
    opts.theater = theater_key;
    opts.terrain_file = terrain_json.filename().string();

    f4::world_convert::ClassTable class_table;
    const auto ct_path = impl_->install->find_class_table(camp->cam);
    if (!ct_path.empty()) {
        try {
            class_table.load(ct_path);
            opts.class_table = &class_table;
        } catch (const std::exception& e) {
            impl_->last_error = "Class table load failed: " + std::string(e.what());
        }
    } else {
        impl_->last_error = "FALCON4.ct not found — objectives will lack icons";
    }

    const std::string json = f4::world_convert::to_world_json(cam, opts);

    // Write next to the .cam, then load via the normal path.
    auto world_json = camp->cam;
    world_json.replace_extension(".world.json");
    {
        std::ofstream f(world_json);
        if (!f) throw std::runtime_error("cannot write " + world_json.string());
        f << json;
    }
    load_world_json(world_json);

    // Persist the last theater + campaign so the next launch pre-selects them.
    impl_->settings.last_theater_key = theater_key;
    impl_->settings.last_campaign_stem = campaign_stem;
    save_settings(impl_->settings);
}

void ViewerApp::open_hex_inspector_with_file(const std::filesystem::path& path) {
    impl_->hex_inspector.open();
    impl_->hex_inspector.load_file(path);
}

// ---------------------------------------------------------------------------
// Diagnostics helpers (forward declarations — defined below)
// ---------------------------------------------------------------------------

namespace {

/// Build a comprehensive diagnostic report for the current install.
/// Used by Tools > Install Diagnostics and by --diagnostics CLI flag.
std::string build_install_diagnostics(const f4::install::Installation& inst);

/// Build a detailed error report for a failed campaign load.
std::string build_campaign_load_error(const f4::install::Installation& inst,
                                       const std::string& theater_key,
                                       const std::string& campaign_stem,
                                       const std::string& exception_msg);

} // namespace

std::string ViewerApp::install_diagnostics_text() const {
    if (!impl_->install) {
        return "No install set. Use File > Set Install Path... to configure one.\n";
    }
    return build_install_diagnostics(*impl_->install);
}

void ViewerApp::open_install_diagnostics() {
    if (!impl_->install) {
        impl_->install_diagnostics_text =
            "No install set.\n\nUse File > Set Install Path... to pick your "
            "Falcon 4.0 install directory first.";
    } else {
        impl_->install_diagnostics_text = build_install_diagnostics(*impl_->install);
    }
    impl_->install_diagnostics_open = true;
}

// ---------------------------------------------------------------------------
// Diagnostics helpers (definitions)
// ---------------------------------------------------------------------------

namespace {

/// Build a comprehensive diagnostic report for the current install.
/// Used by Tools > Install Diagnostics and by --diagnostics CLI flag.
std::string build_install_diagnostics(const f4::install::Installation& inst) {
    std::ostringstream ss;
    ss << "=== Install Diagnostics ===\n\n";
    ss << "Root: " << inst.root().string() << "\n";
    ss << "Valid: " << (inst.valid() ? "yes" : "no") << "\n\n";

    ss << "--- FALCON4.ct (class table) ---\n";
    if (inst.class_table().empty()) {
        ss << "NOT FOUND. Searched these locations:\n";
        for (const auto& p : inst.diagnostics().class_table_searched) {
            ss << "  " << p.string() << "\n";
        }
        ss << "\nTo fix: place FALCON4.ct in one of the above locations.\n"
           << "The class table maps entity_type values (100+) to ObjectiveType\n"
           << "and unit subtypes. Without it, objectives render as generic\n"
           << "circles instead of type-specific icons.\n";
    } else {
        ss << "Found: " << inst.class_table().string() << "\n";
    }
    ss << "\n";

    ss << "--- theater.lst ---\n";
    if (inst.diagnostics().theater_lst_path.empty()) {
        ss << "Not found in terrdata/. Fell back to directory scan.\n";
    } else {
        ss << "Path: " << inst.diagnostics().theater_lst_path.string() << "\n";
        ss << "Parsed: " << (inst.diagnostics().theater_lst_parsed ? "yes" : "no") << "\n";
        ss << "Keys: " << inst.diagnostics().theater_lst_key_count << "\n";
    }
    ss << "\n";

    ss << "--- Theaters (" << inst.theaters().size() << ") ---\n";
    for (const auto& t : inst.theaters()) {
        ss << "  " << t.display_name << " (" << t.key << ")";
        ss << (t.complete() ? "" : " [INCOMPLETE]");
        ss << "\n";
        ss << "    dir: " << t.dir.string() << "\n";
        ss << "    THEATER.MAP: " << (t.theater_map.empty() ? "MISSING" : "present") << "\n";
        ss << "    THEATER.MEA: " << (t.theater_mea.empty() ? "MISSING" : "present") << "\n";
        ss << "    THEATER.O2:  " << (t.theater_o2.empty() ? "(absent)" : "present") << "\n";
        ss << "    theater.ini: " << (t.theater_ini.empty() ? "(absent)" : "present") << "\n";
        if (!t.theater_files.empty()) {
            ss << "    All THEATER.* files (" << t.theater_files.size() << "):\n";
            for (const auto& f : t.theater_files) {
                ss << "      " << f.filename().string()
                   << "  (" << std::filesystem::file_size(f) << " bytes)\n";
            }
        }
        ss << "\n";
    }

    ss << "--- Theater dirs probed but rejected (no THEATER.MAP) ---\n";
    if (inst.diagnostics().theater_dirs_probed.empty()) {
        ss << "  (none — no subdirs found in terrdata/, or terrdata/ itself missing)\n";
    } else {
        // Show dirs that aren't in theaters() (rejected).
        for (const auto& dir : inst.diagnostics().theater_dirs_probed) {
            bool is_a_theater = false;
            for (const auto& t : inst.theaters()) {
                if (t.dir == dir) { is_a_theater = true; break; }
            }
            if (!is_a_theater) {
                ss << "  " << dir.string() << "\n";
            }
        }
    }
    ss << "\n";

    ss << "--- Campaigns (" << inst.campaigns().size() << ") ---\n";
    ss << "Campaign dir: ";
    if (inst.campaign_dir().empty()) {
        ss << "NOT FOUND\n";
    } else {
        ss << inst.campaign_dir().string() << "\n";
    }
    for (const auto& c : inst.campaigns()) {
        ss << "  " << c.stem;
        if (!c.theater_key.empty()) ss << "  [" << c.theater_key << "]";
        else ss << "  [flat layout]";
        ss << "\n";
        ss << "    path: " << c.cam.string() << "\n";
        ss << "    exists: " << (std::filesystem::exists(c.cam) ? "yes" : "NO") << "\n";
        if (std::filesystem::exists(c.cam)) {
            std::error_code ec;
            const auto sz = std::filesystem::file_size(c.cam, ec);
            if (!ec) ss << "    size: " << sz << " bytes\n";
        }
    }
    ss << "\n";

    ss << "--- Other paths ---\n";
    ss << "sim/ (aircraft): ";
    ss << (inst.aircraft_dir().empty() ? "(absent)" : inst.aircraft_dir().string());
    ss << "\n";
    ss << "terrdata/: ";
    ss << (inst.terrdata_dir().empty() ? "(absent)" : inst.terrdata_dir().string());
    ss << "\n";

    return ss.str();
}

/// Build a detailed error report for a failed campaign load. Used by
/// the campaign-load error modal to show the user the full context
/// (theater info, .cam file info, class table info) alongside the
/// exception message — so they can diagnose the failure without
/// having to open the diagnostics panel separately.
std::string build_campaign_load_error(const f4::install::Installation& inst,
                                       const std::string& theater_key,
                                       const std::string& campaign_stem,
                                       const std::string& exception_msg) {
    std::ostringstream ss;
    ss << "Campaign load failed.\n\n";
    ss << "Error: " << exception_msg << "\n\n";
    ss << "--- Context ---\n";
    ss << "Theater key: " << theater_key << "\n";
    ss << "Campaign stem: " << campaign_stem << "\n\n";

    const auto* theater = inst.find_theater(theater_key);
    if (!theater) {
        ss << "Theater '" << theater_key << "' not found in install.\n";
        ss << "Available theaters:\n";
        for (const auto& t : inst.theaters()) {
            ss << "  - " << t.key << " (" << t.display_name << ")\n";
        }
    } else {
        ss << "Theater: " << theater->display_name << " (" << theater->key << ")\n";
        ss << "  dir: " << theater->dir.string() << "\n";
        ss << "  complete: " << (theater->complete() ? "yes" : "NO") << "\n";
        ss << "  THEATER.MAP: " << (theater->theater_map.empty() ? "MISSING" : "present") << "\n";
        ss << "  THEATER.MEA: " << (theater->theater_mea.empty() ? "MISSING" : "present") << "\n";
    }
    ss << "\n";

    // Find the campaign.
    auto camps = inst.campaigns_for(theater_key);
    const f4::install::Campaign* camp = nullptr;
    for (const auto& c : camps) {
        if (c.stem == campaign_stem) { camp = &c; break; }
    }
    if (!camp) {
        ss << "Campaign '" << campaign_stem << "' not found.\n";
        ss << "Available campaigns for theater '" << theater_key << "':\n";
        if (camps.empty()) {
            ss << "  (none)\n";
        } else {
            for (const auto& c : camps) {
                ss << "  - " << c.stem << "  →  " << c.cam.string() << "\n";
            }
        }
    } else {
        ss << "Campaign file: " << camp->cam.string() << "\n";
        ss << "  exists: " << (std::filesystem::exists(camp->cam) ? "yes" : "NO") << "\n";
        if (std::filesystem::exists(camp->cam)) {
            std::error_code ec;
            const auto sz = std::filesystem::file_size(camp->cam, ec);
            if (!ec) ss << "  size: " << sz << " bytes\n";
        }
    }
    ss << "\n";

    ss << "Class table: ";
    if (inst.class_table().empty()) {
        ss << "NOT FOUND (objectives will lack icons, but campaign should still load)\n";
    } else {
        ss << inst.class_table().string() << "\n";
    }
    ss << "\n";

    ss << "Use Tools > Install Diagnostics for the full diagnostic report.\n";
    return ss.str();
}

} // namespace

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

    // --- Routes (road/rail network from objective link_data) ---
    // Draw thin lines between connected objectives. Roads are tan/brown,
    // rail links are dark gray. This is the ground movement network that
    // ground units (battalions/brigades) use to navigate.
    if (impl_->show_routes && impl_->world_loaded) {
        const Color road_color = {180, 160, 120, 140};
        const Color rail_color = {100, 100, 110, 160};
        const Color sel_color  = {255, 255, 100, 220};
        for (int i = 0; i < static_cast<int>(impl_->world.objectives.size()); ++i) {
            const auto& o = impl_->world.objectives[i];
            const Vector2 p = impl_->world_to_screen(o.x, o.y);
            for (const auto& link : o.links) {
                // Resolve neighbor VU_ID → objective index → position
                auto it = impl_->obj_id_to_index.find(link.neighbor_num);
                if (it == impl_->obj_id_to_index.end()) continue;
                const auto& n = impl_->world.objectives[it->second];
                const Vector2 q = impl_->world_to_screen(n.x, n.y);
                // Draw each link once (only when i < neighbor_index to avoid
                // drawing every road twice).
                if (i >= it->second) continue;
                const Color c = link.is_rail ? rail_color : road_color;
                DrawLineEx(p, q, 1.0f, c);
                // Highlight links from the selected objective.
                if (impl_->sel_kind == Impl::SelectionKind::Objective &&
                    impl_->sel_index == i) {
                    DrawLineEx(p, q, 2.0f, sel_color);
                }
            }
        }
    }

    // --- Objectives ---
    // Render with type-specific icons (airbase, bridge, city, port, radar,
    // powerplant, railroad, factory). Unknown types fall back to a small
    // circle (sized independently of priority). Icons are tinted by owner
    // color so team affiliation is visible. Priority is encoded as a thin
    // gold ring around high-priority objectives, NOT as icon size — this
    // keeps the map legible even when priority values are 1-100.
    if (impl_->show_objectives && impl_->world_loaded) {
        // Icon diameter in pixels. Scales mildly with zoom so icons stay
        // readable when zoomed in but never dominate the screen. The cap
        // keeps the fit-to-world view legible when thousands of objectives
        // share the screen.
        const float base_size = std::clamp(8.0f + impl_->cam_zoom * 0.6f,
                                           10.0f, 24.0f);
        for (int i = 0; i < static_cast<int>(impl_->world.objectives.size()); ++i) {
            const auto& o = impl_->world.objectives[i];
            const Vector2 p = impl_->world_to_screen(o.x, o.y);
            const RlColor c = color_for_owner(o.owner);
            // Use objective_type (from class table) if available; otherwise
            // fall back to the raw type (entity_type) which won't match icons.
            const int icon_idx = Impl::icon_for_objective_type(o.objective_type);
            impl_->draw_icon(icon_idx, p.x, p.y, base_size, c);
            // Priority halo: gold ring for high-priority objectives (>=40).
            // Two tiers for visual hierarchy without making icons huge.
            if (o.priority >= 40) {
                const float ring_r = base_size * 0.5f + 3.0f;
                const Color ring = (o.priority >= 70)
                    ? Color{255, 215, 0,   255}
                    : Color{255, 215, 0,   150};
                DrawCircleLines(static_cast<int>(p.x), static_cast<int>(p.y),
                                static_cast<int>(ring_r), ring);
            }
            // Outline selected.
            if (impl_->sel_kind == Impl::SelectionKind::Objective &&
                impl_->sel_index == i) {
                DrawCircleLines(static_cast<int>(p.x), static_cast<int>(p.y),
                                static_cast<int>(base_size * 0.6f + 4),
                                Color{255, 255, 0, 255});
            }
        }
    }

    // --- Units ---
    // Render with class-specific icons (square, diamond, circle, triangle)
    // tinted by owner color. Falls back to drawn shapes if icons aren't loaded.
    //   Battalion  → square icon
    //   Brigade    → diamond icon
    //   Squadron   → circle icon
    //   TaskForce  → triangle icon
    //   Flight     → hollow circle (drawn — no icon)
    //   Package    → plus/cross (drawn — no icon)
    if (impl_->show_units && impl_->world_loaded) {
        // Unit icon diameter — fixed for consistency with objective icons.
        const float s = std::clamp(6.0f + impl_->cam_zoom * 0.5f, 8.0f, 20.0f);
        for (int i = 0; i < static_cast<int>(impl_->world.units.size()); ++i) {
            const auto& u = impl_->world.units[i];
            const Vector2 p = impl_->world_to_screen(u.x, u.y);
            const RlColor c = color_for_owner(u.owner);
            const Color fill = {c.r, c.g, c.b, 220};
            // Use subtype-specific icon when available; falls back to generic
            // shape icon (square/diamond/circle/triangle) if not.
            const int icon_idx = impl_->icon_for_unit(u.unit_class, u.unit_subtype);

            if (icon_idx >= 0) {
                // Use the sprite icon.
                impl_->draw_icon(icon_idx, p.x, p.y, s, c);
            } else {
                // No icon for this class — draw a fallback shape.
                switch (u.unit_class) {
                    case f4::world::UnitClass::Flight:
                        DrawCircleLines(static_cast<int>(p.x), static_cast<int>(p.y),
                                        static_cast<int>(s * 0.5f), fill);
                        break;
                    case f4::world::UnitClass::Package:
                        DrawLineEx({p.x - s * 0.6f, p.y}, {p.x + s * 0.6f, p.y},
                                   s * 0.3f, fill);
                        DrawLineEx({p.x, p.y - s * 0.6f}, {p.x, p.y + s * 0.6f},
                                   s * 0.3f, fill);
                        break;
                    default:
                        DrawCircleV(p, s * 0.4f, fill);
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
                                static_cast<int>(s * 0.6f + 4),
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
            // --- Primary flow (install-aware) ---
            if (ImGui::MenuItem("Set Install Path...")) {
                set_install_path_dialog();
            }
            if (ImGui::MenuItem("Open Campaign...", nullptr, false,
                                 impl_->install.has_value())) {
                open_campaign_dialog();
            }
            // Show the current install path as a disabled hint so the
            // user can see at a glance whether an install is configured.
            if (impl_->install) {
                ImGui::TextDisabled("    %s",
                    impl_->install->root().string().c_str());
            } else {
                ImGui::TextDisabled("    (no install set)");
            }
            ImGui::Separator();

            // --- Advanced / dev path (legacy + manual file picking) ---
            if (ImGui::BeginMenu("Advanced")) {
                if (ImGui::MenuItem("Open World JSON...")) {
                    auto path = pick_open_file(
                        "Open World JSON",
                        "World JSON (*.world.json)|JSON (*.json)|All files (*.*)",
                        impl_->last_world_json_path);
                    if (!path.empty()) {
                        try { load_world_json(path); }
                        catch (const std::exception& e) {
                            impl_->last_error = e.what();
                        }
                    }
                }
                if (ImGui::MenuItem("Open Terrain JSON...")) {
                    auto path = pick_open_file(
                        "Open Terrain JSON",
                        "Terrain JSON (*.terrain.json)|JSON (*.json)|All files (*.*)",
                        impl_->last_terrain_json_path);
                    if (!path.empty()) {
                        try { load_terrain_json(path); }
                        catch (const std::exception& e) {
                            impl_->last_error = e.what();
                        }
                    }
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Import .cam Archive...")) {
                    auto path = pick_open_file(
                        "Import .cam",
                        "Campaign Archive (*.cam)|All files (*.*)",
                        impl_->last_world_json_path);
                    if (!path.empty()) {
                        try { import_cam_archive(path); }
                        catch (const std::exception& e) {
                            impl_->last_error = e.what();
                        }
                    }
                }
                if (ImGui::MenuItem("Import THEATER.* Binary...")) {
                    // Now that we have a real folder picker, this Just Works.
                    auto dir = pick_folder("Select THEATER.* Directory");
                    if (!dir.empty()) {
                        try { import_terrain_binary(dir); }
                        catch (const std::exception& e) {
                            impl_->last_error = e.what();
                        }
                    }
                }
                ImGui::EndMenu();
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
            ImGui::Checkbox("Routes",      &impl_->show_routes);
            ImGui::Checkbox("Objectives",  &impl_->show_objectives);
            ImGui::Checkbox("Units",       &impl_->show_units);
            ImGui::Checkbox("Grid",        &impl_->show_grid);
            ImGui::Checkbox("Legend",      &impl_->show_legend);
            ImGui::Separator();
            if (ImGui::MenuItem("Fit to World")) impl_->fit_to_world();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Tools")) {
            // Hex Inspector — opens a panel for inspecting raw bytes of
            // any file (FALCON4.ct, .cam, THEATER.*, etc.) with decoder
            // overlays. The primary RE tool.
            const bool hex_open = impl_->hex_inspector.is_open();
            if (ImGui::MenuItem("Hex Inspector...", nullptr, hex_open)) {
                if (!hex_open) impl_->hex_inspector.open();
            }
            ImGui::Separator();
            // Install Diagnostics — shows the full diagnostic report
            // (where we looked for FALCON4.ct, every theater dir probed,
            // every campaign path + exists check). The "what's actually
            // wrong with my install" tool.
            if (ImGui::MenuItem("Install Diagnostics...")) {
                open_install_diagnostics();
            }
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
        ImGui::Checkbox("Routes",      &impl_->show_routes);
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
            ImGui::TextUnformatted("Unit subtypes");
            ImGui::TextDisabled("(ground units use subtype icons)");
            ImGui::TextUnformatted("Armor  Artillery  Infantry");
            ImGui::TextUnformatted("Supply Engineer  HARTS");
            ImGui::TextDisabled("(air units use subtype icons)");
            ImGui::TextUnformatted("Fighter Bomber  Transport");
            ImGui::TextUnformatted("Helicopter  Carrier");
            ImGui::Separator();
            ImGui::TextUnformatted("Generic shapes (fallback)");
            ImGui::TextUnformatted("[] Battalion  <> Brigade");
            ImGui::TextUnformatted("o  Squadron  ^  TaskForce");
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
            ImGui::Text("Obj Type:  %d", o.objective_type);
            ImGui::Text("Entity:    %d", o.type);
            ImGui::Text("Position:   (%d, %d, %.0f ft)", o.x, o.y, o.z);
            ImGui::Text("Owner:      %d", o.owner);
            ImGui::Text("Priority:   %d", o.priority);
            ImGui::Text("Links:     %d (road/rail connections)", static_cast<int>(o.links.size()));
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
                    ImGui::Text("Supply:    %d%%", u.supply);
                    ImGui::Text("Morale:    %d%%", u.morale);
                    ImGui::Text("Fatigue:   %d%%", u.fatigue);
                    ImGui::Text("Parent:    %d", u.parent_id);
                    break;
                case f4::world::UnitClass::Brigade:
                    ImGui::Text("Supply:    %d%%", u.supply);
                    ImGui::Text("Morale:    %d%%", u.morale);
                    ImGui::Text("Fatigue:   %d%%", u.fatigue);
                    ImGui::Text("Elements:  %d", u.elements);
                    if (ImGui::TreeNode("Child battalions")) {
                        for (uint32_t eid : u.element_ids) {
                            ImGui::Text("  ID: %d", eid);
                        }
                        ImGui::TreePop();
                    }
                    break;
                case f4::world::UnitClass::Squadron:
                    ImGui::Text("Fuel:      %d lbs", u.fuel);
                    if (ImGui::TreeNode("Pilots", "Pilots (%d)", static_cast<int>(u.pilots.size()))) {
                        ImGui::Text("ID    Skill Status AA  Missions");
                        for (const auto& p : u.pilots) {
                            const char* status_str = "?";
                            switch (p.status) {
                                case 0: status_str = "OK"; break;
                                case 1: status_str = "Dead"; break;
                                case 2: status_str = "Leave"; break;
                                case 3: status_str = "Hosp"; break;
                            }
                            ImGui::Text("%-5d %-5d %-6s %-3d %d",
                                        p.pilot_id, p.skill, status_str,
                                        p.aa_kills, p.missions_flown);
                        }
                        ImGui::TreePop();
                    }
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
    //
    // BUG FIX: The popup ID in OpenPopup() MUST match the name passed to
    // BeginPopupModal(). Previously we used "FilePicker" in OpenPopup but
    // the title string in BeginPopupModal — the IDs didn't match so the
    // popup never opened. Now both use the title string as the ID.
    if (impl_->pending_dialog_open && !impl_->pending_dialog_title.empty()) {
        const char* popup_id = impl_->pending_dialog_title.c_str();
        if (!ImGui::IsPopupOpen(popup_id)) {
            ImGui::OpenPopup(popup_id);
        }
        ImGui::SetNextWindowSize(ImVec2(500, 160), ImGuiCond_FirstUseEver);
        if (ImGui::BeginPopupModal(popup_id,
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

    // --- Install summary modal (shown after Set Install Path succeeds) ---
    if (impl_->install_summary_open) {
        if (!ImGui::IsPopupOpen("Install Summary")) {
            ImGui::OpenPopup("Install Summary");
        }
        ImGui::SetNextWindowSize(ImVec2(500, 360), ImGuiCond_FirstUseEver);
        if (ImGui::BeginPopupModal("Install Summary",
                                    &impl_->install_summary_open,
                                    ImGuiWindowFlags_NoResize)) {
            ImGui::TextUnformatted(impl_->install_summary_text.c_str());
            ImGui::Separator();
            if (ImGui::Button("Open Campaign...")) {
                impl_->install_summary_open = false;
                ImGui::CloseCurrentPopup();
                open_campaign_dialog();
            }
            ImGui::SameLine();
            if (ImGui::Button("Close")) {
                impl_->install_summary_open = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    // --- Open Campaign modal (Theater + Campaign dropdowns) ---
    if (impl_->campaign_dialog_open && impl_->install) {
        if (!ImGui::IsPopupOpen("Open Campaign")) {
            ImGui::OpenPopup("Open Campaign");
        }
        ImGui::SetNextWindowSize(ImVec2(440, 220), ImGuiCond_FirstUseEver);
        if (ImGui::BeginPopupModal("Open Campaign",
                                    &impl_->campaign_dialog_open,
                                    ImGuiWindowFlags_NoResize)) {
            // --- Theater dropdown ---
            const auto& theaters = impl_->install->theaters();
            const auto* cur_theater =
                (impl_->campaign_dialog_theater_idx >= 0 &&
                 impl_->campaign_dialog_theater_idx < static_cast<int>(theaters.size()))
                    ? &theaters[impl_->campaign_dialog_theater_idx] : nullptr;
            const std::string theater_preview = cur_theater
                ? (cur_theater->display_name + " (" + cur_theater->key + ")")
                : "(none)";
            if (ImGui::BeginCombo("Theater", theater_preview.c_str())) {
                for (int i = 0; i < static_cast<int>(theaters.size()); ++i) {
                    const bool sel = (i == impl_->campaign_dialog_theater_idx);
                    const std::string label = theaters[i].display_name + " (" +
                                              theaters[i].key + ")";
                    if (ImGui::Selectable(label.c_str(), sel)) {
                        impl_->campaign_dialog_theater_idx = i;
                        // Theater changed — refresh the campaigns list.
                        impl_->campaign_dialog_campaigns =
                            impl_->install->campaigns_for(theaters[i].key);
                        impl_->campaign_dialog_campaign_idx = 0;
                    }
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            // --- Campaign dropdown (depends on selected theater) ---
            const auto& camps = impl_->campaign_dialog_campaigns;
            const std::string camp_preview =
                (impl_->campaign_dialog_campaign_idx >= 0 &&
                 impl_->campaign_dialog_campaign_idx < static_cast<int>(camps.size()))
                    ? camps[impl_->campaign_dialog_campaign_idx].display_name
                    : "(no campaigns)";
            if (ImGui::BeginCombo("Campaign", camp_preview.c_str())) {
                if (camps.empty()) {
                    ImGui::TextDisabled("No .cam saves found in this theater");
                }
                for (int i = 0; i < static_cast<int>(camps.size()); ++i) {
                    const bool sel = (i == impl_->campaign_dialog_campaign_idx);
                    if (ImGui::Selectable(camps[i].display_name.c_str(), sel)) {
                        impl_->campaign_dialog_campaign_idx = i;
                    }
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            ImGui::Separator();
            if (ImGui::Button("Load") &&
                impl_->campaign_dialog_campaign_idx >= 0 &&
                impl_->campaign_dialog_campaign_idx < static_cast<int>(camps.size())) {
                const auto& theater = theaters[impl_->campaign_dialog_theater_idx];
                const auto& camp = camps[impl_->campaign_dialog_campaign_idx];
                try {
                    load_campaign_from_install(theater.key, camp.stem);
                    impl_->campaign_dialog_open = false;
                    ImGui::CloseCurrentPopup();
                } catch (const std::exception& e) {
                    // Build a detailed error report so the user can see
                    // exactly what failed (incomplete theater? missing
                    // .cam? parse error?) without having to open the
                    // diagnostics panel separately.
                    impl_->last_error = e.what();
                    impl_->campaign_load_error_text = build_campaign_load_error(
                        *impl_->install, theater.key, camp.stem, e.what());
                    impl_->campaign_load_error_open = true;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                impl_->campaign_dialog_open = false;
                ImGui::CloseCurrentPopup();
            }

            // Helpful hint when no campaigns are present.
            if (camps.empty() && cur_theater) {
                ImGui::Separator();
                ImGui::TextWrapped(
                    "No .cam files found under campaign/%s/ or campaign/.\n"
                    "Start a new campaign in Falcon 4.0 first, then refresh.",
                    cur_theater->key.c_str());
            }
            ImGui::EndPopup();
        }
    }

    // --- Install Diagnostics modal (Tools > Install Diagnostics) ---
    if (impl_->install_diagnostics_open) {
        if (!ImGui::IsPopupOpen("Install Diagnostics")) {
            ImGui::OpenPopup("Install Diagnostics");
        }
        ImGui::SetNextWindowSize(ImVec2(700, 500), ImGuiCond_FirstUseEver);
        if (ImGui::BeginPopupModal("Install Diagnostics",
                                    &impl_->install_diagnostics_open,
                                    ImGuiWindowFlags_NoResize)) {
            // Render the diagnostic text in a scrollable, selectable
            // (copyable) read-only text box. The user can select-all +
            // copy to share the full report.
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0.10f, 0.12f, 1.0f));
            ImGui::BeginChild("diag_text", ImVec2(0, -40), true,
                               ImGuiWindowFlags_HorizontalScrollbar);
            // Use InputTextMultiline as a read-only text viewer — it
            // supports selection + copy out of the box, unlike
            // ImGui::TextUnformatted which doesn't allow selection.
            // We use a sufficiently large buffer and disable editing.
            // (The text is in impl_->install_diagnostics_text, which we
            // need to copy into a mutable buffer for InputTextMultiline.)
            static std::string diag_buf;  // static so it persists across frames
            diag_buf = impl_->install_diagnostics_text;
            diag_buf.resize(diag_buf.size() + 1, '\0');  // room for null terminator
            ImGui::InputTextMultiline("##diag_input",
                                       diag_buf.data(), diag_buf.size(),
                                       ImVec2(-1, -1),
                                       ImGuiInputTextFlags_ReadOnly |
                                       ImGuiInputTextFlags_AllowTabInput);
            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::Separator();
            if (ImGui::Button("Copy to Clipboard")) {
                ImGui::SetClipboardText(impl_->install_diagnostics_text.c_str());
            }
            ImGui::SameLine();
            if (ImGui::Button("Close")) {
                impl_->install_diagnostics_open = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    // --- Campaign Load Error modal ---
    // Shown when load_campaign_from_install throws. Shows the full error
    // message + theater/campaign/class-table context so the user can
    // diagnose the failure without having to open the diagnostics panel.
    if (impl_->campaign_load_error_open) {
        if (!ImGui::IsPopupOpen("Campaign Load Failed")) {
            ImGui::OpenPopup("Campaign Load Failed");
        }
        ImGui::SetNextWindowSize(ImVec2(600, 450), ImGuiCond_FirstUseEver);
        if (ImGui::BeginPopupModal("Campaign Load Failed",
                                    &impl_->campaign_load_error_open,
                                    ImGuiWindowFlags_NoResize)) {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.18f, 0.10f, 0.10f, 1.0f));
            ImGui::BeginChild("err_text", ImVec2(0, -40), true,
                               ImGuiWindowFlags_HorizontalScrollbar);
            // Same InputTextMultiline trick for selectability.
            static std::string err_buf;
            err_buf = impl_->campaign_load_error_text;
            err_buf.resize(err_buf.size() + 1, '\0');
            ImGui::InputTextMultiline("##err_input",
                                       err_buf.data(), err_buf.size(),
                                       ImVec2(-1, -1),
                                       ImGuiInputTextFlags_ReadOnly |
                                       ImGuiInputTextFlags_AllowTabInput);
            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::Separator();
            if (ImGui::Button("Copy to Clipboard")) {
                ImGui::SetClipboardText(impl_->campaign_load_error_text.c_str());
            }
            ImGui::SameLine();
            if (ImGui::Button("Open Install Diagnostics")) {
                impl_->campaign_load_error_open = false;
                ImGui::CloseCurrentPopup();
                open_install_diagnostics();
            }
            ImGui::SameLine();
            if (ImGui::Button("Close")) {
                impl_->campaign_load_error_open = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    // --- Hex Inspector panel (Tools > Hex Inspector) ---
    impl_->hex_inspector.draw();

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
    // Pre-fill with the last world JSON path if available — saves typing.
    if (!impl_->last_world_json_path.empty()) {
        std::string s = impl_->last_world_json_path.string();
        std::snprintf(impl_->pending_dialog_path, sizeof(impl_->pending_dialog_path),
                      "%s", s.c_str());
    } else {
        impl_->pending_dialog_path[0] = '\0';
    }
    impl_->pending_dialog_open = true;
}

} // namespace f4::viewer
