// f4-scenario-player/cli/main.cpp — entry point.
//
//   f4-scenario-player <scenario.json>
//   f4-scenario-player <scenario.json> --screenshot out.png
//   f4-scenario-player <scenario.json> --run --speed 4 --shot-at 30 --screenshot out.png
//   f4-scenario-player <scenario.json> --width 1920 --height 1080
//   f4-scenario-player <scenario.json> --harness out/bvr_intercept_result.json
//   f4-scenario-player <scenario.json> --harness out/bvr_intercept_result.json \
//                                      --horizon-sec 300 --sample-sec 30 --runs 2
//
// Loads the scenario, initializes the simulation (spawns aircraft, loads
// 3D model, wires ATC), and runs the Raylib render loop. The aircraft
// starts at the parking spot in PAUSED state — press Space to begin taxi.
//
// --harness is the M4 headless acceptance path (sibling to --screenshot):
// instead of opening a window, it runs BvrInterceptHarness over the
// loaded scenario, writes the three artifacts (bvr_intercept_result.json
// + bvr_intercept_summary.json + bvr_intercept_diary.json), prints the
// verdict, and exits with the harness's exit code. Mutually exclusive
// with --screenshot (one or the other; never both).

#include <f4/scenario_player/player_app.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

int main(int argc, char** argv) {
    std::string scenario_path;
    std::string screenshot_path;
    bool exit_after_screenshot = false;
    bool start_running = false;
    bool start_follow = false;
    double speed = 1.0;
    double shot_at_sec = 1.5;
    double camera_distance = -1.0;   // <0 = scenario default
    int window_w = 1600;
    int window_h = 900;

    // --harness: the M4 headless acceptance path (sibling to --screenshot).
    // When set, the player loads the scenario, runs BvrInterceptHarness
    // headlessly (no GL context, no render loop), writes the three
    // artifacts, prints the verdict, and exits with the harness's exit
    // code. The path arg is the bvr_intercept_result.json location; the
    // summary + diary go to siblings under the standard names.
    std::string harness_summary_out;
    bool run_harness = false;
    std::int64_t harness_horizon_sec = 300;
    double harness_sample_sec = 30.0;
    int harness_runs = 2;

    // SHOWCASE-1: --record <path> forces the FlightRecorder trace on for
    // this run (the world viewer's replay mode consumes it). The path is
    // relative to the CURRENT DIRECTORY (not the scenario's dir).
    std::string record_path;
    int record_every = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--screenshot" && i + 1 < argc) {
            screenshot_path = argv[++i];
            exit_after_screenshot = true;
        } else if (a == "--harness" && i + 1 < argc) {
            harness_summary_out = argv[++i];
            run_harness = true;
        } else if (a == "--horizon-sec" && i + 1 < argc) {
            harness_horizon_sec =
                static_cast<std::int64_t>(std::atoll(argv[++i]));
        } else if (a == "--sample-sec" && i + 1 < argc) {
            harness_sample_sec = std::atof(argv[++i]);
        } else if (a == "--runs" && i + 1 < argc) {
            harness_runs = std::atoi(argv[++i]);
        } else if (a == "--run") {
            start_running = true;
        } else if (a == "--follow") {
            start_follow = true;
        } else if (a == "--speed" && i + 1 < argc) {
            speed = std::atof(argv[++i]);
        } else if (a == "--shot-at" && i + 1 < argc) {
            shot_at_sec = std::atof(argv[++i]);
        } else if (a == "--camera-distance" && i + 1 < argc) {
            camera_distance = std::atof(argv[++i]);
        } else if (a == "--record" && i + 1 < argc) {
            record_path = argv[++i];
        } else if (a == "--record-every" && i + 1 < argc) {
            record_every = std::atoi(argv[++i]);
        } else if (a == "--width" && i + 1 < argc) {
            window_w = std::atoi(argv[++i]);
        } else if (a == "--height" && i + 1 < argc) {
            window_h = std::atoi(argv[++i]);
        } else if (a == "--help" || a == "-h") {
            std::cout <<
                "f4-scenario-player — Play an F4 scenario and render it\n"
                "\n"
                "Usage:\n"
                "  f4-scenario-player <scenario.json> [options]\n"
                "\n"
                "Options:\n"
                "  --screenshot <path>   Take a screenshot after 1.5s and exit\n"
                "  --harness <path>      Run BvrInterceptHarness headlessly over\n"
                "                        the scenario, write the three artifacts\n"
                "                        (bvr_intercept_result.json +\n"
                "                        bvr_intercept_summary.json +\n"
                "                        bvr_intercept_diary.json) beside <path>,\n"
                "                        print the verdict, and exit with the\n"
                "                        harness's exit code (0=all green,\n"
                "                        1=abort, 2=non-combat, 3=no detect,\n"
                "                        4=no launch, 5=no kill, 6=roster leak,\n"
                "                        9=non-deterministic). Mutually\n"
                "                        exclusive with --screenshot.\n"
                "  --horizon-sec <N>     Harness horizon in sim seconds (300)\n"
                "  --sample-sec <N>      Harness sample cadence in sim sec (30)\n"
                "  --runs <N>            Harness passes (2 = determinism proof)\n"
                "  --width <N>           Window width (default: 1600)\n"
                "  --height <N>          Window height (default: 900)\n"
                "  --record <path>       Force a FlightRecorder trace for this\n"
                "                        run, written to <path> on exit (the\n"
                "                        world viewer's Open Replay / Mission QC\n"
                "                        menu loads it). Path is relative to the\n"
                "                        current directory.\n"
                "  --record-every <N>    Trace decimation (with --record;\n"
                "                        1 = every tick, N = every Nth).\n"
                "  --help                Show this help message\n"
                "\n"
                "Controls:\n"
                "  Left-drag: orbit     Right-drag: pan     Scroll: zoom\n"
                "  Space: pause/resume  F: focus aircraft   R: reset view\n"
                "  Tab: cycle watched aircraft (bvr_intercept: EAGLE1/BANDIT1)\n"
                "  C: follow watched aircraft   F2: screenshot\n";
            return 0;
        } else if (scenario_path.empty()) {
            scenario_path = a;
        } else {
            std::cerr << "Unexpected argument: " << a << "\n";
            return 1;
        }
    }

    if (scenario_path.empty()) {
        std::cerr << "Usage: f4-scenario-player <scenario.json> [options]\n"
                  << "Run with --help for details.\n";
        return 1;
    }

    // --harness and --screenshot are mutually exclusive: one opens a
    // window (the render loop), the other refuses to. Both at once is a
    // user error — the artifacts each writes have nothing to do with
    // each other, and the screenshot path needs the GL context the
    // harness path deliberately avoids.
    if (run_harness && exit_after_screenshot) {
        std::cerr << "error: --harness and --screenshot are mutually "
                     "exclusive — --harness runs headlessly (no GL "
                     "context), --screenshot needs the render loop.\n";
        return 1;
    }

    f4::scenario_player::PlayerApp app;
    app.set_window_size(window_w, window_h);

    // SHOWCASE-1: force recording on BEFORE load_scenario (the override
    // is applied between the JSON parse and the Simulation build).
    if (!record_path.empty()) {
        app.set_recording(record_path, record_every);
    }

    try {
        app.load_scenario(scenario_path);
    } catch (const std::exception& e) {
        std::cerr << "error: failed to load scenario: " << e.what() << "\n";
        return 2;
    }

    // ── --harness: the M4 headless acceptance path ──────────────────────
    // Sibling to the --screenshot block below: load → run → write → exit.
    // The harness composes its OWN fresh Simulation (the determinism
    // proof demands a from-scratch build per pass), so the player's
    // already-initialized Simulation is left untouched. run_harness
    // returns the harness's exit code directly; we do NOT fall through
    // to app.run().
    if (run_harness) {
        try {
            return app.run_harness(harness_summary_out,
                                   harness_horizon_sec,
                                   harness_sample_sec,
                                   harness_runs);
        } catch (const std::exception& e) {
            std::cerr << "error: harness failed: " << e.what() << "\n";
            return 1;
        }
    }

    app.set_time_scale(speed);
    if (start_running) app.set_paused(false);
    if (start_follow) app.set_follow_camera(true);
    if (camera_distance > 0.0) app.set_camera_distance(camera_distance);

    if (exit_after_screenshot) {
        app.schedule_screenshot(static_cast<float>(shot_at_sec), screenshot_path);
    }

    app.run();
    return 0;
}
