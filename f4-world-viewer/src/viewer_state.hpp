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
//   symbols.cpp         — procedural symbol drawing (replaces the old
//                         PNG-icon system — see symbols.hpp)
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
#include <f4/viewer/class_table_browser.hpp>
#include <f4/viewer/symbol_creator.hpp>
#include <f4/viewer/settings.hpp>
#include <f4/viewer/replay_mode.hpp>   // ReplayState (Path B2 — trace playback)

#include <f4/entities/entity.hpp>
#include <f4/install/installation.hpp>
#include <f4/terrain/terrain_data.hpp>
#include <f4/world/world_loader.hpp>

// KoreaObj model database + Falcon4.ct class table — used by the
// Ground Layout 3D panel to render real 3D feature models (buildings,
// towers, hangars, etc.) at their FeatureEntryState offsets.
//
// IMPORTANT: include these BEFORE <raylib.h>. Raylib defines `PI` as a
// preprocessor macro which would otherwise collide with any `using PI`
// declaration brought in transitively. We don't pull in f4-flight-model
// here (no flight headers in the world-viewer), but keeping f4-models
// before raylib is the safe pattern used across this codebase.
#include <f4/models/model_database.hpp>
#include <f4/models/geometry.hpp>
#include <f4/models/texture.hpp>
#include <f4/world_convert/class_table.hpp>

// f4-renderer — consolidated 3D rendering components (orbit camera,
// lit shader, mesh builder, texture cache, draw helpers, symbols,
// feature-mesh drawing).
// Replaces duplicated code that was previously inline in
// ground_layout_3d.cpp and class_table_browser.cpp.
#include <f4/renderer/orbit_camera.hpp>
#include <f4/renderer/lit_shader.hpp>
#include <f4/renderer/mesh_builder.hpp>
#include <f4/renderer/texture_cache.hpp>
#include <f4/renderer/draw_3d.hpp>
#include <f4/renderer/coord_transform.hpp>
#include <f4/renderer/feature_mesh.hpp>
#include <f4/renderer/entity_render.hpp>
#include <f4/renderer/render_resources.hpp>
#include <f4/renderer/world_renderer.hpp>
#include <f4/renderer/world_camera.hpp>

#include <raylib.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "symbols.hpp"  // Backward-compat aliases → f4::renderer::SymbolKind etc.
#include <f4/renderer/ground_layout_models.hpp>  // AirfieldGeometry3D + builder (shared)
using f4::renderer::AirfieldGeometry3D;

namespace f4::viewer {

// ---------------------------------------------------------------------------
// Color helpers — keyed by owner (Control) byte. Same scheme as the
// late f4-world-vis SVG renderer (which we deleted in favor of this viewer).
// Defined inline here because they're used by both canvas.cpp (terrain
// tile rendering, objective icon tinting, unit fill colors) and
// imgui_panels.cpp (the legend panel swatches).
// ---------------------------------------------------------------------------
// RlColor now comes from f4::renderer::RlColor via the using alias
// in symbols.hpp. The struct layout is identical:
//   struct RlColor { unsigned char r, g, b, a; };

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
// Member-function definitions that need to touch this struct (draw_symbol,
// world_to_screen, screen_to_world, fit_to_world, rebuild_objective_index)
// live in symbols.cpp and camera.cpp — declared here, defined there. The
// free functions in diagnostics.cpp take a const Installation& and don't
// need Impl access.
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
    // Phase 2 fix: File > Exit was a no-op — the menu item's comment
    // admitted it. We now set this flag and check it in run()'s loop.
    bool should_exit = false;

    // Phase 2: Objective search/filter. When non-empty, only objectives
    // whose class_name contains this substring (case-insensitive) are
    // drawn on the canvas. Empty = show all.
    char objective_search[128] = {0};
    // POLISH-2.2: cached lowercase version of objective_search. The
    // canvas loop used to allocate + lowercase a std::string needle
    // PER OBJECTIVE PER FRAME (2659 objectives × 60fps = ~160k
    // allocations/sec just for the search). Now we lowercase the
    // needle ONCE per frame (when ImGui::InputText reports a change)
    // and store it here. The canvas loop reads this directly.
    //
    // Updated by update_search_cache() — call after every InputText
    // that writes to objective_search.
    char objective_search_lower[128] = {0};
    /// Recompute objective_search_lower from objective_search.
    /// Call after any ImGui::InputText that modifies objective_search.
    /// Defined inline here (trivial).
    void update_search_cache() {
        for (int i = 0; i < 128; ++i) {
            char c = objective_search[i];
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            objective_search_lower[i] = c;
            if (c == '\0') break;
        }
    }
    // Phase 2: Team filter. 0xFF = no filter; otherwise only objectives
    // and units owned by this team are drawn (others are dimmed).
    uint8_t team_filter = 0xFF;

