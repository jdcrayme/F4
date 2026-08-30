// f4-world-viewer/cli/main.cpp — entry point.
//
//   f4-world-viewer                                       -> open empty viewer
//   f4-world-viewer world.json                            -> load world JSON
//   f4-world-viewer world.json terrain.json               -> load both
//   f4-world-viewer --install /path/to/falcon4            -> set install path
//                                                            (cached to settings)
//   f4-world-viewer --install /path --campaign korea save1
//                                                         -> set install + load
//                                                            campaign in-process
//   f4-world-viewer --replay trace.json                   -> open in REPLAY MODE
//                                                            (Path B2 — steps
//                                                            through a recorded
//                                                            flight trace; see
//                                                            Docs/DIGI_AI_PHASE2_PLAN.md
//                                                            §7)
//   f4-world-viewer --hex-inspect /path/to/file           -> open the Hex Inspector
//                                                            panel with file loaded
//   f4-world-viewer --install /path/to/falcon4 --diagnostics
//                                                         -> print install diagnostics
//                                                            to stderr + exit (no GUI)
//   f4-world-viewer --install /path/to/falcon4 --snapshot out.txt
//                                                         -> write install snapshot
//                                                            (hex dumps of every
//                                                             interesting data file)
//                                                            to out.txt + exit
//   f4-world-viewer --install /path/to/falcon4 --list-files out.txt
//                                                         -> write recursive file
//                                                            listing (every file
//                                                            under install root,
//                                                            path + size, no hex)
//                                                            to out.txt + exit
//   f4-world-viewer world.json terrain.json --screenshot out.png
//       Take a screenshot after 1.5 seconds and exit. Useful for headless
//       smoke tests; on Windows you'd run this from a cmd window.
//   f4-world-viewer world.json terrain.json --zoom 8 --center 500,500
//       Launch focused on a specific area. Useful when you want to inspect
//       individual objectives/units without manually panning/zooming.
//
// On startup, the viewer auto-restores the last install path from
// settings.json (Linux: ~/.config/f4-viewer/; macOS: ~/Library/Application
// Support/f4-viewer/; Windows: %APPDATA%/F4Viewer/). The --install flag
// is for first-run or scripted use; once set, the path persists.

#include <f4/viewer/viewer_app.hpp>
#include <f4/viewer/hex_inspector.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

