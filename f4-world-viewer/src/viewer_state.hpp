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
// 2D map symbols are now authored as SVGs in the repo-root symbols/
// directory; f4::renderer::SymbolDirectory (a member of Impl) loads them
// lazily, and the canvas + inspector render through
// f4::renderer::RenderEntityIcon() / SymbolDirectory::draw(). The old
// procedural symbols.cpp (~850 LoC vocabulary) was deleted in the
// SYMBOL-SVG-2 follow-up; see f4/renderer/symbol_library.hpp.
//
// Cross-file internal helpers (the diagnostics builders) live in
// diagnostics.hpp alongside this header.

#pragma once

#include <f4/viewer/viewer_app.hpp>
#include <f4/viewer/hex_inspector.hpp>
#include <f4/viewer/class_table_browser.hpp>
#include <f4/viewer/settings.hpp>
#include <f4/viewer/replay_mode.hpp>   // ReplayState (Path B2 — trace playback)

#include <f4/entities/entity.hpp>
#include <f4/install/installation.hpp>
#include <f4/terrain/terrain_data.hpp>
#include <f4/world/world_loader.hpp>
#include <f4/simulation/campaign_session.hpp>  // V-CAMP: the live loop
#include <f4/simulation/campaign_session_runner.hpp>  // V-THREAD: the campaign thread
#include <f4/simulation/bubble_manager.hpp>  // V-3DLIVE: vehicle roster

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
#include <f4/renderer/terrain_mesh.hpp>      // TerrainMesh (Path B1)
#include <f4/renderer/terrain_chunks.hpp>    // TerrainChunkSet (Path B1 chunked)
#include <f4/renderer/world_view.hpp>        // WorldView (textured theater path)

#if defined(_WIN32)
// On Windows, raylib's CloseWindow/ShowCursor clash with Win32's
// winuser.h (C2733: "you cannot overload a function with extern C
// linkage"). If a translation unit includes <windows.h> before this
// header (directly or transitively) without NOUSER, the build breaks.
// Defining NOUSER here ensures that even if a downstream .cpp includes
// <windows.h> without the guard, the USER-decl API set (CloseWindow,
// ShowCursor, etc.) stays out of scope. Harmless on non-Windows.
#ifndef NOGDI
#define NOGDI
#endif
#ifndef NOUSER
#define NOUSER
#endif
#endif

#include <raylib.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <atomic>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <f4/renderer/ground_layout_models.hpp>  // AirfieldGeometry3D + builder (shared)
#include <f4/renderer/symbol_library.hpp>        // SymbolDirectory (lazy SVG symbols)
using f4::renderer::AirfieldGeometry3D;
using f4::renderer::RlColor;

