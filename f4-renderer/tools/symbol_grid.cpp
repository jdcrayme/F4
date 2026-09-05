// symbol_grid.cpp — render every symbol in symbols/ to a grid PNG.
//
// Headless visual verification that the SVG→SymbolLibrary pipeline produces
// distinct, recognizable shapes (not all squares). Loads each .svg in
// symbols/, lays them out on a 1024x1024 grid, draws with a fixed team
// color + outline, and saves a PNG. No UI panels, no map, no terrain —
// just the symbols.
//
// Usage: symbol_grid <symbols_dir> <out.png>

#include <f4/renderer/symbol_library.hpp>
#include <f4/renderer/svg_import.hpp>
#include <f4/renderer/entity_render.hpp>  // key mapping tables

#include <raylib.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <symbols_dir> <out.png>\n", argv[0]);
        return 1;
    }
    const std::filesystem::path symbols_dir = argv[1];
    const std::string out_path = argv[2];

    // Collect every .svg in the directory, sorted for stable output.
    std::vector<std::filesystem::path> files;
    for (const auto& e : std::filesystem::directory_iterator(symbols_dir)) {
        if (e.path().extension() == ".svg") files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        std::fprintf(stderr, "no .svg files in %s\n", symbols_dir.c_str());
        return 1;
    }

    // 1024x1024 canvas; pick a column count so each cell is ~96 px.
    const int cols = 10;
    const int rows = static_cast<int>((files.size() + cols - 1) / cols);
    const int cell = 96;
    const int w = cols * cell;
    const int h = rows * cell;

    SetTraceLogLevel(LOG_WARNING);
    InitWindow(w, h, "symbol_grid");
    SetTargetFPS(30);

    f4::renderer::SymbolDirectory symbols(symbols_dir);

    // Fill = friendly blue, outline = dark contrast — same palette the
    // canvas uses for owner=team 1.
    const f4::renderer::RlColor fill{
        static_cast<unsigned char>( 60),
        static_cast<unsigned char>(140),
        static_cast<unsigned char>(230),
        255};
    const f4::renderer::RlColor outline{
        static_cast<unsigned char>( 20),
        static_cast<unsigned char>( 40),
        static_cast<unsigned char>( 80),
        255};

    // Draw two frames so the GPU has a valid back buffer, then snap.
    for (int frame = 0; frame < 3; ++frame) {
        BeginDrawing();
        ClearBackground(Color{30, 30, 36, 255});

        for (std::size_t i = 0; i < files.size(); ++i) {
            const auto& f = files[i];
            const std::string key = f.stem().string();
            const int col = static_cast<int>(i % cols);
            const int row = static_cast<int>(i / cols);
            const float cx = col * cell + cell * 0.5f;
            const float cy = row * cell + cell * 0.5f;
            symbols.draw(key, cx, cy, /*size_px=*/78.0f, fill, outline);

            // Tiny label under each so we can match shape → key by eye.
            DrawText(key.c_str(),
                     static_cast<int>(col * cell + 4),
                     static_cast<int>(row * cell + cell - 12), 9,
                     Color{200, 200, 210, 220});
        }

        EndDrawing();
    }
    TakeScreenshot(out_path.c_str());
    CloseWindow();
    std::printf("wrote %s (%d symbols)\n", out_path.c_str(),
                static_cast<int>(files.size()));
    return 0;
}
