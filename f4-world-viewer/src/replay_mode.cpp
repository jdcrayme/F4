// f4-world-viewer/src/replay_mode.cpp
//
// ReplayMode PURE-DATA implementation — ReplayState helpers + load_replay
// + fit_replay_camera. NO raylib/imgui dependency (those live in
// replay_view.cpp). This file is linked into both the viewer library
// AND the unit tests, so it must compile without a GL context.
//
// See replay_mode.hpp for the data model, replay_view.cpp for the
// ViewerApp render methods, and DIGI_AI_PHASE2_PLAN.md §7 for design.

#include <f4/viewer/replay_mode.hpp>

#include <f4/recorder/flight_recorder.hpp>
#include <f4/geo/position.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace f4::viewer {

// ---------------------------------------------------------------------------
// ReplayState helpers
// ---------------------------------------------------------------------------

const f4::recorder::FlightSnapshot*
ReplayState::current_snapshot() const noexcept {
    if (!recording || recording->empty()) return nullptr;
    const auto& snaps = recording->snapshots();
    return &snaps[std::min(current_tick, snaps.size() - 1)];
}

std::uint64_t ReplayState::focused_entity_id() const noexcept {
    if (entity_ids.empty()) return 0;
    return entity_ids[std::min(focused_entity_index, entity_ids.size() - 1)];
}

std::vector<f4::recorder::FlightSnapshot>
ReplayState::focused_entity_snapshots() const {
    std::vector<f4::recorder::FlightSnapshot> out;
    if (!recording) return out;
    const auto id = focused_entity_id();
    if (id == 0) return out;
    for (const auto& s : recording->snapshots()) {
        if (s.entity_id == id) out.push_back(s);
    }
    return out;
}

void ReplayState::step(int n) noexcept {
    if (!recording || recording->empty()) return;
    if (n > 0) {
        current_tick += static_cast<std::size_t>(n);
    } else if (n < 0) {
        const std::size_t un = static_cast<std::size_t>(-n);
        current_tick = (current_tick > un) ? (current_tick - un) : 0;
    }
    const std::size_t last = recording->snapshots().size() - 1;
    if (current_tick > last) current_tick = last;
}

void ReplayState::jump_to_start() noexcept {
    current_tick = 0;
}

void ReplayState::jump_to_end() noexcept {
    if (!recording || recording->empty()) { current_tick = 0; return; }
    current_tick = recording->snapshots().size() - 1;
}

void ReplayState::infer_sim_dt() noexcept {
    if (!recording || recording->size() < 2) {
        sim_dt_s = 1.0 / 60.0;
        return;
    }
    // Use the focused entity's snapshots (so we don't mix entities).
    auto focused = focused_entity_snapshots();
    if (focused.size() < 2) {
        // Fallback: use the global snapshots array (mixed entities).
        const auto& snaps = recording->snapshots();
        sim_dt_s = snaps[1].sim_time_s - snaps[0].sim_time_s;
        if (sim_dt_s <= 0.0) sim_dt_s = 1.0 / 60.0;
        return;
    }
    // Median of inter-snapshot spacings (robust to gaps/jumps).
    std::vector<double> dts;
    dts.reserve(focused.size() - 1);
    for (std::size_t i = 1; i < focused.size(); ++i) {
        const double dt = focused[i].sim_time_s - focused[i-1].sim_time_s;
        if (dt > 0.0) dts.push_back(dt);
    }
    if (dts.empty()) { sim_dt_s = 1.0 / 60.0; return; }
    std::sort(dts.begin(), dts.end());
    sim_dt_s = dts[dts.size() / 2];
}

// ---------------------------------------------------------------------------
// load_replay (free function)
// ---------------------------------------------------------------------------

bool load_replay(ReplayState& state,
                 const std::filesystem::path& trace_json,
                 std::string* err_out) {
    try {
        if (!std::filesystem::exists(trace_json)) {
            if (err_out) *err_out = "file does not exist: " + trace_json.string();
            return false;
        }
        auto rec = f4::recorder::FlightRecorder::load_json(trace_json);
        if (rec.empty()) {
            if (err_out) *err_out = "trace is empty: " + trace_json.string();
            return false;
        }
        state.recording = std::move(rec);
        state.current_tick = 0;
        state.paused = true;
        state.speed_multiplier = 1.0;
        state.tick_accumulator = 0.0;
        state.focused_entity_index = 0;

        // Build unique entity IDs in first-appearance order.
        state.entity_ids.clear();
        for (const auto& s : state.recording->snapshots()) {
            if (std::find(state.entity_ids.begin(),
                          state.entity_ids.end(),
                          s.entity_id) == state.entity_ids.end()) {
                state.entity_ids.push_back(s.entity_id);
            }
        }
        state.infer_sim_dt();
        return true;
    } catch (const std::exception& e) {
        if (err_out) *err_out = std::string("load failed: ") + e.what();
        return false;
    }
}

// ---------------------------------------------------------------------------
// fit_replay_camera — bbox fit (pure data, no raylib)
// ---------------------------------------------------------------------------

// Compute the bounding box of all focused-entity snapshots + the
// target_positions, then set replay_cam_x/y/zoom to fit.
// Public free function (declared in replay_mode.hpp) — callable from
// ViewerApp::run() after InitWindow so the first frame shows the whole
// trail without the user having to press F.
void fit_replay_camera(ReplayState& state,
                       float window_w, float window_h,
                       float& cam_x, float& cam_y, float& cam_zoom) {
    if (!state.active()) return;
    auto focused = state.focused_entity_snapshots();
    if (focused.empty()) return;

    double min_x = 1e30, min_y = 1e30, max_x = -1e30, max_y = -1e30;
    bool any = false;
    for (const auto& s : focused) {
        min_x = std::min(min_x, s.position.x);
        min_y = std::min(min_y, s.position.y);
        max_x = std::max(max_x, s.position.x);
        max_y = std::max(max_y, s.position.y);
        any = true;
        if (s.target_position.x != 0.0 || s.target_position.y != 0.0) {
            min_x = std::min(min_x, s.target_position.x);
            min_y = std::min(min_y, s.target_position.y);
            max_x = std::max(max_x, s.target_position.x);
            max_y = std::max(max_y, s.target_position.y);
        }
    }
    if (!any) return;
    const double margin = std::max(50.0, (max_x - min_x) * 0.10);
    min_x -= margin; min_y -= margin;
    max_x += margin; max_y += margin;
    const double w = max_x - min_x;
    const double h = max_y - min_y;
    cam_x = static_cast<float>((min_x + max_x) * 0.5);
    cam_y = static_cast<float>((min_y + max_y) * 0.5);
    const float zoom_x = (w > 1.0) ? window_w / static_cast<float>(w) : 1.0f;
    const float zoom_y = (h > 1.0) ? window_h / static_cast<float>(h) : 1.0f;
    cam_zoom = std::min(zoom_x, zoom_y) * 0.9f;
    cam_zoom = std::clamp(cam_zoom, 1e-4f, 10.0f);
}

} // namespace f4::viewer
