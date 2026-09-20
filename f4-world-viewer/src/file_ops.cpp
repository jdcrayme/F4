// f4-world-viewer/src/file_ops.cpp
//
// ViewerApp file-load / import operations. Binary imports (.cam
// archives, THEATER.* terrain) run through the converter CLIs
// (cam2json / terrain2json) as subprocesses — Tranche 0d: the runtime
// app no longer links the converter libraries (P2 boundary). The CLIs
// are importer-side tools; their paths are injected at configure time
// via generator expressions (F4_CAM2JSON_EXE / F4_TERRAIN2JSON_EXE).
//
// Split out of the original 1920-LoC viewer_app.cpp god-file (item #5
// of the architecture review). No behavior change — same paths, same
// FALCON4.ct auto-search, same status-message format.
//
// The four public methods here are called from:
//   - File menu (File > Advanced > Open World JSON / Open Terrain JSON /
//     Import .cam Archive / Import THEATER.* Binary)
//   - load_campaign_from_install() in install_flow.cpp (which calls
//     load_world_json after writing the converted JSON next to the .cam)

#include "viewer_state.hpp"

#include <f4/world/detail/world_state.hpp>

#include <f4/viewer/pipeline_io.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace f4::viewer {

void ViewerApp::Impl::try_load_theater_tiles() {
    // The install flow (load_campaign_from_install) does this inline with
    // status reporting; this helper covers the paths that learn the
    // theater indirectly — load_world_json, import_cam_archive (which
    // routes through load_world_json), and set_install_path.
    if (theater_tiles_loaded) return;   // already have tiles
    if (!install || theater_name.empty()) return;
    const auto* theater = install->find_theater(theater_name);
    if (!theater || !theater->complete()) return;
    current_theater_dir = theater->dir;
    try {
        theater_tiles_loaded = world.load_theater(theater->dir);
    } catch (const std::exception&) {
        // Malformed tile data shouldn't take down an otherwise-good
        // world load — fall back to untextured terrain.
        theater_tiles_loaded = false;
    }
    if (theater_tiles_loaded) {
        // Repaint the 2D map with real far-tile art next frame.
        invalidate_terrain_cache();
    }
}

void ViewerApp::load_terrain_json(const std::filesystem::path& path) {
    impl_->terrain.load_terrain_json(path);
    impl_->terrain_loaded = true;
    impl_->last_terrain_json_path = path;
    impl_->status_msg = "Loaded terrain: " + path.string();
    // POLISH-2.1: invalidate the terrain RenderTexture cache so the
    // next draw_canvas() re-renders it from the newly-loaded data.
    // (Safe to call before GL context exists — just sets a flag.)
    impl_->invalidate_terrain_cache();
    impl_->fit_to_world();
}

void ViewerApp::load_world_json(const std::filesystem::path& path) {
    // Load WorldState from JSON, then populate EntityWorld via the ECS bridge.
    f4::world::WorldState ws;
    ws.load(path);

    // V-CAMP: a live campaign session is bound to the world it was
    // created over (its WorldState, its sim world). Loading a different
    // world under a running session would leave it flying stale data —
    // stop it here (the user can start a fresh session over the new
    // world from the Campaign menu).
    if (impl_->session) {
        stop_campaign_session();
    }

    // Extract metadata before populating (we need these even if pop fails).
    impl_->theater_name = ws.theater;
    impl_->world_version = ws.version;
    impl_->terrain_file_ref = ws.terrain_file;

    // CAMP-HOST-3: capture the team slot→name table from the world the
    // viewer itself parsed — the flights table's team column reads this
    // (the contract's flights rows carry the slot; names are host data
    // from the same world file every session starts from).
    impl_->world_team_names.clear();
    for (const auto& t : ws.teams) {
        impl_->world_team_names.emplace_back(
            static_cast<std::int8_t>(t.slot), t.name);
    }

    // If the WorldState already has terrain loaded, transfer it.
    if (ws.terrain_loaded) {
        impl_->terrain = std::move(ws.terrain);
        impl_->terrain_loaded = true;
    }

    // Populate EntityWorld from WorldState via the ECS bridge.
    impl_->eworld = f4::entities::EntityWorld();  // clear previous
    impl_->pop = f4::world::populate_world(impl_->eworld, ws);

    // Phase C/D: no per-kind caches to populate. The render loops call
    // impl_->objectives() / units() / teams() / campaign_entity() directly,
    // which delegate to EntityWorld::with_tag_ref() — now O(1) thanks to
    // the per-tag-value index added in Phase D. This eliminates the
    // "snapshot on load" pattern and the stale-cache risk that came with it.

    // Build team_by_slot mapping: slot index → team EntityId.
    // This is the only per-kind lookup that still needs a pre-pass, because
    // it's a slot-indexed array (not a tag query). We walk the team entities
    // once and index them by CampaignIdentityComponent::team_id.
    impl_->team_by_slot.clear();
    impl_->team_by_slot.resize(8);  // 8 team slots max
    for (const auto& tid : impl_->teams()) {
        auto h = f4::entities::EntityHandle(tid, &impl_->eworld);
        auto* cid = h.get<f4::entities::CampaignIdentityComponent>();
        if (cid && cid->team_id >= 0 && cid->team_id < 8) {
            impl_->team_by_slot[static_cast<size_t>(cid->team_id)] = tid;
        }
    }

    impl_->world_loaded = true;
    ++impl_->world_generation;
    impl_->world_path_display = path.string();
    impl_->last_world_json_path = path;
    impl_->status_msg = "Loaded world: " + path.string() +
        "  (" + std::to_string(impl_->objectives().size()) + " objectives, " +
        std::to_string(impl_->units().size()) + " units)";

    // Try to auto-load the referenced terrain file — but only if terrain
    // isn't already loaded. This prevents a spurious "auto-load failed"
    // error when load_campaign_from_install() has already loaded terrain
    // before calling us: the world JSON's terrain_file field is relative
    // to the .cam's directory (e.g. "terrain.json"), and that path may
    // not resolve correctly from CWD.
    if (!impl_->terrain_file_ref.empty() && !impl_->terrain_loaded) {
        // Resolution ladder: the world JSON names its terrain with a bare
        // filename ("korea.terrain.json") that only resolves by accident —
        // the standard export layout puts the terrain at
        // Data/Theater/<theater>/terrain.json, NOT beside the world JSON
        // (the old single-shot join failed on every standard-layout load).
        // Try beside the world first (import layouts keep them together),
        // then the standard theater layout, then CWD-relative.
        std::vector<std::filesystem::path> candidates;
        candidates.push_back(std::filesystem::path(path.parent_path()) /
                             impl_->terrain_file_ref);
        const auto data_dir = discover_data_dir();
        if (!data_dir.empty() && !impl_->theater_name.empty()) {
            candidates.push_back(data_dir / "Theater" / impl_->theater_name /
                                 "terrain.json");
            candidates.push_back(data_dir / "Theater" / impl_->theater_name /
                                 impl_->terrain_file_ref);
        }
        candidates.push_back(std::filesystem::path(impl_->terrain_file_ref));
        bool loaded = false;
        std::string last_err;
        for (const auto& cand : candidates) {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(cand, ec)) continue;
            try {
                impl_->terrain.load_terrain_json(cand);
                impl_->terrain_loaded = true;
                impl_->last_terrain_json_path = cand;
                impl_->status_msg += "  + terrain: " + cand.string();
                loaded = true;
                break;
            } catch (const std::exception& e) {
                last_err = e.what();
            }
        }
        if (!loaded) {
            impl_->last_error = "Auto-load terrain failed: " +
                                (last_err.empty()
                                     ? std::string("no candidate path exists")
                                     : last_err);
        }
    }
    // The world names a theater ("korea") — if an install is configured,
    // pull the theater binaries (post levels + tile art) so the 2D map
    // and the 3D panel render textured terrain. No-op without an install
    // or when tiles are already loaded.
    impl_->try_load_theater_tiles();

    impl_->fit_to_world();
}

