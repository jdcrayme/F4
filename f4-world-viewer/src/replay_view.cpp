// f4-world-viewer/src/replay_view.cpp
//
// ViewerApp replay-mode render methods — the raylib/imgui-dependent half
// of replay mode. The pure-data half (ReplayState helpers, load_replay,
// fit_replay_camera) lives in replay_mode.cpp so it can be unit-tested
// without pulling in raylib.
//
// Methods defined here (all private members of ViewerApp, dispatched
// from run() when impl_->replay.active() is true):
//   handle_replay_input  — pan/zoom + arrow-key stepping + playback
//   draw_replay_canvas   — 2D top-down: grid, trail, target, aircraft, HUD
//   draw_replay_panel    — right-side ImGui panel with scrubber + state
//
// See replay_mode.hpp for the data model and DIGI_AI_PHASE2_PLAN.md §7
// for the design rationale.

#include "viewer_state.hpp"
#include <f4/viewer/replay_mode.hpp>

#include <raylib.h>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace f4::viewer {

// Screen → world (replay camera). Inline because it's used in hot loops.
static void replay_screen_to_world(float window_w, float window_h,
                                    float cam_x, float cam_y, float cam_zoom,
                                    float sx, float sy,
                                    float* ex_ft, float* ey_ft) {
    const float cx = window_w * 0.5f;
    const float cy = window_h * 0.5f;
    *ex_ft = cam_x + (sx - cx) / cam_zoom;
    *ey_ft = cam_y - (sy - cy) / cam_zoom;
}

// Cross-track error → color. 0 = green, 100+ ft = red.
static Color cross_track_color(double xte_ft) {
    const double t = std::min(1.0, std::abs(xte_ft) / 100.0);
    const auto r = static_cast<unsigned char>(60 + t * 195);
    const auto g = static_cast<unsigned char>(220 - t * 160);
    const unsigned char b = 60;
    return {r, g, b, 255};
}

