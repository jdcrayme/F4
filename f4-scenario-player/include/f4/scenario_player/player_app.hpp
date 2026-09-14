// f4-scenario-player/include/f4/scenario_player/player_app.hpp
//
// PlayerApp — top-level orchestrator for the f4-scenario-player host.
//
// Owns the Simulation (EntityWorld + MessageBus + ModelDatabase + tick
// loop) and the Raylib renderer. The split mirrors f4-models-viewer:
//   - f4-simulation::Simulation  (library, no rendering)
//   - f4::scenario_player::PlayerApp (executable + Raylib)
//
// Lifecycle:
//   PlayerApp app;
//   app.load_scenario("scenarios/kunsan_parking.json");
//   app.run();          // opens a window and runs the sim+render loop
//
// See Docs/archive/SCENARIO_PLAYER_PLAN.md for the full plan.

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>

namespace f4::scenario_player {

class PlayerApp {
public:
    PlayerApp();
    ~PlayerApp();

    PlayerApp(const PlayerApp&) = delete;
    PlayerApp& operator=(const PlayerApp&) = delete;
    PlayerApp(PlayerApp&&) = delete;
    PlayerApp& operator=(PlayerApp&&) = delete;

    /// Load a scenario from a JSON file. Throws on parse / asset load
    /// failure. Resolves asset paths (KoreaObj.HDR/.LOD/.TEX, f16.json,
    /// trace.json) relative to the scenario file's parent directory.
    void load_scenario(const std::filesystem::path& json_path);

    /// Set the initial window size. Must be called before run().
    void set_window_size(int width, int height) noexcept;

    /// Schedule a single screenshot after `delay_sec` (for headless
    /// smoke tests). The app exits after the screenshot is taken.
    void schedule_screenshot(float delay_sec,
                             const std::filesystem::path& path);

    /// Start the sim RUNNING (default start is paused at parking).
    /// Must be called before run().
    void set_paused(bool paused) noexcept;

    /// Sim speed multiplier. 1.0 is true real time at ANY frame rate:
    /// the value scales the WALL-CLOCK time fed into the run loop's
    /// fixed-timestep accumulator — never the per-tick dt — so every
    /// tick the sim sees is exactly scenario.sim_dt wide and the FCS
    /// filters stay at their tuned operating point. Clamped to
    /// [0.1, 10.0]. Must be called before run().
    void set_time_scale(double scale) noexcept;

    /// Camera follows the aircraft each frame (the C toggle). Before run().
    void set_follow_camera(bool follow) noexcept;

    /// Override the initial orbit distance (feet). Before run().
    void set_camera_distance(double dist_ft) noexcept;

    /// Run the render + sim loop until window close. Blocks.
    void run();

    /// Run the BVR intercept harness headlessly over the loaded scenario
    /// and write the summary JSON. Sibling to run() — used by the
    /// --harness CLI flag. Does NOT create a GL context; does NOT enter
    /// the render loop. Returns the harness's exit code (0 = all green;
    /// 1 = abort; 2 = non-combat; 3/4 = fight_alive; 5 = engagement;
    /// 6 = roster; 9 = non-deterministic — see bvr_intercept_qc.cpp's
    /// exit-code table).
    ///
    /// `summary_out` is the path of the byte-stable recorder JSON
    /// artifact (bvr_intercept_result.json). The summary JSON
    /// (bvr_intercept_summary.json — verdicts + counters + MD5) and the
    /// diary JSON (bvr_intercept_diary.json — per-sample telemetry) are
    /// written as siblings under the SAME naming bvr_intercept_qc uses,
    /// so the rendered variant produces the same artifacts as the
    /// headless tool.
    int run_harness(const std::filesystem::path& summary_out,
                    std::int64_t horizon_sec = 300,
                    double sample_sec = 30.0,
                    int runs = 2);

private:
    // The path passed to load_scenario(). run_harness builds the harness
    // options from it (the harness re-loads the scenario fresh, so the
    // player's already-built Simulation is left untouched — the harness
    // composes its own). Stored outside the Impl so the header stays
    // free of f4-simulation types (the pimpl keeps the heavy includes
    // in the .cpp).
    std::filesystem::path scenario_json_path_;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace f4::scenario_player
