// f4-world-viewer/cli/main.cpp — entry point.
//
//   f4-world-viewer                                       -> open empty viewer
//   f4-world-viewer world.json                            -> load world JSON
//   f4-world-viewer world.json terrain.json               -> load both
//   f4-world-viewer world.json terrain.json --screenshot out.png
//       Take a screenshot after 1.5 seconds and exit. Useful for headless
//       smoke tests; on Windows you'd run this from a cmd window.
//
// Use the File menu to import additional files (including raw .cam and
// THEATER.* binaries) at runtime.

#include <f4/viewer/viewer_app.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

int main(int argc, char** argv) {
    f4::viewer::ViewerApp app;

    // Parse positional + --screenshot flag.
    std::string screenshot_path;
    bool exit_after_screenshot = false;
    int positional = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--screenshot" && i + 1 < argc) {
            screenshot_path = argv[++i];
            exit_after_screenshot = true;
        } else if (positional == 0) {
            try { app.load_world_json(a); }
            catch (const std::exception& e) { std::cerr << "world load: " << e.what() << "\n"; }
            positional = 1;
        } else if (positional == 1) {
            try { app.load_terrain_json(a); }
            catch (const std::exception& e) { std::cerr << "terrain load: " << e.what() << "\n"; }
            positional = 2;
        }
    }

    if (exit_after_screenshot) {
        // Take a screenshot after 1.5 seconds (enough for first frame to render).
        app.schedule_screenshot(1.5f, screenshot_path);
        // Run for 3 seconds total, then exit.
        // The simplest way: spawn a thread that calls exit() after 3s.
        // (We use a portable approach via std::thread + std::exit.)
        std::thread([path = screenshot_path]() {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            std::cout << "Screenshot saved to " << path << "; exiting.\n";
            std::exit(0);
        }).detach();
    }

    app.run();
    return 0;
}
