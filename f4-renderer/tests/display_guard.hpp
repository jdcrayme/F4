// f4-renderer/tests/display_guard.hpp
//
// Graceful skip for GPU-context tests in display-less environments.
//
// The GPU tests (orbit_camera, lit_shader, texture_cache, draw_3d,
// feature_mesh, world_renderer) need a real GL context via raylib's
// InitWindow(). On a headless Linux box (CI, containers, WSL without X)
// raylib 5.0's InitWindow fails to open the display and subsequently
// SEGFAULTS inside rlglInit (the GL loader runs even when GLFW failed),
// and CloseWindow() on a never-opened window crashes too. The crash
// aborts the whole ctest run with a bare SIGSEGV, which is both noisy
// and unhelpful — the fix is to detect the condition BEFORE InitWindow
// and skip instead.
//
// Usage (in a fixture's SetUp(), replacing the raw SetConfigFlags +
// InitWindow pair):
//
//     void SetUp() override {
//         if (!initialized_) {
//             if (!f4::testing::init_window_if_display(
//                     256, 256, "test",
//                     FLAG_WINDOW_HIDDEN | FLAG_WINDOW_UNDECORATED)) {
//                 GTEST_SKIP() << "no display — GPU-context test skipped";
//             }
//             initialized_ = true;
//         }
//     }
//
// NOTE: the GTEST_SKIP() call must live in SetUp itself, not inside a
// helper — the macro is a `return GTEST_MESSAGE_(...)`, so calling it
// from another function returns from the helper and SetUp would carry
// on as if the window existed.
//
// On Windows and macOS there is always a window system, so the guard is
// a no-op there (the check compiles out and the function just init's).

#pragma once

#include <raylib.h>

#include <cstdlib>

namespace f4::testing {

/// True when raylib's InitWindow can plausibly succeed in this
/// environment. Linux/X11 requires DISPLAY to be set (raylib 5.0 in this
/// project is configured with USE_WAYLAND OFF). Windows/macOS always
/// report true — window creation works without environment setup.
inline bool display_available_for_gpu_tests() {
#if defined(_WIN32) || defined(__APPLE__)
    return true;
#else
    const char* d = std::getenv("DISPLAY");
    return d != nullptr && d[0] != '\0';
#endif
}

/// SetConfigFlags() + InitWindow(), guarded by a display check: returns
/// false (without touching the GPU) when no display is available, so the
/// caller can GTEST_SKIP() instead of segfaulting inside
/// InitWindow→rlglInit. Returns true after a normal InitWindow() call —
/// check IsWindowReady() for full guarantee, as usual with raylib.
inline bool init_window_if_display(int w, int h, const char* title,
                                   unsigned flags = FLAG_WINDOW_HIDDEN) {
    if (!display_available_for_gpu_tests()) {
        return false;
    }
    SetConfigFlags(flags);
    InitWindow(w, h, title);
    return true;
}

}  // namespace f4::testing