void ViewerApp::import_terrain_binary(const std::filesystem::path& terrain_dir) {
    // Tranche 0d: run the terrain2json CLI instead of linking the
    // converter library (P2 boundary). The JSON lands in Data/Temp/
    // — installs can be read-only, and Temp conversions must not
    // shadow the canonical Data/Theater exports.
    const auto data_dir = discover_data_dir();
    if (data_dir.empty() || !std::filesystem::exists(data_dir)) {
        throw std::runtime_error(
            "Data/ directory not found — cannot place the converted "
            "terrain JSON. Run from the repo or export the pipeline "
            "data with scripts/export-game-data.");
    }
    const auto out = temp_dir(data_dir) / "Theater" /
                     terrain_dir.filename().string() / "terrain.json";
    std::filesystem::create_directories(out.parent_path());

    std::string output;
    const int rc = run_converter(F4_TERRAIN2JSON_EXE,
                                 quote_arg(terrain_dir) + " " + quote_arg(out),
                                 &output);
    if (rc != 0) {
        throw std::runtime_error("terrain2json failed (exit " +
                                 std::to_string(rc) + ")" +
                                 (output.empty() ? "" : ":\n" + output));
    }
    load_terrain_json(out);
}

void ViewerApp::import_cam_archive(const std::filesystem::path& cam_path) {
    // Tranche 0d: run the cam2json CLI instead of linking the converter
    // library (P2 boundary). cam2json resolves the class table (bundled
    // fixture fallback) and joins theater names/layouts when an objects
    // dir is available — the same flow the build's korea-real-world-json
    // target uses. The world JSON lands in Data/Temp/World/ rather than
    // next to the .cam: installs can be read-only, and Temp keeps the
    // canonical Data/World exports untouched.
    const auto data_dir = discover_data_dir();
    if (data_dir.empty() || !std::filesystem::exists(data_dir)) {
        throw std::runtime_error(
            "Data/ directory not found — cannot place the converted "
            "world JSON. Run from the repo or export the pipeline data "
            "with scripts/export-game-data.");
    }
    const auto out = temp_dir(data_dir) / "World" /
                     (cam_path.stem().string() + ".world.json");
    std::filesystem::create_directories(out.parent_path());

    // Prefer a theater objects dir near the .cam for the join.
    std::filesystem::path objects_dir;
    for (const auto& cand : { cam_path.parent_path() / "terrdata" / "objects",
                              cam_path.parent_path() / "objects" }) {
        if (std::filesystem::exists(cand)) {
            objects_dir = cand;
            break;
        }
    }

    std::string args = quote_arg(cam_path) + " " + quote_arg(out);
    if (!objects_dir.empty()) {
        args += " --theater-data " + quote_arg(objects_dir);
    }
    std::string output;
    const int rc = run_converter(F4_CAM2JSON_EXE, args, &output);
    if (rc != 0) {
        throw std::runtime_error("cam2json failed (exit " +
                                 std::to_string(rc) + ")" +
                                 (output.empty() ? "" : ":\n" + output));
    }
    load_world_json(out);
}

} // namespace f4::viewer
