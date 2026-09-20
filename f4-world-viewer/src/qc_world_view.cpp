// f4-world-viewer/src/qc_world_view.cpp
//
// QC-WORLD: fly a Mission QC scenario as an OVERLAY on the world map.
//
// The sandbox scenario mode (scenario_player_view.cpp) takes over the
// whole render path with its own terrain + airfield + HUD — great for
// watching one aircraft up close, invisible from the world map. This
// file is the other presentation of the SAME machinery: the scenario
// Simulation still runs (deterministic, FlightRecorder trace, the QC
// gates untouched) but run()'s dispatch keeps the campaign canvas up,
// and:
//   * canvas.cpp's QC layer draws the aircraft, their routes, their
//     TRAILS, and the active runway on the world map (scenario
//     positions are absolute theater ENU feet — /1024 = canvas grid
//     units, no transform needed),
//   * clicking a QC aircraft selects it (SelectionKind::QcAircraft,
//     resolved through Impl::qc_handle into the scenario sim's world),
//   * the Inspector reads the same components it reads for session
//     aircraft, and the 3D tab's per-frame chase target follows it,
//   * the QC panel here drives pause/speed/follow/stop.
//
// Templates anchored with airbase_source take off from the REAL runway
// (derive_real_airbase resolves the objective's PHD layout at
// initialize()); hand-authored templates fly their absolute-ENU route
// as-is. Recording is forced to the Mission QC menu's
// qc/<stem>/trace.json convention, so a world run is Open-replayable
// exactly like a headless one.

#include "viewer_state.hpp"

#include <imgui.h>

#include <cmath>
#include <filesystem>
#include <stdexcept>