    // Data — EntityWorld (populated from WorldState via the ECS bridge)
    f4::entities::EntityWorld eworld;
    f4::world::PopulatedWorld pop;
    f4::terrain::TerrainData terrain;
    bool terrain_loaded = false;
    std::string theater_name;       // from WorldState.theater
    int world_version = 0;          // from WorldState.version
    std::string terrain_file_ref;   // from WorldState.terrain_file
    bool world_loaded = false;
    std::string world_path_display;
    /// Team entity IDs indexed by team slot (0..7). Built when a world
    /// is loaded so we can resolve owner slot → team entity quickly.
    std::vector<f4::entities::EntityId> team_by_slot;

    // Phase C/D: the per-kind EntityId caches from Phase B have been
    // removed. with_tag_ref() is now O(1) (Phase D added a per-tag-value
    // index to EntityWorld), so the render loops call it directly each
    // frame — zero allocation, zero copy, and no stale-cache risk.
    //
    // The convenience helpers below (campaign_entity(), teams(), objectives(),
    // units()) wrap the verbose with_tag_ref() calls so the render loops
    // stay readable. Each returns a const-ref to the internal index vector
    // (or a single EntityId for campaign), valid until the next world
    // mutation that touches that bucket.
    //
    // Campaign is a singleton (at most one entity with role="campaign"),
    // so we return the first match or a null EntityId if none exists.

    /// The campaign entity (role="campaign"), or a null EntityId if no
    /// world is loaded. O(1) tag-index lookup.
    [[nodiscard]] f4::entities::EntityId campaign_entity() const {
        const auto& ids = eworld.with_tag_ref(
            f4::entities::tags::ROLE,
            f4::entities::TagValue::from(std::string("campaign")));
        return ids.empty() ? f4::entities::EntityId{} : ids[0];
    }
    /// All team entities (role="team"), as a const-ref into the tag index.
    /// O(1) lookup. Valid until the next set_tag/destroy that touches the
    /// "team" bucket.
    [[nodiscard]] const std::vector<f4::entities::EntityId>& teams() const {
        return eworld.with_tag_ref(
            f4::entities::tags::ROLE,
            f4::entities::TagValue::from(std::string("team")));
    }
    /// All objective entities (role="objective"). O(1) lookup.
    [[nodiscard]] const std::vector<f4::entities::EntityId>& objectives() const {
        return eworld.with_tag_ref(
            f4::entities::tags::ROLE,
            f4::entities::TagValue::from(std::string("objective")));
    }
    /// All unit entities. Units have one of six ROLE values (battalion/
    /// brigade/squadron/taskforce/flight/package), so we query by
    /// OPDOMAIN instead — every unit has a domain tag (air/ground/naval/
    /// unknown), while teams and objectives don't. Returns a const-ref
    /// to a static-per-world composite vector.
    /// NOTE: unlike teams()/objectives(), this one cannot return a single
    /// const-ref into the index because units are spread across 4 domain
    /// buckets. We build a composite on each call. This is O(total_units)
    /// per call — acceptable for the render loops (called ~4x per frame),
    /// but if it becomes hot, add a "category=unit" tag in the loader.
    [[nodiscard]] std::vector<f4::entities::EntityId> units() const {
        std::vector<f4::entities::EntityId> out;
        for (const char* d : {"air", "ground", "naval", "unknown"}) {
            const auto& ids = eworld.with_tag_ref(
                f4::entities::tags::OPDOMAIN,
                f4::entities::TagValue::from(std::string(d)));
            out.insert(out.end(), ids.begin(), ids.end());
        }
        return out;
    }