int main(int argc, char** argv) {
    f4::viewer::ViewerApp app;

    // Parse positional + flags. Positional args are world JSON, terrain JSON
    // (in that order) — kept for backward compatibility with the existing
    // screenshot smoke tests.
    std::string screenshot_path;
    bool exit_after_screenshot = false;
    bool have_initial_camera = false;
    float init_cx = 0.0f, init_cy = 0.0f, init_zoom = 4.0f;
    int positional = 0;

    // --install + --campaign flags for the new install-aware flow.
    std::string install_path;
    std::string campaign_theater;
    std::string campaign_stem;
    bool have_install = false;
    bool have_campaign = false;

    // --hex-inspect <path> — open the Hex Inspector with a file pre-loaded.
    std::string hex_inspect_path;
    bool have_hex_inspect = false;

    // --diagnostics — print install diagnostics to stderr and exit.
    // Useful for debugging "class table not found" or "campaign can't
    // open" issues without having to launch the GUI.
    bool print_diagnostics = false;

    // --snapshot <path> — write install snapshot (hex dumps of every
    // interesting data file) to <path> and exit. See snapshot.hpp.
    std::string snapshot_path;
    bool have_snapshot = false;

    // --list-files <path> — write a recursive file listing of the
    // install (every regular file's relative path + size, no hex dumps)
    // to <path> and exit. Used to document install layouts across
    // vanilla / FreeFalcon / BMS installs and to spot files our
    // curated snapshot list missed. See snapshot.hpp.
    std::string list_files_path;
    bool have_list_files = false;

    // --replay <path> — open a FlightRecorder trace JSON and switch
    // the viewer to REPLAY MODE. See replay_mode.hpp. The viewer will
    // render the trail + current snapshot + AI state panel instead of
    // the normal campaign canvas. Useful for debugging AI scenarios
    // after the fact — the trace is self-contained (no campaign data
    // needed).
    std::string replay_path;
    bool have_replay = false;
    std::string select_name;              // --select <substring>

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--screenshot" && i + 1 < argc) {
            screenshot_path = argv[++i];
            exit_after_screenshot = true;
        } else if (a == "--zoom" && i + 1 < argc) {
            init_zoom = std::strtof(argv[++i], nullptr);
            have_initial_camera = true;
        } else if (a == "--center" && i + 1 < argc) {
            // Parse "x,y"
            std::string s = argv[++i];
            auto comma = s.find(',');
            if (comma != std::string::npos) {
                init_cx = std::strtof(s.substr(0, comma).c_str(), nullptr);
                init_cy = std::strtof(s.substr(comma + 1).c_str(), nullptr);
                have_initial_camera = true;
            } else {
                std::cerr << "--center expects 'x,y' (got '" << s << "')\n";
            }
        } else if (a == "--install" && i + 1 < argc) {
            install_path = argv[++i];
            have_install = true;
        } else if (a == "--campaign" && i + 2 < argc) {
            campaign_theater = argv[++i];
            campaign_stem = argv[++i];
            have_campaign = true;
        } else if (a == "--hex-inspect" && i + 1 < argc) {
            hex_inspect_path = argv[++i];
            have_hex_inspect = true;
        } else if (a == "--diagnostics") {
            print_diagnostics = true;
        } else if (a == "--snapshot" && i + 1 < argc) {
            snapshot_path = argv[++i];
            have_snapshot = true;
        } else if (a == "--list-files" && i + 1 < argc) {
            list_files_path = argv[++i];
            have_list_files = true;
        } else if (a == "--replay" && i + 1 < argc) {
            replay_path = argv[++i];
            have_replay = true;
        } else if (a == "--select" && i + 1 < argc) {
            select_name = argv[++i];
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

    // Apply --install: detect + cache. May override the restored settings.
    if (have_install) {
        if (!app.set_install_path(install_path)) {
            std::cerr << "warning: install detection failed for '" << install_path
                      << "' — continuing without install\n";
        }
    }

    // Apply --campaign: load via the install-aware path. Requires --install
    // to have succeeded (or to have been restored from settings).
    if (have_campaign) {
        try {
            app.load_campaign_from_install(campaign_theater, campaign_stem);
        } catch (const std::exception& e) {
            std::cerr << "campaign load failed: " << e.what() << "\n";
            return 1;
        }
    }

    // Apply --hex-inspect: open the Hex Inspector panel with a file loaded.
    if (have_hex_inspect) {
        app.open_hex_inspector_with_file(hex_inspect_path);
    }

    // Apply --select: pick the first objective whose name contains the
    // substring (programmatic map click — lights up the 3D Ground
    // Layout panel for headless screenshot validation).
    if (!select_name.empty()) {
        if (!app.select_by_name(select_name)) {
            std::cerr << "warning: no objective name matches '" << select_name
                      << "'\n";
        }
    }

    // Print install diagnostics to stderr + exit (no GUI). Useful for
    // debugging install-detection issues without launching the viewer.
    // The user runs: f4-world-viewer --install /path --diagnostics
    if (print_diagnostics) {
        std::cerr << app.install_diagnostics_text();
        return 0;
    }

    // Write an install snapshot to disk + exit (no GUI). Dumps
    // the first 8 KB of every interesting Falcon4 data file as a hex
    // dump, so the dev team can ground-truth binary struct layouts.
    // The user runs: f4-world-viewer --install /path --snapshot out.txt
    if (have_snapshot) {
        std::string err;
        if (app.snapshot_install_files(snapshot_path, &err)) {
            std::cerr << "snapshot written to: " << snapshot_path << "\n";
            return 0;
        } else {
            std::cerr << "snapshot failed: " << err << "\n";
            return 1;
        }
    }

    // Write a recursive file listing to disk + exit (no GUI). The user
    // runs: f4-world-viewer --install /path --list-files out.txt
    // The output is a plain-text manifest of every regular file under
    // the install root (relative path + size in bytes), sorted within
    // each directory. Used to document install layouts across vanilla
    // / FreeFalcon / BMS installs side-by-side and to spot files our
    // curated snapshot list missed.
    if (have_list_files) {
        std::string err;
        if (app.list_install_files(list_files_path, &err)) {
            std::cerr << "file listing written to: " << list_files_path << "\n";
            return 0;
        } else {
            std::cerr << "file listing failed: " << err << "\n";
            return 1;
        }
    }

    if (have_initial_camera) {
        app.set_initial_camera(init_cx, init_cy, init_zoom);
    }

    // Apply --replay: load the trace JSON and switch to replay mode.
    // This MUST happen after any --install / --campaign loading so the
    // replay render path is the one that takes over when run() starts.
    // (Replay mode is mutually exclusive with the campaign canvas —
    // the viewer renders either one or the other.)
    if (have_replay) {
        std::string err;
        if (!app.load_replay(replay_path, &err)) {
            std::cerr << "replay load failed: " << err << "\n";
            return 1;
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
