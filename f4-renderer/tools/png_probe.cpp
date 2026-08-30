// png_probe.cpp — print average pixel colors of image regions (debug tool).
// Build: cl /std:c++20 /I<raylib>/src png_probe.cpp <raylib.lib>
// usage: png_probe <image> [x y w h] — probe a sub-rectangle at 8x8.
#include "raylib.h"
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: png_probe <image> [x y w h]\n"); return 1; }
    Image img = LoadImage(argv[1]);
    if (img.data == nullptr) { printf("load failed\n"); return 1; }
    int px = 0, py = 0, pw = img.width, ph = img.height;
    if (argc >= 6) {
        px = atoi(argv[2]); py = atoi(argv[3]);
        pw = atoi(argv[4]); ph = atoi(argv[5]);
    }
    printf("%dx%d probe %d,%d %dx%d\n", img.width, img.height, px, py, pw, ph);
    const int rows = 8, cols = 8;
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            const int x0 = px + pw * c / cols, x1 = px + pw * (c + 1) / cols;
            const int y0 = py + ph * r / rows, y1 = py + ph * (r + 1) / rows;
            long sr = 0, sg = 0, sb = 0, n = 0, black = 0;
            double var = 0;  // luminance variance — texture detail detector
            double mean = 0;
            for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) {
                Color p = GetImageColor(img, x, y);
                sr += p.r; sg += p.g; sb += p.b; ++n;
                if (p.r < 8 && p.g < 8 && p.b < 12) ++black;
            }
            const double ir = sr, ig = sg, ib = sb, in_ = n;
            mean = (ir + ig + ib) / (3.0 * in_);
            for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) {
                Color p = GetImageColor(img, x, y);
                const double lum = (p.r + p.g + p.b) / 3.0;
                var += (lum - mean) * (lum - mean);
            }
            var /= in_;
            printf("r%dc%d #%02x%02x%02x v=%.0f  ", r, c,
                   (int)(sr / n), (int)(sg / n), (int)(sb / n), var);
        }
        printf("\n");
    }
    UnloadImage(img);
    return 0;
}
