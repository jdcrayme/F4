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
// See Docs/SCENARIO_PLAYER_PLAN.md for the full plan.

#pragma once

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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace f4::scenario_player