void ViewerApp::handle_replay_input() {
    const Vector2 mouse = GetMousePosition();

    // Pan: middle-mouse or left-drag (when not over ImGui)
    if (IsMouseButtonPressed(MOUSE_BUTTON_MIDDLE) ||
        (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !ImGui::GetIO().WantCaptureMouse)) {
        impl_->replay_dragging = true;
        impl_->replay_drag_start = mouse;
        impl_->replay_drag_cam_x0 = impl_->replay_cam_x;
        impl_->replay_drag_cam_y0 = impl_->replay_cam_y;
    }
    if (impl_->replay_dragging &&
        (IsMouseButtonReleased(MOUSE_BUTTON_MIDDLE) ||
         IsMouseButtonReleased(MOUSE_BUTTON_LEFT))) {
        impl_->replay_dragging = false;
    }
    if (impl_->replay_dragging) {
        const float dx = (mouse.x - impl_->replay_drag_start.x) / impl_->replay_cam_zoom;
        const float dy = (mouse.y - impl_->replay_drag_start.y) / impl_->replay_cam_zoom;
        impl_->replay_cam_x = impl_->replay_drag_cam_x0 - dx;
        impl_->replay_cam_y = impl_->replay_drag_cam_y0 + dy;
    }

    // Zoom: mouse wheel
    const float wheel = GetMouseWheelMove();
    if (wheel != 0.0f && !ImGui::GetIO().WantCaptureMouse) {
        float ex_before, ey_before;
        replay_screen_to_world(static_cast<float>(impl_->window_w), static_cast<float>(impl_->window_h),
                                impl_->replay_cam_x, impl_->replay_cam_y, impl_->replay_cam_zoom,
                                mouse.x, mouse.y, &ex_before, &ey_before);
        impl_->replay_cam_zoom *= (wheel > 0) ? 1.15f : (1.0f / 1.15f);
        impl_->replay_cam_zoom = std::clamp(impl_->replay_cam_zoom, 1e-4f, 10.0f);
        float ex_after, ey_after;
        replay_screen_to_world(static_cast<float>(impl_->window_w), static_cast<float>(impl_->window_h),
                                impl_->replay_cam_x, impl_->replay_cam_y, impl_->replay_cam_zoom,
                                mouse.x, mouse.y, &ex_after, &ey_after);
        impl_->replay_cam_x += ex_before - ex_after;
        impl_->replay_cam_y += ey_before - ey_after;
    }

    // Playback keys (only when not typing in ImGui)
    const bool want_kbd = !ImGui::GetIO().WantCaptureKeyboard;

    if (IsKeyPressed(KEY_SPACE) && want_kbd) {
        impl_->replay.paused = !impl_->replay.paused;
    }
    if (IsKeyPressed(KEY_RIGHT) && want_kbd) {
        impl_->replay.paused = true;
        impl_->replay.step(1);
    }
    if (IsKeyPressed(KEY_LEFT) && want_kbd) {
        impl_->replay.paused = true;
        impl_->replay.step(-1);
    }
    if (IsKeyPressed(KEY_KP_ADD) || (IsKeyPressed(KEY_EQUAL) && want_kbd)) {
        impl_->replay.speed_multiplier =
            std::min(10.0, impl_->replay.speed_multiplier * 1.5);
    }
    if (IsKeyPressed(KEY_KP_SUBTRACT) || (IsKeyPressed(KEY_MINUS) && want_kbd)) {
        impl_->replay.speed_multiplier =
            std::max(0.25, impl_->replay.speed_multiplier / 1.5);
    }
    if (IsKeyPressed(KEY_HOME) && want_kbd) {
        impl_->replay.jump_to_start();
    }
    if (IsKeyPressed(KEY_END) && want_kbd) {
        impl_->replay.jump_to_end();
    }
    if (IsKeyPressed(KEY_F) && want_kbd) {
        fit_replay_camera(impl_->replay,
                          static_cast<float>(impl_->window_w),
                          static_cast<float>(impl_->window_h),
                          impl_->replay_cam_x,
                          impl_->replay_cam_y,
                          impl_->replay_cam_zoom);
    }

    // Advance playback
    if (!impl_->replay.paused && impl_->replay.active()) {
        const double dt = GetFrameTime();
        impl_->replay.tick_accumulator += dt * impl_->replay.speed_multiplier;
        while (impl_->replay.tick_accumulator >= 1.0) {
            impl_->replay.step(1);
            impl_->replay.tick_accumulator -= 1.0;
            // Pause at end
            const auto& snaps = impl_->replay.recording->snapshots();
            if (impl_->replay.current_tick >= snaps.size() - 1) {
                impl_->replay.paused = true;
                impl_->replay.tick_accumulator = 0.0;
                break;
            }
        }
    }
}