namespace f4::viewer {

void ViewerApp::fly_scenario_in_world(const std::filesystem::path& json_path) {
    // Stop any current run cleanly first (flushes its trace).
    stop_scenario_run();

    auto& sp = impl_->scenario_player;
    const std::string stem = json_path.stem().string();

    // Recording on, at the Mission QC menu's trace convention — the run
    // is Open-replayable from the menu the moment it ends. The trace's
    // parent dir must exist: the recorder's write throws otherwise (the
    // campaign_qc CLI creates it itself; the viewer is the other writer).
    sp.record_override = true;
    sp.record_override_path = qc_trace_path(stem);
    sp.record_override_every = 10;
    std::error_code mk_ec;
    std::filesystem::create_directories(
        sp.record_override_path.parent_path(), mk_ec);
    sp.world_overlay = true;

    try {
        load_scenario(json_path);
    } catch (const std::exception& e) {
        sp.world_overlay = false;
        sp.record_override = false;
        impl_->last_error = e.what();
        impl_->status_msg =
            "QC in world: scenario load failed — " + std::string(e.what());
        return;
    }

    // The overlay draws on the campaign canvas — a world is required.
    // Auto-load the template's own airbase_source world when the canvas
    // is empty (the anchored templates carry exactly the right file).
    if (impl_->last_world_json_path.empty()) {
        const auto& world_json = sp.scenario.airbase_source.world_json_path;
        bool loaded = false;
        if (!world_json.empty()) {
            try {
                load_world_json(world_json);
                loaded = true;
            } catch (const std::exception& e) {
                impl_->status_msg = "QC in world: world auto-load failed — " +
                                    std::string(e.what());
            }
        }
        if (!loaded) {
            stop_scenario_run();
            if (impl_->status_msg.find("QC in world") != 0) {
                impl_->status_msg =
                    "QC in world: load a world first (File > Open "
                    "Campaign / a world JSON) — the template carries no "
                    "airbase_source world to auto-load.";
            }
            return;
        }
    }

    // Aim the map camera at the (derived) runway threshold — and mark
    // the camera as caller-set so run()'s one-time fit-to-world doesn't
    // override the aim (the same contract set_initial_camera honors).
    impl_->cam_x = static_cast<float>(
        sp.scenario.airfield.threshold_position.x / 1024.0);
    impl_->cam_y = static_cast<float>(
        sp.scenario.airfield.threshold_position.y / 1024.0);
    impl_->cam_zoom = std::max(impl_->cam_zoom, 8.0f);
    impl_->initial_camera_set = true;
    impl_->last_error.clear();  // drop stale world-load noise (e.g. the
                                // world JSON's own terrain auto-attempt)

    // Wheels rolling.
    sp.paused = false;
    impl_->status_msg = "QC in world: " + sp.scenario.name +
                        " is flying from its real runway — watch the map, "
                        "click an aircraft to inspect + chase in 3D.";
}

void ViewerApp::stop_scenario_run() {
    auto& sp = impl_->scenario_player;
    if (!sp.active()) return;

    // Flush the trace FIRST (Open replay consumes it; the headless QC
    // loop's artifact set). A failed write must never block the stop —
    // report and move on.
    if (sp.sim) {
        try {
            sp.sim->write_recording();
        } catch (const std::exception& e) {
            impl_->last_error = std::string("Trace write failed: ") +
                                e.what();
        }
    }

    // Overlay runs never built the sandbox's GL resources
    // (sp_ensure_gl_resources_built only runs on the sandbox render
    // path), so teardown is CPU-side: drop the sim, freshen the bus
    // observers (they attached to the dying sim's bus), clear the
    // overlay-only state. The sandbox mode never calls this — it ends
    // at the run() epilogue, which unloads its GL resources in order.
    sp.sim.reset();
    sp.sim_initialized = false;
    sp.world_overlay = false;
    sp.paused = true;
    sp.sim_accumulator = 0.0;
    sp.first_frame = true;
    sp.radio_log = RadioLog{};
    sp.combat_log = f4::simulation::CombatTranscript{};
    sp.missile_trails.clear();

    impl_->qc_trails.clear();
    impl_->qc_follow_selected = false;
    if (impl_->sel_kind == Impl::SelectionKind::QcAircraft) {
        impl_->sel_kind = Impl::SelectionKind::None;
        impl_->sel_entity = f4::entities::EntityId{};
    }
    impl_->status_msg = "QC world run stopped — trace written.";
}

void ViewerApp::qc_world_frame_update() {
    auto& sp = impl_->scenario_player;
    if (!sp.active() || !sp.world_overlay || !sp.sim) return;

    // Trails: one movement-gated point per aircraft per frame. The gate
    // keeps a whole mission inside the cap (a 500 ft/s cruise appends
    // ~2.5 points/s; a taxiing aircraft almost none) while still
    // covering the full flight end to end.
    for (const auto eid : sp.sim->aircraft_entities()) {
        auto h = impl_->qc_handle(eid);
        auto* tf = h.get<f4::entities::TransformComponent>();
        if (!tf) continue;
        auto& trail = impl_->qc_trails[eid.value];
        constexpr double kMinStepFt = 200.0;
        if (trail.empty() ||
            std::abs(tf->position.x - trail.back().first) > kMinStepFt ||
            std::abs(tf->position.y - trail.back().second) > kMinStepFt) {
            trail.emplace_back(tf->position.x, tf->position.y);
            if (trail.size() > Impl::kQcTrailMaxPoints) {
                trail.erase(trail.begin());
            }
        }
    }

    // 2D map follow: the camera tracks the selected QC aircraft (the
    // Inspector's 3D tab chase view keys off the selection itself).
    if (impl_->qc_follow_selected &&
        impl_->sel_kind == Impl::SelectionKind::QcAircraft &&
        impl_->sel_entity.valid()) {
        auto h = impl_->qc_handle(impl_->sel_entity);
        if (auto* tf = h.get<f4::entities::TransformComponent>()) {
            impl_->cam_x = Impl::grid_x(tf);
            impl_->cam_y = Impl::grid_y(tf);
        }
    }
}

void ViewerApp::draw_qc_world_panel() {
    auto& sp = impl_->scenario_player;
    if (!sp.active() || !sp.world_overlay) return;

    ImGui::SetNextWindowSize(ImVec2(320, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("QC Flight (world)", nullptr,
                      ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }

    ImGui::Text("Mission: %s", sp.scenario.name.c_str());
    if (sp.sim) {
        ImGui::Text("Sim time: %.0f s   (%d aircraft)",
                    sp.sim->sim_time_s(),
                    static_cast<int>(sp.sim->aircraft_entities().size()));
    }
    ImGui::Separator();

    if (ImGui::Button(sp.paused ? "Resume (Space)" : "Pause (Space)")) {
        sp.paused = !sp.paused;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130);
    float speed_f = static_cast<float>(sp.time_scale);
    if (ImGui::SliderFloat("Speed", &speed_f, 0.1f, 10.0f, "%.1fx")) {
        sp.time_scale = static_cast<double>(speed_f);
    }

    if (impl_->sel_kind == Impl::SelectionKind::QcAircraft) {
        ImGui::Checkbox("Follow on map (G)", &impl_->qc_follow_selected);
    } else {
        ImGui::TextDisabled("Follow: click an aircraft first");
    }
    // Center the map camera on the QC flight and select it — the one-click
    // "where is it / what is it" affordance (the flight moves; hunting a
    // moving symbol across the map by hand is no fun).
    if (ImGui::Button("Center on flight") && sp.sim) {
        for (const auto eid : sp.sim->aircraft_entities()) {
            auto h = impl_->qc_handle(eid);
            auto* tf = h.get<f4::entities::TransformComponent>();
            if (!tf) continue;
            impl_->sel_kind = Impl::SelectionKind::QcAircraft;
            impl_->sel_entity = eid;
            impl_->cam_x = Impl::grid_x(tf);
            impl_->cam_y = Impl::grid_y(tf);
            impl_->cam_zoom = std::max(impl_->cam_zoom, 16.0f);
            break;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop run")) {
        stop_scenario_run();
        ImGui::End();
        return;
    }

    ImGui::Separator();
    ImGui::TextDisabled("Aircraft + trails draw on the world map; click one "
                        "to inspect it and chase it in the 3D tab.");

    ImGui::End();
}

} // namespace f4::viewer