    // POLISH-2.1: RenderTexture terrain cache. The naive draw loop called
    // DrawRectangleRec once per terrain cell — 128×128 = 16,384 calls per
    // frame just for terrain, which dominated the frame time on large
    // theaters. We now render the terrain into a 1024×1024 RenderTexture
    // (1 pixel per theater grid unit) ONCE when the terrain is loaded,
    // then blit it each frame with a single DrawTexturePro that the
    // world_to_screen transform positions/scales. This drops terrain
    // draw cost from ~16k ops to 1 op per frame, with zero visual
    // difference at typical zoom levels.
    //
    // The texture is grid-space (not screen-space) so it's zoom/pan
    // invariant — we never need to re-render it unless the terrain
    // data itself changes (load_terrain_json / unload).
    //
    // NOTE: The RenderTexture is allocated on the GPU via Raylib —
    // we MUST UnloadRenderTexture() before re-allocating and on
    // viewer shutdown. The destructor handles shutdown; ensure_terrain_cache()
    // handles re-allocation.
    RenderTexture2D terrain_cache = {0};
    bool terrain_cache_valid = false;  // true once terrain_cache holds the current terrain
    /// (Re)render the terrain into terrain_cache. No-op if the cache
    /// is already valid. Allocates the RenderTexture on first call,
    /// reuses it on subsequent calls (terrain_cache_valid is reset
    /// by invalidate_terrain_cache() when new terrain is loaded).
    /// Defined in canvas.cpp (next to draw_canvas which consumes it).
    void ensure_terrain_cache();
    /// Mark the terrain cache as stale. Called whenever a new terrain
    /// is loaded (file_ops.cpp::load_terrain_json) so the next
    /// draw_canvas() re-renders it.
    void invalidate_terrain_cache();

    // Selection — now uses EntityId instead of (kind, index)
    enum class SelectionKind { None, Objective, Unit };
    SelectionKind sel_kind = SelectionKind::None;
    f4::entities::EntityId sel_entity;  // valid when sel_kind != None

    // INSPECTOR-TABS-1: which tab of the combined Inspector window is
    // active. Persisted across frames so the user's choice survives
    // selection changes. ImGui's BeginTabItem returns the open/close
    // state; we write back to this field via SetTabItemInScope (in
    // draw_inspector_window) so the next frame knows which tab to draw.
    //
    // The values match the tab order in draw_inspector_window():
    //   0 = Inspect (entity detail)
    //   1 = Ground Layout (2D top-down)
    //   2 = 3D (orbit camera)
    int inspector_active_tab = 0;

    // --- ECS access helpers (inline) ---
    /// Create an EntityHandle for a given EntityId in our EntityWorld.
    f4::entities::EntityHandle handle(f4::entities::EntityId id) const {
        return f4::entities::EntityHandle(id,
            const_cast<f4::entities::EntityWorld*>(&eworld));
    }
    /// Get the grid X coordinate from a TransformComponent (feet → grid).
    static float grid_x(const f4::entities::TransformComponent* tr) {
        return tr ? static_cast<float>(tr->position.x / 1024.0) : 0.0f;
    }
    /// Get the grid Y coordinate from a TransformComponent (feet → grid).
    static float grid_y(const f4::entities::TransformComponent* tr) {
        return tr ? static_cast<float>(tr->position.y / 1024.0) : 0.0f;
    }
    /// Get objective_type from PropertyBag (0 if absent).
    static uint8_t obj_type_from_pb(const f4::entities::PropertyBag* pb) {
        if (pb) {
            auto it = pb->ints.find("objective_type");
            if (it != pb->ints.end()) return static_cast<uint8_t>(it->second);
        }
        return 0;
    }
    /// Get an int from PropertyBag, with default.
    static int64_t pb_int(const f4::entities::PropertyBag* pb,
                          const std::string& key, int64_t def = 0) {
        if (pb) {
            auto it = pb->ints.find(key);
            if (it != pb->ints.end()) return it->second;
        }
        return def;
    }
    /// Get a string from PropertyBag.
    static const std::string& pb_str(const f4::entities::PropertyBag* pb,
                                      const std::string& key) {
        if (pb) {
            auto it = pb->strings.find(key);
            if (it != pb->strings.end()) return it->second;
        }
        static const std::string empty;
        return empty;
    }
    /// Resolve a team name by owner slot index.
    const char* team_name_for_slot(uint8_t owner) const {
        if (owner < team_by_slot.size()) {
            auto h = handle(team_by_slot[owner]);
            auto* cid = h.get<f4::entities::CampaignIdentityComponent>();
            if (cid && !cid->callsign.empty()) return cid->callsign.c_str();
        }
        return "(empty)";
    }

