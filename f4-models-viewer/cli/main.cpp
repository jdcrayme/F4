// f4-models-viewer/cli/main.cpp — entry point.
//
//   f4-models-viewer                                       -> open empty viewer
//   f4-models-viewer KoreaObj.HDR KoreaObj.LOD             -> load model database
//   f4-models-viewer --install /path/to/falcon4            -> set install path
//   f4-models-viewer --parent 42                           -> select parent model
//   f4-models-viewer --lod 1                               -> select LOD level
//   f4-models-viewer --screenshot out.png                  -> take screenshot
//   f4-models-viewer --width 1920 --height 1080            -> set window size

#include <f4/models_viewer/viewer_app.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

int main(int argc, char** argv) {
    f4::models_viewer::ViewerApp app;

    std::string screenshot_path;
    bool exit_after_screenshot = false;
    std::string install_path;
    bool have_install = false;
    int parent_idx = -1;
    int lod_idx = -1;
    int window_w = 1600;
    int window_h = 900;

    // Positional args: HDR path, LOD path
    std::string hdr_path;
    std::string lod_path;
    int positional = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--install" && i + 1 < argc) {
            install_path = argv[++i];
            have_install = true;
        } else if (a == "--parent" && i + 1 < argc) {
            parent_idx = std::atoi(argv[++i]);
        } else if (a == "--lod" && i + 1 < argc) {
            lod_idx = std::atoi(argv[++i]);
        } else if (a == "--screenshot" && i + 1 < argc) {
            screenshot_path = argv[++i];
            exit_after_screenshot = true;
        } else if (a == "--width" && i + 1 < argc) {
            window_w = std::atoi(argv[++i]);
        } else if (a == "--height" && i + 1 < argc) {
            window_h = std::atoi(argv[++i]);
        } else if (a == "--help" || a == "-h") {
            std::cout <<
                "f4-models-viewer — Interactive 3D model viewer for F4 KoreaObj data\n"
                "\n"
                "Usage:\n"
                "  f4-models-viewer [HDR_PATH] [LOD_PATH] [options]\n"
                "\n"
                "Options:\n"
                "  --install <path>      Set Falcon 4.0 install path\n"
                "  --parent <N>          Select parent model by index\n"
                "  --lod <N>             Select LOD level (0 = highest detail)\n"
                "  --screenshot <path>   Take a screenshot and exit\n"
                "  --width <N>           Window width (default: 1600)\n"
                "  --height <N>          Window height (default: 900)\n"
                "  --help                Show this help message\n"
                "\n"
                "Controls:\n"
                "  Left-drag: orbit     Right-drag: pan     Scroll: zoom\n"
                "  F: fit to model      R: reset camera     F2: screenshot\n";
            return 0;
        } else if (positional == 0) {
            hdr_path = a;
            positional = 1;
        } else if (positional == 1) {
            lod_path = a;
            positional = 2;
        }
    }

    // Apply --install
    if (have_install) {
        if (!app.set_install_path(install_path)) {
            std::cerr << "warning: install detection failed for '" << install_path
                      << "' — continuing without install\n";
        }
    }

    // Apply positional HDR/LOD
    if (!hdr_path.empty() && !lod_path.empty()) {
        try {
            app.load_model(hdr_path, lod_path);
        } catch (const std::exception& e) {
            std::cerr << "model load error: " << e.what() << "\n";
        }
    }

    // Apply --parent
    if (parent_idx >= 0) {
        app.select_parent(parent_idx);
    }

    // Apply --lod
    if (lod_idx >= 0) {
        app.select_lod(lod_idx);
    }

    // Schedule screenshot (for headless smoke tests)
    if (exit_after_screenshot) {
        app.schedule_screenshot(1.5f, screenshot_path);
        // Exit after 3 seconds total
        std::thread([path = screenshot_path]() {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            std::cout << "Screenshot saved to " << path << "; exiting.\n";
            std::exit(0);
        }).detach();
    }

    app.run();
    return 0;
}