namespace f4::viewer {

// ---------------------------------------------------------------------------
// Symbols directory resolution — where the lazy SymbolDirectory looks
// for <key>.svg files. The build copies symbols/ next to the executable;
// running from the repo root also works. F4_SYMBOLS_DIR overrides
// everything (used by headless runs/tests). Defined in viewer_app.cpp.
// ---------------------------------------------------------------------------
std::filesystem::path resolve_symbols_dir();

// ---------------------------------------------------------------------------
// Color helpers — keyed by owner (Control) byte. Same scheme as the
// late f4-world-vis SVG renderer (which we deleted in favor of this viewer).
// Defined inline here because they're used by both canvas.cpp (terrain
// tile rendering, objective icon tinting, unit fill colors) and
// imgui_panels.cpp (the legend panel swatches).
// ---------------------------------------------------------------------------
// RlColor is f4::renderer::RlColor (via symbol_library.hpp):
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

// ---------------------------------------------------------------------------
// Two-tone team palettes — the icon paint source.
// ---------------------------------------------------------------------------
// Every team gets TWO colors (the user's two-tone design): a PRIMARY that
// paints the icon's background/frame fill and a SECONDARY that paints the
// glyph strokes and contrast outlines. SVG icons are authored as
// black-on-white placeholders and the importer maps white -> Fill (primary)
// and black -> Outline (secondary), so BOTH icon colors are runtime-
// substituted per team — e.g. the enemy team renders red icons with
// dark-red glyphs where the SVG had white and black.
//
// The pairs are hand-tuned per team (hue kept from color_for_owner, the
// secondary a distinctly darker shade) rather than derived by a fixed
// multiply — a 0.4x multiply turns the yellow team's fill into mud, and
// glyphs need more contrast than a linear scale gives.
//
// color_for_owner() above stays the single-color source for destination
// lines, waypoint dots, selection rings and the legend swatches.
struct TeamPalette {
    RlColor primary;    // icon background / frame fill
    RlColor secondary;  // glyph strokes / contrast outline
};

inline TeamPalette team_palette_for_owner(uint8_t owner) {
    switch (owner) {
        case 0:  return {{172, 172, 172, 255}, { 76,  76,  76, 255}}; // neutral
        case 1:  return {{210,  65,  60, 255}, {118,  22,  20, 255}}; // enemy — red / dark red
        case 2:  return {{ 74, 134, 216, 255}, { 28,  58, 122, 255}}; // friendly — blue / navy
        case 3:  return {{ 88, 182,  88, 255}, { 32,  96,  34, 255}}; // ROK — green / forest
        case 4:  return {{228, 206,  88, 255}, {126, 101,  26, 255}}; // Japan — yellow / olive
        case 5:  return {{226, 130,  62, 255}, {132,  58,  20, 255}}; // DPRK — orange / rust
        case 6:  return {{184,  86, 184, 255}, {102,  30, 102, 255}}; // PRC — magenta / plum
        default: return {{200, 200, 200, 255}, { 96,  96,  96, 255}};
    }
}

/// Scale both palette entries by `f` (the team/mission filter's 0.3 dim,
/// applied to the pair so filtered icons fade as one).
inline TeamPalette dimmed_palette(TeamPalette p, float f) {
    p.primary.r = static_cast<unsigned char>(p.primary.r * f);
    p.primary.g = static_cast<unsigned char>(p.primary.g * f);
    p.primary.b = static_cast<unsigned char>(p.primary.b * f);
    p.primary.a = static_cast<unsigned char>(p.primary.a * f);
    p.secondary.r = static_cast<unsigned char>(p.secondary.r * f);
    p.secondary.g = static_cast<unsigned char>(p.secondary.g * f);
    p.secondary.b = static_cast<unsigned char>(p.secondary.b * f);
    p.secondary.a = static_cast<unsigned char>(p.secondary.a * f);
    return p;
}

inline RlColor to_rl(const f4::terrain::Color4& c) {
    return {c.r, c.g, c.b, c.a};
}

// V-CAMP: the campaign session's speed presets (shared by run()'s
// advance call and the Campaign window's radio buttons). Scales apply
// to WALL-CLOCK time; the session's sim tick is fixed (the "Fix Your
// Timestep" contract — see viewer_app.cpp's advance block). 240x is
// the practical ceiling: the session's per-frame tick cap (240) × 60
// FPS. At 60x a 30-minute tasking cycle passes in 30 real seconds; at
// 240x a full day passes in 6 minutes.
inline constexpr float kSessionSpeedTable[] = {1.0f, 10.0f, 60.0f, 240.0f};
inline constexpr const char* kSessionSpeedNames[] = {"1x", "10x", "60x",
                                                     "240x"};
inline constexpr int kSessionSpeedCount =
    static_cast<int>(sizeof(kSessionSpeedTable) /
                     sizeof(kSessionSpeedTable[0]));

// ---------------------------------------------------------------------------
// ViewerApp::Impl — all state the render loop touches, in one struct.
//
// The fields are grouped by concern (window/camera, data, selection, layer
// toggles, status, install-aware state, modals, hex inspector, screenshots)
// so a reader can find what they need without scanning the whole struct.
//
// Member-function definitions that need to touch this struct
// (world_to_screen, screen_to_world, fit_to_world,
// rebuild_objective_index) live in camera.cpp and canvas.cpp — declared
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
    // Phase 2 fix: File > Exit was a no-op — the menu item's comment
    // admitted it. We now set this flag and check it in run()'s loop.
    bool should_exit = false;
    // V-SMOKE: request_exit() is callable from ANY thread (the
    // --screenshot timeout thread) — atomic, not a plain bool.
    std::atomic<bool> exit_requested{false};

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