void ViewerApp::draw_replay_canvas() {
    if (!impl_->replay.active()) return;
    const auto* snap = impl_->replay.current_snapshot();
    if (!snap) return;

    // Draw a subtle grid (every 1000 ft)
    if (impl_->replay.show_trail) {
        const float grid_step_ft = 1000.0f;
        const Color grid_color = {40, 42, 48, 255};
        // Find visible world bounds
        float ex0, ey0, ex1, ey1;
        replay_screen_to_world(static_cast<float>(impl_->window_w), static_cast<float>(impl_->window_h),
                                impl_->replay_cam_x, impl_->replay_cam_y, impl_->replay_cam_zoom,
                                0, 0, &ex0, &ey0);
        replay_screen_to_world(static_cast<float>(impl_->window_w), static_cast<float>(impl_->window_h),
                                impl_->replay_cam_x, impl_->replay_cam_y, impl_->replay_cam_zoom,
                                static_cast<float>(impl_->window_w),
                                static_cast<float>(impl_->window_h),
                                &ex1, &ey1);
        const float min_x = std::min(ex0, ex1);
        const float max_x = std::max(ex0, ex1);
        const float min_y = std::min(ey0, ey1);
        const float max_y = std::max(ey0, ey1);
        const float step_screen = grid_step_ft * impl_->replay_cam_zoom;
        if (step_screen > 8.0f) {  // don't draw if grid lines would be <8px apart
            for (float gx = std::floor(min_x / grid_step_ft) * grid_step_ft;
                 gx <= max_x; gx += grid_step_ft) {
                const auto p1 = impl_->replay_world_to_screen(gx, min_y);
                const auto p2 = impl_->replay_world_to_screen(gx, max_y);
                DrawLineV(p1, p2, grid_color);
            }
            for (float gy = std::floor(min_y / grid_step_ft) * grid_step_ft;
                 gy <= max_y; gy += grid_step_ft) {
                const auto p1 = impl_->replay_world_to_screen(min_x, gy);
                const auto p2 = impl_->replay_world_to_screen(max_x, gy);
                DrawLineV(p1, p2, grid_color);
            }
        }
    }

    // Draw trail polyline (focused entity only), colored by cross-track error
    if (impl_->replay.show_trail) {
        auto focused = impl_->replay.focused_entity_snapshots();
        if (focused.size() >= 2) {
            // Only draw up to the current tick
            std::size_t draw_count = 0;
            for (std::size_t i = 0; i < focused.size(); ++i) {
                if (focused[i].tick > snap->tick) break;
                draw_count = i + 1;
            }
            if (draw_count >= 2) {
                for (std::size_t i = 1; i < draw_count; ++i) {
                    const auto& s0 = focused[i-1];
                    const auto& s1 = focused[i];
                    const auto p0 = impl_->replay_world_to_screen(s0.position.x, s0.position.y);
                    const auto p1 = impl_->replay_world_to_screen(s1.position.x, s1.position.y);
                    const Color c = cross_track_color(s1.cross_track_error_ft);
                    DrawLineEx(p0, p1, 2.0f, c);
                }
            }
        }
    }

    // Draw intended path (target positions connected, as a dashed line)
    if (impl_->replay.show_intended_path) {
        auto focused = impl_->replay.focused_entity_snapshots();
        const Color path_color = {120, 160, 220, 180};
        for (std::size_t i = 1; i < focused.size(); ++i) {
            if (focused[i].tick > snap->tick) break;
            const auto& s0 = focused[i-1];
            const auto& s1 = focused[i];
            // Connect s0.target_position to s1.target_position
            if (s0.target_position.x != 0.0 || s0.target_position.y != 0.0) {
                const auto p0 = impl_->replay_world_to_screen(
                    s0.target_position.x, s0.target_position.y);
                const auto p1 = impl_->replay_world_to_screen(
                    s1.target_position.x, s1.target_position.y);
                // Dashed line: draw every other segment
                if (i % 2 == 0) {
                    DrawLineV(p0, p1, path_color);
                }
            }
        }
    }

    // Draw target marker (current target_position)
    if (impl_->replay.show_target_marker &&
        (snap->target_position.x != 0.0 || snap->target_position.y != 0.0)) {
        const auto p = impl_->replay_world_to_screen(
            snap->target_position.x, snap->target_position.y);
        const float sz = 8.0f;
        DrawRectangleLines(static_cast<int>(p.x - sz), static_cast<int>(p.y - sz),
                           static_cast<int>(sz * 2), static_cast<int>(sz * 2),
                           {220, 200, 80, 255});
        if (impl_->replay.show_ai_labels && !snap->target_description.empty()) {
            DrawText(snap->target_description.c_str(),
                     static_cast<int>(p.x + sz + 4),
                     static_cast<int>(p.y - sz - 4),
                     12, {220, 200, 80, 255});
        }
    }

    // Draw the aircraft: a triangle oriented by heading_rad (compass, +y=N)
    {
        const auto p = impl_->replay_world_to_screen(snap->position.x, snap->position.y);
        const float hdg = static_cast<float>(snap->heading_rad);
        // Triangle pointing up (+y) at heading 0; rotate by -hdg because
        // screen-y is flipped (north = screen-up = -y).
        const float cos_h = std::cos(-hdg);
        const float sin_h = std::sin(-hdg);
        const float len = 14.0f;   // pixels
        const float wid = 7.0f;
        // Nose (forward), tail (back), left wing, right wing
        const Vector2 nose = {p.x + 0 * cos_h - (-len) * sin_h,
                              p.y + 0 * sin_h + (-len) * cos_h};
        const Vector2 tail = {p.x + 0 * cos_h - (len) * sin_h,
                              p.y + 0 * sin_h + (len) * cos_h};
        const Vector2 lw   = {p.x + (-wid) * cos_h - (len * 0.5f) * sin_h,
                              p.y + (-wid) * sin_h + (len * 0.5f) * cos_h};
        const Vector2 rw   = {p.x + (wid) * cos_h - (len * 0.5f) * sin_h,
                              p.y + (wid) * sin_h + (len * 0.5f) * cos_h};
        // Body color: cyan for the focused aircraft
        const Color body = {80, 200, 220, 255};
        DrawTriangle(nose, lw, rw, body);
        DrawTriangle(nose, rw, tail, body);
        // Outline
        DrawLineV(nose, lw, {255, 255, 255, 200});
        DrawLineV(nose, rw, {255, 255, 255, 200});
        DrawLineV(lw, tail, {255, 255, 255, 200});
        DrawLineV(rw, tail, {255, 255, 255, 200});
    }

    // AI state label above aircraft
    if (impl_->replay.show_ai_labels) {
        const auto p = impl_->replay_world_to_screen(snap->position.x, snap->position.y);
        const std::string label =
            snap->callsign + " | " + snap->ai_mode + " | " + snap->ai_state;
        DrawText(label.c_str(),
                 static_cast<int>(p.x - label.size() * 3.5f),
                 static_cast<int>(p.y - 36),
                 14, {255, 255, 255, 230});
        // Kinematics line
        if (impl_->replay.show_kinematics_hud) {
            char kin[128];
            std::snprintf(kin, sizeof(kin),
                "V=%.0fkts A=%.0fft H=%.0f%s",
                snap->vcas_kts,
                snap->altitude_msl_ft,
                snap->heading_deg(),
                snap->on_ground ? " GND" : "");
            DrawText(kin,
                     static_cast<int>(p.x - 60),
                     static_cast<int>(p.y - 20),
                     12, {200, 200, 200, 200});
        }
    }

    // Top-left overlay: replay status
    {
        const auto& rec = *impl_->replay.recording;
        const std::size_t total = rec.size();
        char status[256];
        std::snprintf(status, sizeof(status),
            "REPLAY  tick %zu / %zu   time %.1fs   %s   %.2fx",
            impl_->replay.current_tick + 1,
            total,
            snap->sim_time_s,
            impl_->replay.paused ? "PAUSED" : "PLAYING",
            impl_->replay.speed_multiplier);
        DrawText(status, 12, 12, 16, {255, 255, 255, 230});
        DrawText("Space=play/pause  Left/Right=step  +/-=speed  Home/End=jump  F=fit",
                 12, 34, 12, {180, 180, 180, 220});
    }
}

