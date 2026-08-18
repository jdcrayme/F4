// f4-scenario-player/cli/main.cpp — entry point.
//
//   f4-scenario-player <scenario.json>
//   f4-scenario-player <scenario.json> --screenshot out.png
//   f4-scenario-player <scenario.json> --run --speed 4 --shot-at 30 --screenshot out.png
//   f4-scenario-player <scenario.json> --width 1920 --height 1080
//
// Loads the scenario, initializes the simulation (spawns aircraft, loads
// 3D model, wires ATC), and runs the Raylib render loop. The aircraft
// starts at the parking spot in PAUSED state — press Space to begin taxi.

#include <f4/scenario_player/player_app.hpp>

#include <chrono>
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

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--screenshot" && i + 1 < argc) {
            screenshot_path = argv[++i];
            exit_after_screenshot = true;
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
                "  --width <N>           Window width (default: 1600)\n"
                "  --height <N>          Window height (default: 900)\n"
                "  --help                Show this help message\n"
                "\n"
                "Controls:\n"
                "  Left-drag: orbit     Right-drag: pan     Scroll: zoom\n"
                "  Space: pause/resume  F: focus aircraft   R: reset view\n"
                "  F2: screenshot\n";
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

    f4::scenario_player::PlayerApp app;
    app.set_window_size(window_w, window_h);

    try {
        app.load_scenario(scenario_path);
    } catch (const std::exception& e) {
        std::cerr << "error: failed to load scenario: " << e.what() << "\n";
        return 2;
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