    // Layer toggles
    bool show_terrain = true;
    bool show_objectives = true;
    bool show_units = true;
    bool show_grid = false;
    bool show_legend = true;
    // Visualization overlays — toggled off by default to reduce clutter
    // when the user just wants to see the strategic picture. Enable
    // individually to inspect specific layers.
    bool show_radar_arcs = false;             // 8-wedge detection coverage per radar objective
    bool show_ground_layout_overlay = true;   // runway/taxi/parking shapes on main canvas (zoom-gated)
    bool show_unit_destinations = true;       // thin line from unit to (dest_x, dest_y)
    bool show_waypoints = true;               // unit waypoint polyline + dots
    // When true, the 2D canvas overlays real KoreaObj 3D models for each
    // feature on the SELECTED objective (using a top-down orthographic
    // camera that matches the 2D view). Requires KoreaObj.HDR/.LOD/.TEX
    // to be discoverable under the current Installation. Shares the
    // mesh+texture cache with the 3D Ground Layout panel, so models
    // already loaded by either view are free for the other. Zoom-gated
    // via the same `cam_zoom > 4.0f` threshold as the 2D ground-layout
    // overlay so the meshes only appear when the user is zoomed in
    // enough to actually see them.
    bool show_feature_meshes = true;
    bool show_squadron_links = true;          // squadron → home airbase thin line
    bool show_hierarchy_lines = false;        // battalion → brigade parent lines (planned)
    // POLISH-2.4: minimap in the bottom-right corner of the canvas.
    // Shows the whole 1024×1024 theater at a glance: terrain thumbnail
    // (re-uses the cached terrain texture), objective dots (colored by
    // owner), unit dots (colored by owner), and a yellow rectangle
    // marking the current main-canvas viewport. Click anywhere on the
    // minimap to pan the main canvas to that location.
    bool show_minimap = true;
    // Minimap size in pixels (square). 192 keeps it readable without
    // eating too much canvas real estate. Sized for ~1080p displays;
    // adjust if the window is much smaller.
    int minimap_size = 192;

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

    // Phase 2: scratch buffers for the diagnostics + error modals.
    // Previously these were `static std::string` inside the function
    // bodies (imgui_panels.cpp:914, 953) — not thread-safe, not
    // reentrant, reallocated every frame, and the "static so it
    // persists" comment was misleading (the buffer was resized every
    // frame anyway). Moved here as proper Impl members.
    std::string diag_buf;
    std::string err_buf;

    // Hex Inspector panel — owned by the viewer, opened via Tools menu.
    HexInspector hex_inspector;

    // Class Table Browser panel — owned by the viewer, opened via Tools menu.
    ClassTableBrowser class_table_browser;

    // Symbol Creator panel — owned by the viewer, opened via Tools menu.
    // Interactive editor for the data-driven symbol library (see
    // f4/renderer/symbol_library.hpp). Lets the user build symbol
    // definitions by dragging points on a 2D canvas, then save/load
    // the resulting library to JSON. The eventual refactor of
    // symbols.cpp will consume the same library data model.
    SymbolCreator symbol_creator;

    // Scheduled screenshot (for headless smoke tests)
    bool screenshot_pending = false;
    double screenshot_at = 0.0;    // GetTime() value
    std::string screenshot_path;

    // VU_ID.num → EntityId lookups are now in pop.objective_id_map and
    // pop.unit_id_map (populated by populate_world). No separate rebuild needed.

    // --- Procedural symbols ---
    //
    // Symbol drawing is now provided by f4::renderer::draw_symbol()
    // (raylib direct) and f4::renderer::draw_symbol_imgui() (ImGui draw
    // list). Call sites in canvas.cpp use f4::renderer::draw_symbol()
    // directly; imgui_panels.cpp uses draw_symbol_imgui() via the
    // f4::viewer::draw_symbol_imgui using alias from symbols.hpp.

    // --- Camera transforms (defined in camera.cpp) ---