void ViewerApp::draw_replay_panel() {
    if (!impl_->replay.active()) return;
    const auto* snap = impl_->replay.current_snapshot();
    if (!snap) return;

    // Right-side detail panel
    ImGui::SetNextWindowPos(ImVec2(static_cast<float>(impl_->window_w) - 360.0f, 60.0f),
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(350, 600), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Replay Inspector", nullptr,
                      ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    // Scrubber
    const auto& rec = *impl_->replay.recording;
    const std::size_t total = rec.size();
    int tick_int = static_cast<int>(impl_->replay.current_tick);
    ImGui::Text("Tick %zu / %zu   (t = %.2fs)",
                impl_->replay.current_tick + 1, total, snap->sim_time_s);
    if (ImGui::SliderInt("##tick", &tick_int, 0,
                          static_cast<int>(total - 1))) {
        impl_->replay.paused = true;
        impl_->replay.current_tick = static_cast<std::size_t>(tick_int);
    }

    // Playback controls
    if (ImGui::Button(impl_->replay.paused ? "Play [Space]" : "Pause [Space]")) {
        impl_->replay.paused = !impl_->replay.paused;
    }
    ImGui::SameLine();
    if (ImGui::Button("|< Home")) impl_->replay.jump_to_start();
    ImGui::SameLine();
    if (ImGui::Button("< Step")) { impl_->replay.paused = true; impl_->replay.step(-1); }
    ImGui::SameLine();
    if (ImGui::Button("Step >")) { impl_->replay.paused = true; impl_->replay.step(1); }
    ImGui::SameLine();
    if (ImGui::Button("End >|")) impl_->replay.jump_to_end();

    // Speed
    ImGui::Text("Speed: %.2fx", impl_->replay.speed_multiplier);
    ImGui::SameLine();
    if (ImGui::Button("-")) {
        impl_->replay.speed_multiplier =
            std::max(0.25, impl_->replay.speed_multiplier / 1.5);
    }
    ImGui::SameLine();
    if (ImGui::Button("+")) {
        impl_->replay.speed_multiplier =
            std::min(10.0, impl_->replay.speed_multiplier * 1.5);
    }
    ImGui::SameLine();
    const char* speeds[] = {"0.25x","0.5x","1x","2x","5x","10x"};
    const double speed_vals[] = {0.25, 0.5, 1.0, 2.0, 5.0, 10.0};
    for (int i = 0; i < 6; ++i) {
        if (i > 0) ImGui::SameLine();
        if (ImGui::SmallButton(speeds[i])) {
            impl_->replay.speed_multiplier = speed_vals[i];
        }
    }

    ImGui::Separator();

    // Entity picker (multi-aircraft)
    if (impl_->replay.entity_ids.size() > 1) {
        ImGui::Text("Aircraft:");
        for (std::size_t i = 0; i < impl_->replay.entity_ids.size(); ++i) {
            if (i > 0) ImGui::SameLine();
            char buf[32];
            std::snprintf(buf, sizeof(buf), "##ent%zu", i);
            const bool selected = (i == impl_->replay.focused_entity_index);
            if (selected) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.6f, 0.9f, 1.0f));
            // Find the callsign for this entity
            std::string label = std::to_string(impl_->replay.entity_ids[i]);
            for (const auto& s : rec.snapshots()) {
                if (s.entity_id == impl_->replay.entity_ids[i] && !s.callsign.empty()) {
                    label = s.callsign;
                    break;
                }
            }
            if (ImGui::SmallButton((label + buf).c_str())) {
                impl_->replay.focused_entity_index = i;
                fit_replay_camera(impl_->replay,
                                  static_cast<float>(impl_->window_w),
                                  static_cast<float>(impl_->window_h),
                                  impl_->replay_cam_x,
                                  impl_->replay_cam_y,
                                  impl_->replay_cam_zoom);
            }
            if (selected) ImGui::PopStyleColor();
        }
        ImGui::Separator();
    }

    // Identity
    ImGui::Text("Callsign: %s", snap->callsign.c_str());
    ImGui::Text("Entity ID: %llu", static_cast<unsigned long long>(snap->entity_id));

    ImGui::Separator();

    // AI brain state
    if (ImGui::CollapsingHeader("AI Brain", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Mode:  %s", snap->ai_mode.c_str());
        ImGui::Text("State: %s", snap->ai_state.c_str());
        if (!snap->ai_event.empty()) {
            ImGui::Text("Event: %s", snap->ai_event.c_str());
        }
        if (!snap->ai_guard_result.empty()) {
            ImGui::Text("Guard: %s", snap->ai_guard_result.c_str());
        }
    }

    // Kinematics
    if (ImGui::CollapsingHeader("Kinematics", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Alt MSL:  %.0f ft", snap->altitude_msl_ft);
        ImGui::Text("Alt AGL:  %.0f ft", snap->altitude_agl_ft);
        ImGui::Text("VCAS:     %.0f kts", snap->vcas_kts);
        ImGui::Text("GS:       %.0f kts", snap->gs_kts);
        ImGui::Text("Mach:     %.2f", snap->mach);
        ImGui::Text("Heading:  %.1f deg", snap->heading_deg());
        ImGui::Text("Pitch:    %.1f deg", snap->pitch_deg());
        ImGui::Text("Roll:     %.1f deg", snap->roll_deg());
        ImGui::Text("On grnd:  %s", snap->on_ground ? "YES" : "no");
        ImGui::Text("G-load:   %.2f", snap->nz);
    }

    // Controls
    if (ImGui::CollapsingHeader("Controls")) {
        ImGui::Text("Throttle:   %.2f", snap->throttle_cmd);
        ImGui::Text("Pitch cmd:  %.2f", snap->pitch_cmd);
        ImGui::Text("Roll cmd:   %.2f", snap->roll_cmd);
        ImGui::Text("Yaw cmd:    %.2f", snap->yaw_cmd);
        ImGui::Text("Speed brk:  %.2f", snap->speed_brake_cmd);
        ImGui::Text("Gear down:  %s", snap->gear_handle_down ? "YES" : "no");
        ImGui::Text("Brakes:     %s", snap->wheel_brakes ? "YES" : "no");
        ImGui::Text("Afterburn:  %s", snap->afterburner_lit ? "YES" : "no");
        ImGui::Text("Engine RPM: %.2f", snap->engine_rpm);
        ImGui::Text("Fuel:       %.0f lbs", snap->fuel_lbs);
    }

    // Path tracking
    if (ImGui::CollapsingHeader("Path Tracking", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Target: %s", snap->target_description.c_str());
        ImGui::Text("  pos: (%.0f, %.0f, %.0f)",
                    snap->target_position.x,
                    snap->target_position.y,
                    snap->target_position.z);
        ImGui::Text("Cross-track err: %.1f ft", snap->cross_track_error_ft);
        ImGui::Text("Along-track err: %.1f ft", snap->along_track_error_ft);
        ImGui::Text("Vertical err:    %.1f ft", snap->vertical_error_ft);

        // Cross-track error bar
        const double xte_max = 200.0;
        const double xte_frac = std::min(1.0, std::abs(snap->cross_track_error_ft) / xte_max);
        ImGui::Text("XTE bar:");
        ImGui::SameLine();
        const ImVec2 bar_pos = ImGui::GetCursorScreenPos();
        const ImVec2 bar_size = ImVec2(200, 14);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(bar_pos,
                          ImVec2(bar_pos.x + bar_size.x, bar_pos.y + bar_size.y),
                          IM_COL32(40, 40, 40, 255));
        // Green→yellow→red gradient by fraction
        ImU32 bar_color;
        if (xte_frac < 0.5) {
            // Green to yellow
            const float t = static_cast<float>(xte_frac / 0.5);
            bar_color = IM_COL32(60 + static_cast<int>(t * 195),
                                   220 - static_cast<int>(t * 40),
                                   60, 255);
        } else {
            // Yellow to red
            const float t = static_cast<float>((xte_frac - 0.5) / 0.5);
            bar_color = IM_COL32(255,
                                   180 - static_cast<int>(t * 120),
                                   60 - static_cast<int>(t * 60), 255);
        }
        dl->AddRectFilled(bar_pos,
                          ImVec2(bar_pos.x + bar_size.x * static_cast<float>(xte_frac),
                                 bar_pos.y + bar_size.y),
                          bar_color);
        dl->AddRect(bar_pos,
                    ImVec2(bar_pos.x + bar_size.x, bar_pos.y + bar_size.y),
                    IM_COL32(180, 180, 180, 255));
        ImGui::Dummy(bar_size);
    }

    ImGui::Separator();

    // View toggles
    if (ImGui::CollapsingHeader("View", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Trail", &impl_->replay.show_trail);
        ImGui::Checkbox("Intended path", &impl_->replay.show_intended_path);
        ImGui::Checkbox("Target marker", &impl_->replay.show_target_marker);
        ImGui::Checkbox("AI labels", &impl_->replay.show_ai_labels);
        ImGui::Checkbox("Kinematics HUD", &impl_->replay.show_kinematics_hud);
        if (ImGui::Button("Fit to trail [F]")) {
            fit_replay_camera(impl_->replay,
                              static_cast<float>(impl_->window_w),
                              static_cast<float>(impl_->window_h),
                              impl_->replay_cam_x,
                              impl_->replay_cam_y,
                              impl_->replay_cam_zoom);
        }
    }

    ImGui::End();
}

} // namespace f4::viewer
