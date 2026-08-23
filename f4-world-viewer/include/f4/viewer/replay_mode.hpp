// f4-world-viewer/include/f4/viewer/replay_mode.hpp
//
// Replay mode — load a FlightRecorder trace JSON and step through it
// visually in the world viewer. This is the "playback host" promised
// by DIGI_AI_PHASE2_PLAN.md §7: a debugging tool that lets you watch
// a recorded AI flight tick-by-tick, with the trail colored by
// cross-track error and the AI brain state visible as a label.
//
// Design:
//   - The trace is loaded via FlightRecorder::load_json() (already
//     implemented + round-trip tested in f4-recorder).
//   - ReplayState tracks current_tick, paused, speed_multiplier, and
//     view toggles (trail, intended path, AI labels).
//   - The 2D canvas shows the aircraft as a heading-oriented triangle
//     at the snapshot's ENU position, the trail as a polyline colored
//     by cross_track_error_ft (green=0, red=high), and the current
//     target_position as a hollow marker.
//   - An ImGui panel shows the current snapshot's full state: tick,
//     time, callsign, AI mode/state/event, kinematics, controls.
//
// Why 2D first: the existing viewer is 2D-first and the trail + label
// view is the minimum viable debugging surface. 3D mesh replay is
// Phase 3 of DIGI_AI_PHASE2_PLAN §7.2 and depends on the BSP mesh
// pipeline; the 2D view ships first because it works for any trace
// without asset loading.
//
// Dependencies: f4-recorder, f4-geo, raylib, imgui. C++20.

#pragma once

#include <f4/recorder/flight_recorder.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace f4::viewer {

/// Replay playback state. Lives inside ViewerApp::Impl when a replay
/// is loaded. The viewer's run() loop branches on `replay_active`
/// to dispatch to draw_replay_canvas() + draw_replay_panel() instead
/// of the normal draw_canvas() + draw_imgui().
struct ReplayState {
    /// The loaded recording. std::nullopt when no replay is active.
    std::optional<f4::recorder::FlightRecorder> recording;

    /// Index into recording->snapshots() for the currently-displayed
    /// frame. Clamped to [0, size-1] after every step.
    std::size_t current_tick{0};

    /// Playback state.
    bool paused{true};            // start paused so the user can scrub
    double speed_multiplier{1.0}; // 0.25, 0.5, 1, 2, 5, 10
    /// Accumulator for real-time playback. When paused=false, the run
    /// loop adds dt*speed_multiplier each frame; on reaching 1.0 we
    /// advance current_tick by 1 and subtract 1.0.
    double tick_accumulator{0.0};

    /// The simulated dt of the recording, in seconds. Inferred from
    /// the spacing between consecutive snapshot.sim_time_s values.
    /// If only one snapshot exists, defaults to 1/60.
    double sim_dt_s{1.0 / 60.0};

    /// View toggles.
    bool show_trail{true};
    bool show_intended_path{true};
    bool show_ai_labels{true};
    bool show_target_marker{true};
    bool show_kinematics_hud{true};

    /// Which entity to focus on (read out in the panel + used as the
    /// trail source). 0 = first entity in the recording. If the
    /// recording has multiple entities, the panel lets the user cycle.
    std::size_t focused_entity_index{0};

    /// Cached unique entity IDs in first-appearance order. Built by
    /// load_replay() so the focus picker + per-entity trail lookup
    /// are O(1).
    std::vector<std::uint64_t> entity_ids;

    /// True when a replay is loaded and the viewer should dispatch
    /// to the replay render path instead of the normal canvas.
    [[nodiscard]] bool active() const noexcept {
        return recording.has_value() && !recording->empty();
    }

    /// The snapshot at current_tick, or nullptr if no replay is loaded.
    [[nodiscard]] const f4::recorder::FlightSnapshot* current_snapshot() const noexcept;

    /// The entity_id of the focused aircraft, or 0 if none.
    [[nodiscard]] std::uint64_t focused_entity_id() const noexcept;

    /// All snapshots for the focused entity (in tick order). Used by
    /// the trail polyline. Returns an empty vector if no replay.
    [[nodiscard]] std::vector<f4::recorder::FlightSnapshot>
    focused_entity_snapshots() const;

    /// Step forward/backward by n ticks. Clamps to [0, size-1].
    void step(int n) noexcept;

    /// Jump to the first/last tick.
    void jump_to_start() noexcept;
    void jump_to_end() noexcept;

    /// Recompute sim_dt_s from the recording (median of inter-snapshot
    /// spacings). Called once by load_replay().
    void infer_sim_dt() noexcept;
};

/// Load a trace JSON into the viewer's replay state. Returns true on
/// success. On failure, sets err_out (if non-null) and returns false.
/// After a successful load, the viewer's run() loop will dispatch to
/// the replay render path until the user closes the replay (File >
/// Close Replay) or loads a different one.
///
/// Thread-safety: must be called from the main thread (the function
/// itself is pure, but the ViewerApp::Impl it mutates is owned by the
/// render loop).
bool load_replay(ReplayState& state,
                 const std::filesystem::path& trace_json,
                 std::string* err_out = nullptr);

/// Fit the replay camera (replay_cam_x/y/zoom on Impl) to the bounding
/// box of all focused-entity snapshots + target_positions. Declared
/// here so ViewerApp::run() can call it after InitWindow; defined in
/// replay_mode.cpp where it has access to the ViewerApp::Impl via the
/// viewer_state.hpp include.
///
/// NOTE: this is declared as a free function taking the camera params
/// by reference so it can be called from both run() (with Impl fields)
/// and from replay_mode.cpp's draw functions. The state must be active
/// (a recording loaded) or this is a no-op.
void fit_replay_camera(ReplayState& state,
                       float window_w, float window_h,
                       float& cam_x, float& cam_y, float& cam_zoom);

} // namespace f4::viewer