    /// Convert world (grid) coordinates to screen pixels.
    Vector2 world_to_screen(float gx, float gy) const;

    /// Convert screen pixels to world (grid) coordinates.
    void screen_to_world(float sx, float sy, float* gx, float* gy) const;

    /// Fit camera to show the entire theater grid (1024x1024 by default).
    void fit_to_world();

    /// Fit the main canvas to the bounding box of the selected
    /// objective's ground_layout + features. The bbox is in FEET
    /// relative to the objective center; we convert to grid units
    /// (1 grid = 1024 ft) and center the camera on the objective
    /// with a zoom that fits the bbox with a small margin. No-op
    /// if the objective has no layout or features.
    void fit_to_selection_layout();

    // -----------------------------------------------------------------------
    // Ground Layout 3D panel state
    // -----------------------------------------------------------------------
    //
    // The 3D panel renders the selected objective's airfield into an
    // offscreen RenderTexture2D using Raylib's BeginMode3D, then
    // displays the texture inside an ImGui window via rlImGuiImageSize.
    // The camera orbits the airfield center; mouse drag rotates, scroll
    // zooms (only when the ImGui window is hovered, so we don't steal
    // input from the main 2D canvas).
    //
    // The geometry is rebuilt only when the selection changes (cached
    // via ground_layout_3d_cached_entity). The RenderTexture is
    // allocated lazily on first use and freed in the ViewerApp dtor.
    RenderTexture2D ground_layout_3d_target = {0};
    bool ground_layout_3d_target_valid = false;
    int  ground_layout_3d_target_w = 0;
    int  ground_layout_3d_target_h = 0;

    // Orbit camera — f4::renderer::OrbitCamera replaces the previous
    // manual yaw/pitch/distance + Camera3D fields. Configured with
    // ground-layout-appropriate limits (MIN_DISTANCE=50, MAX_DISTANCE=50000).
    // The camera is updated via update_from_orbit() and accessed via
    // camera() for BeginMode3D.
    f4::renderer::OrbitCamera gl3d_orbit_cam{
        f4::renderer::OrbitCameraConfig{
            .min_distance     = 50.0f,
            .max_distance     = 50000.0f,
            .initial_yaw      = 34.377f,    // 0.6 rad → ~34°
            .initial_pitch    = 28.648f,    // 0.5 rad → ~29°
            .initial_distance = 4000.0f,
            .orbit_sensitivity = 0.2865f,   // 0.005 rad/px → ~0.29°/px
            .zoom_speed       = 0.1f
        }
    };
    // Airfield center in objective-local ENU feet (set when geometry is built).
    float ground_layout_3d_center_x = 0.0f;
    float ground_layout_3d_center_y = 0.0f;
    // Cached geometry + the entity it was built from. Rebuild when the
    // selected entity changes (compared by EntityId).
    AirfieldGeometry3D ground_layout_3d_geometry;
    f4::entities::EntityId ground_layout_3d_cached_entity;
    bool ground_layout_3d_show_labels = true;
    bool ground_layout_3d_show_features = true;
    bool ground_layout_3d_show_runway = true;
    bool ground_layout_3d_show_taxiways = true;
    bool ground_layout_3d_show_parking = true;
    bool ground_layout_3d_show_grid = true;
    // When true, render real KoreaObj BSP models for features (buildings,
    // towers, hangars, etc.) at their FeatureEntryState offsets, replacing
    // the flat footprint quads. Requires KoreaObj.HDR/.LOD/.TEX to be
    // discoverable under the current Installation. Falls back silently
    // to footprint rendering when models aren't loaded (the panel still
    // shows runway/taxiway/parking geometry).
    bool ground_layout_3d_show_models = true;

    // --- KoreaObj model database + class table (lazy) -------------------
    //
    // Loaded once on first use of draw_ground_layout_3d() when an
    // installation is configured. We don't load eagerly at startup
    // because:
    //   - ModelDatabase::load() is ~50-150ms for a full KoreaObj
    //   - The user may never open the 3D panel
    //   - We need the GL context for any mesh upload, and that's not
    //     available until run() calls InitWindow()
    //
    // `models_3d_load_attempted` distinguishes "haven't tried yet" from
    // "tried and failed" so we don't re-attempt every frame after a
    // failure (the failure message would otherwise pollute status_msg).
    std::optional<f4::models::ModelDatabase> model_db_3d;
    f4::world_convert::ClassTable class_table_3d;
    bool models_3d_load_attempted = false;
    bool models_3d_loaded = false;
    std::string models_3d_error;  // empty if loaded successfully