    /// Objectives within `radius_ft` of (east_ft, north_ft). Used by the
    /// 3D panel to render neighboring objectives (towns, power plants,
    /// etc.) alongside the selected one. O(n_objectives) per call —
    /// acceptable for the 3D panel (called once per frame when terrain
    /// + models are both on).
    [[nodiscard]] std::vector<f4::entities::EntityId>
    objectives_within_radius(float east_ft, float north_ft, float radius_ft) const {
        std::vector<f4::entities::EntityId> out;
        const float r2 = radius_ft * radius_ft;
        for (const auto& eid : objectives()) {
            auto h = handle(eid);
            auto* tf = h.get<f4::entities::TransformComponent>();
            if (!tf) continue;
            const float dx = static_cast<float>(tf->position.x) - east_ft;
            const float dy = static_cast<float>(tf->position.y) - north_ft;
            if (dx * dx + dy * dy <= r2) out.push_back(eid);
        }
        return out;
    }
    /// All unit entities. The loader tags every unit "category=unit"
    /// (tags::CATEGORY, set in populate_units), so this is a single
    /// const-ref tag-index read. The old version unioned the four
    /// OPDOMAIN buckets into a fresh vector on every call — O(total
    /// units) ~4x per frame in the render loops.
    [[nodiscard]] const std::vector<f4::entities::EntityId>& units() const {
        return eworld.with_tag_ref(
            f4::entities::tags::CATEGORY,
            f4::entities::TagValue::from(std::string("unit")));
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
    // NOTE: The texture is allocated on the GPU via Raylib — we MUST
    // UnloadTexture() before re-allocating and on viewer shutdown. The
    // destructor handles shutdown; ensure_terrain_cache() handles
    // re-allocation.
    //
    // Textured mode: when the theater binaries loaded (WorldView), the
    // cache is one far-tile thumbnail per MEA cell (L5 posts map 1:1
    // onto the grid) — real tile art instead of elevation-band colors.
    Texture2D terrain_cache = {};
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
    /// Best-effort load of the theater binaries (post levels + tile art)
    /// through WorldView whenever we know enough to find them: an install
    /// is configured and the loaded world names a theater. Non-fatal —
    /// on any failure the viewer stays on the untextured fallback.
    /// Defined in file_ops.cpp (next to the load paths that call it).
    void try_load_theater_tiles();

    // --- V-CAMP: the live campaign session --------------------------------
    //
    // The Phase-C loop (C1 ledger + C2 one-pool tasking + C3 routed
    // generation) as ONE object the render loop drives — the full
    // campaign_qc wiring with the one-world improvement (generated
    // missions materialize INTO the sim's world and fly; see
    // f4-simulation's campaign_session.hpp). Created by the Campaign >
    // Start Session menu item (impl_->start_campaign_session in
    // campaign_session_view.cpp); destroyed by Stop (a reset is just a
    // new session).
    std::unique_ptr<f4::simulation::CampaignSession> session;

    // V-THREAD: the campaign's OWN thread. The old run() called
    // session->advance(wall_dt * speed) inline in the ImGui frame — a
    // frame's advance could legally run 240 ticks over hundreds of
    // aircraft (seconds of work inside one BeginDrawing/EndDrawing:
    // "the UI becomes unresponsive", the user's report). The runner's
    // worker advances the session in short mutex-guarded batches
    // (adaptive tick budget, ~6-12 ms per hold); the render loop takes
    // the SAME mutex for its frame read+draw scope, so every existing
    // session read (canvas layers, Campaign window, inspector, hit
    // tests through session_handle) stays consistent WITHOUT touching
    // each call site — one lock scope in run() instead.
    //
    // Declared AFTER `session`: reverse-order destruction stops +
    // joins the worker BEFORE the session dies (the runner borrows it).
    // stop_campaign_session()/run()'s exit path stop it explicitly;
    // ~Impl is the belt-and-braces second line.
    std::unique_ptr<f4::simulation::CampaignSessionRunner> session_runner;

    // V-CAMP async start: CampaignSession::create() over a real install
    // world is SLOW (world-JSON parse + world population + hundreds of
    // flights + thousands of squadron parked aircraft — tens of seconds
    // on the big saves). Running it synchronously inside the ImGui
    // button handler froze the whole window ("not responding") for the
    // whole build; the user clicked Play while frozen, the queued Space
    // unpaused the session, and the first tick crashed (the dangling
    // class table the same tranche fixed). create() is pure headless —
    // no GL, no raylib, no ImGui — so it runs on a worker thread; the
    // result rides a future (its shared state is the one rendezvous —
    // no other data races: the worker touches nothing of Impl's).
    // run() polls adopt_session_start() every frame.
    struct SessionStartResult {
        std::unique_ptr<f4::simulation::CampaignSession> session;
        std::string error;
    };
    // atomic: written by the frame thread, polled by the --screenshot
    // exit-timeout thread (it holds the countdown while a start is in
    // flight). Plain assignments/reads, sequenced by the atomicity.
    std::atomic<bool> session_starting{false};
    std::thread session_start_thread;
    std::future<SessionStartResult> session_start_future;
    /// Speed preset index into kCampaignSpeeds (campaign_session_view).
    /// 0 = paused-via-zero is NOT used — pause is session->set_paused;
    /// the presets scale the WALL-CLOCK dt fed to advance(), never the
    /// fixed sim tick (the "Fix Your Timestep" contract).
    int campaign_speed_index = 1;         // default 10x (tasking on a
                                          // 30-min cycle is visible)
    /// The Campaign window (draw_campaign_session_view).
    bool show_campaign_window = true;
    /// Canvas live layer: the session's aircraft + their routes.
    bool show_live_layer = true;
    /// Canvas: route polylines for live aircraft.
    bool show_live_routes = true;
    /// Canvas: draw flight plans (static waypoints, live routes, mission
    /// and package links) for EVERY flight. Default off — the selected
    /// flight's plan only, so the polylines don't cover the map.
    bool show_all_routes = false;
    /// Canvas: the threat-map overlay (enemy AD rings as cells).
    bool show_threat_overlay = false;
    /// True when advance() hit the tick cap last frame (time dilated —
    /// surfaced in the window so the user knows the preset outran CPU).
    /// V-THREAD: mirrored from the runner's atomic once per frame (the
    /// worker, not the frame, advances now).
    bool campaign_time_dilated = false;
    /// V-3DLIVE: the campaign camera bubble drives deaggregation (zoom
    /// into a ground unit → its vehicles/personnel appear, even while
    /// paused). Toggle in the Campaign window.
    bool campaign_view_bubble = true;
    /// V-3DLIVE: the view bubble is re-pointed only when the camera
    /// moved/zoomed beyond these thresholds — avoids per-frame deagg
    /// churn (and re-pointing while the camera sits still).
    float last_bubble_gx = -1.0e9f, last_bubble_gy = -1.0e9f;
    float last_bubble_zoom = -1.0f;
    /// Session start options (the Campaign window's start row).
    int campaign_start_team = -1;
    int campaign_start_max_flights = 48;
    /// Last session creation failure (shown in the window when set).
    char campaign_error[256] = {0};
    /// V-THREAD: the Stop button's deferred stop — set inside the
    /// frame session-lock scope (a direct runner->stop() there would
    /// self-deadlock); run() processes it right after the scope ends.
    bool session_stop_requested = false;
    /// The session instance the pending stop targets. process_session_stop
    /// drops nothing if a different session was adopted in between (the
    /// menu's Reset flow requests a stop and immediately starts a new
    /// build — if the create won the race, the fresh session survives).
    const f4::simulation::CampaignSession* session_stop_target = nullptr;
    /// V-SMOKE (--play): the adopted session starts RUNNING instead of
    /// paused. Set by the CLI (--play) BEFORE request_campaign_session;
    /// adopt_session_start honors it for both the runner and the
    /// session's own flag. Interactive starts stay paused (default).
    bool session_auto_play = false;

    // --- Selection --------------------------------------------------------
    // The sel_kind/sel_entity pair; a LiveAircraft selection stores
    // the entity id in sel_entity too, but the entity lives in the
    // SESSION's world (a different EntityWorld with its own id space —
    // id values may collide between worlds, so the kind discriminates).
    enum class SelectionKind { None, Objective, Unit, LiveAircraft };
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

    /// Force the "3D" tab selected on the next inspector draw — set by
    /// select_by_name() (the --select CLI flag) so headless screenshots
    /// capture the 3D textured-terrain view without clicking the tab.
    /// Consumed (cleared) by draw_inspector_window().
    bool inspector_force_3d_tab = false;

    // --- ECS access helpers (inline) ---
    /// Create an EntityHandle for a given EntityId in our EntityWorld.
    f4::entities::EntityHandle handle(f4::entities::EntityId id) const {
        return f4::entities::EntityHandle(id,
            const_cast<f4::entities::EntityWorld*>(&eworld));
    }
    /// Create an EntityHandle in the SESSION's world (the live campaign
    /// loop's EntityWorld — a different world from eworld, with its own
    /// id space). Only valid while a session runs.
    [[nodiscard]] f4::entities::EntityHandle
    session_handle(f4::entities::EntityId id) const {
        return f4::entities::EntityHandle(id,
            const_cast<f4::entities::EntityWorld*>(&session->sim().world()));
    }
    /// The session's live aircraft roster (empty when no session).
    [[nodiscard]] const std::vector<f4::entities::EntityId>&
    live_aircraft() const {
        static const std::vector<f4::entities::EntityId> empty;
        return session ? session->sim().aircraft_entities() : empty;
    }
    /// V-3DLIVE: the session's PARKED squadron aircraft roster (empty
    /// when no session) — the aircraft sitting on the ramps. Drawn
    /// dimmed in the 2D live layer and as models in the live 3D pass.
    [[nodiscard]] const std::vector<f4::entities::EntityId>&
    parked_aircraft() const {
        static const std::vector<f4::entities::EntityId> empty;
        return session ? session->sim().squadron_aircraft_entities()
                       : empty;
    }
    /// V-3DLIVE: the session's DEAGGREGATED vehicle roster (empty when
    /// no session / nothing deaggregated) — individual tanks, trucks,
    /// and personnel squads. Grows/shrinks as the (camera or ownship)
    /// bubble moves.
    [[nodiscard]] const std::vector<f4::entities::EntityId>&
    deaggregated_vehicles() const {
        static const std::vector<f4::entities::EntityId> empty;
        return (session && session->sim().bubble_manager())
                   ? session->sim().bubble_manager()->vehicle_entities()
                   : empty;
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
    // --- B.3 campaign-QC layers ------------------------------------------
    // The tasking picture: flights colored by owner already render via the
    // base unit pass; these overlays add the RELATIONSHIPS the campaign
    // logic actually created (target assignments, package composition,
    // the bullseye reference), and the mission filter isolates one
    // mission type for end-to-end inspection.
    bool show_mission_links = true;           // flight → mission target line
    bool show_package_links = true;           // package → element flights lines
    bool show_bullseye = true;                // campaign bullseye crosshair
    /// Mission filter: -1 = all missions. Otherwise only flights whose
    /// FlightPlanComponent::mission == mission_filter render at full
    /// strength (others dim to 25%); the ATO table and mission links
    /// follow the same filter. Shared with team_filter.
    int mission_filter = -1;
    /// True when the flight entity passes impl_->mission_filter.
    [[nodiscard]] bool mission_filter_passes(
        const f4::entities::FlightPlanComponent* fp) const {
        return mission_filter < 0 || (fp && fp->mission != 0 &&
            static_cast<int>(fp->mission) == mission_filter);
    }
    /// The "ATO / Tasking" window (draw_campaign_qc_view). ON by default —
    /// the ATO is the primary QC surface for campaign tasking.
    bool show_ato = true;
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

    // Lazy SVG symbol directory — the map icon vocabulary. Keys resolve
    // to <symbols dir>/<key>.svg, parsed on first use; missing files
    // render as the fallback square (see f4/renderer/symbol_library.hpp).
    // The directory is probed once at startup (exe-relative candidates
    // cover both the build tree and running from the repo root).
    f4::renderer::SymbolDirectory symbols{resolve_symbols_dir()};

    // Scheduled screenshot (for headless smoke tests)
    bool screenshot_pending = false;
    double screenshot_at = 0.0;    // GetTime() value
    std::string screenshot_path;

    // VU_ID.num → EntityId lookups are now in pop.objective_id_map and
    // pop.unit_id_map (populated by populate_world). No separate rebuild needed.

    // --- SVG symbols ---
    //
    // 2D map symbols are SVG files in symbols/ (one per icon key, see
    // f4::renderer::SymbolDirectory). The directory above is probed once
    // at startup (exe-relative candidates cover both the build tree and
    // running from the repo root). Call sites in canvas.cpp use
    // SymbolDirectory::draw() (raylib direct) and RenderEntityIcon()
    // (entity dispatch); imgui_panels.cpp can use draw_imgui() when a
    // legend or inspector needs to render a symbol into a draw list.

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
    // 3D terrain mesh (Path B1 — shared with scenario player)
    // -----------------------------------------------------------------------
    //
    // When terrain is loaded (load_terrain_json or import_terrain_binary),
    // we build a TerrainMesh centered on the selected objective for the
    // 3D panel. The mesh is rebuilt when the selection changes (the
    // center moves) or when the terrain changes. Passed to render_world()
    // via SceneDescription.terrain_mesh — the same path the scenario
    // player uses, so both apps share the terrain rendering code.
    f4::renderer::TerrainMesh terrain_mesh_3d;
    bool terrain_mesh_3d_built = false;
    f4::entities::EntityId terrain_mesh_3d_cached_entity;  // rebuild on selection change
    bool show_terrain_mesh_3d = true;  // toggle in the 3D panel

    // -----------------------------------------------------------------------
    // 3D terrain chunk set (Path B1 chunked — frustum-culled)
    // -----------------------------------------------------------------------
    //
    // Alternative to terrain_mesh_3d for callers that want per-chunk
    // frustum culling. When use_terrain_chunks is true (default), the
    // 3D panel builds a TerrainChunkSet instead of a single TerrainMesh.
    // Each chunk is independently frustum-culled, so for a typical
    // orbit camera ~50% of chunks are skipped, halving draw calls.
    // The chunk set also lifts the unsigned-short index cap, allowing
    // higher total vertex counts (each chunk is small).
    //
    // When use_terrain_chunks is false, the panel falls back to the
    // single TerrainMesh above (legacy path). This is kept as a toggle
    // so any regression in the chunk path can be worked around without
    // a code change.
    f4::renderer::TerrainChunkSet terrain_chunk_set_3d;
    bool terrain_chunk_set_3d_built = false;
    f4::entities::EntityId terrain_chunk_set_3d_cached_entity;
    bool use_terrain_chunks = true;   // toggle: chunk set vs single mesh

    // -----------------------------------------------------------------------
    // Textured theater — the ONE shared load-a-world path
    // -----------------------------------------------------------------------
    //
    // When a campaign is loaded from a real install (or a theater dir is
    // otherwise known), WorldView loads the raw theater binaries
    // (THEATER.L*/O* posts + TEXTURE.BIN/texture.zip/FArtILES tile art)
    // and builds the TEXTURED chunk set for the 3D panel — replacing the
    // vertex-color path above. Without theater binaries (JSON-only
    // terrain loads) the panel keeps the untextured chunk/mesh paths.
    f4::renderer::WorldView world;
    std::filesystem::path current_theater_dir;   // set by the install flow
    bool theater_tiles_loaded = false;           // world.theater_loaded()
    f4::entities::EntityId world_view_cached_entity;  // rebuild on selection change

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