    // --- Shared GPU resources (f4::renderer::RenderResources) ------------
    //
    // Owns the mesh cache (one Raylib Mesh per unique KoreaObj
    // parent_index — features sharing a vis_type share one GPU upload),
    // the texture cache, the lit shader, the default material, lighting
    // state, and the airfield geometry cache. Shared across the 3D
    // Ground Layout panel, the 2D canvas's feature-mesh pass, and the
    // 3D world mode — a feature rendered once in any view is cached for
    // all the others.
    //
    // Must be unloaded before the GL context goes away —
    // render_res_3d.unload_all() (called from run()'s shutdown path).
    f4::renderer::RenderResources render_res_3d;

    // --- Per-frame diagnostic counters for the 3D Models path -----------
    //
    // Reset at the start of each draw_ground_layout_3d() call, updated as
    // features are walked, and displayed in the panel so the user can see
    // exactly where the pipeline is dropping features (placeholder? no
    // vis_type? empty mesh? 0 triangles?).
    int diag_3d_features_total = 0;
    int diag_3d_features_skipped_placeholder = 0;
    int diag_3d_features_no_vistype = 0;
    int diag_3d_features_no_mesh = 0;
    int diag_3d_features_drawn = 0;
    int diag_3d_meshes_drawn = 0;
    int diag_3d_triangles_drawn = 0;

    // --- Methods (defined in ground_layout_3d.cpp) ---------------------
    //
    // All require the GL context (rlImGuiSetup has been called). No-op
    // or returning false when no Installation is configured or when
    // KoreaObj files can't be found.
    /// Lazily load KoreaObj.HDR/.LOD/.TEX + Falcon4.ct from the configured
    /// Installation. Returns true on success. Idempotent — once loaded,
    /// subsequent calls return true without re-loading. After a failure,
    /// returns false and sets models_3d_error (further calls are no-ops
    /// until models_3d_load_attempted is reset).
    bool ensure_models_3d_loaded();

    // -----------------------------------------------------------------------
    // Replay mode state (Path B2 — trace playback)
    // -----------------------------------------------------------------------
    //
    // When a trace JSON is loaded via load_replay(), the viewer enters
    // replay mode: run() dispatches to handle_replay_input() +
    // draw_replay_canvas() + draw_replay_panel() instead of the normal
    // canvas path. The replay has its OWN camera (separate from the
    // campaign cam_x/cam_y/cam_zoom) because the trail lives in feet
    // (not grid units) and is self-contained (no campaign world data
    // needed).
    //
    // See replay_mode.hpp + replay_mode.cpp for the implementation.
    ReplayState replay;

    // Replay camera (feet-space, separate from campaign camera)
    float replay_cam_x = 0.0f;          // ENU feet, centered on trail
    float replay_cam_y = 0.0f;
    float replay_cam_zoom = 0.5f;        // pixels per foot
    bool replay_dragging = false;
    Vector2 replay_drag_start = {0, 0};
    float replay_drag_cam_x0 = 0.0f;
    float replay_drag_cam_y0 = 0.0f;
    /// Set by load_replay() when a trace is loaded; consumed once by
    /// run() right after InitWindow to fit the replay camera to the
    /// trail (camera fit needs window dimensions).
    bool replay_needs_fit = false;

    /// Replay camera world→screen transform (ENU feet → pixels).
    /// Inline because it's used by the per-frame draw loop in
    /// replay_mode.cpp and needs to be fast.
    [[nodiscard]] Vector2 replay_world_to_screen(double ex_ft, double ey_ft) const {
        const float cx = static_cast<float>(window_w) * 0.5f;
        const float cy = static_cast<float>(window_h) * 0.5f;
        return {
            cx + (static_cast<float>(ex_ft) - replay_cam_x) * replay_cam_zoom,
            cy - (static_cast<float>(ey_ft) - replay_cam_y) * replay_cam_zoom
        };
    }
};

} // namespace f4::viewer
