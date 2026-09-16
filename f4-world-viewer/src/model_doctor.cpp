// f4-world-viewer/src/model_doctor.cpp
//
// ANIM-DOCTOR: the Class Table Browser's animation doctor section
// (Docs/AIRCRAFT_ANIMATION_PLAN.md §5.3 / M2 exit criteria).
//
// A hands-on actuation surface for the animation pipeline, attached to
// the browser's 3D model preview:
//   - lists every channel bound on the previewed model (from the
//     RuntimeModel's anim map — the converter's output);
//   - sliders for DOF channels, checkboxes for switch channels,
//     writing into the browser's scratch AnimValues — the same state
//     the preview's animated draw path reads;
//   - a scripted gear cycle that runs the f4-anim gear sequencer —
//     the M2 acceptance test (doors first, legs second, visibility
//     hysteresis) without touching a flight model.
//
// Editing rules: the panel is a doctor tool, not a gameplay surface.
// It only touches the browser's scratch channel state; the class table
// and the model files are read-only here.

#include <f4/viewer/class_table_browser.hpp>

#include <f4/anim/channels.hpp>
#include <f4/anim/rig.hpp>
#include <f4/gltf/anim_map.hpp>
#include <f4/renderer/runtime_model_cache.hpp>

#include <imgui.h>

#include <cmath>
#include <string>
#include <vector>

namespace f4::viewer {

namespace {

/// Doctor-local gear-cycle playback state (app-wide, one at a time —
/// the panel shows the previewed model only).
struct GearCycleState {
    bool playing = false;
    double t_seconds = 0.0;
    float gear_pos = 1.0f;   // start parked (down)
    int direction = -1;      // start by retracting
};
GearCycleState g_cycle;

/// Heuristic display range per channel group. Rotation channels get a
/// ±180° slider (values stay radians internally); switch channels get
/// a 0/1 checkbox; translators get a ±10 ft slider.
struct SliderSpec {
    bool is_switch = false;
    float min = -3.14159f, max = 3.14159f;
    float deg_per_unit = 1.0f;   // display factor
    const char* unit = " rad";
};

SliderSpec slider_spec_for(f4::anim::Channel c) {
    using Channel = f4::anim::Channel;
    const auto id = static_cast<uint16_t>(c);
    SliderSpec s;

    // Switch channels: every name starting "sw." or "light.".
    const char* name = f4::anim::channel_name(c);
    if (name[0] == 's' && name[1] == 'w' && name[2] == '.') {
        s.is_switch = true;
        return s;
    }
    if (id >= static_cast<uint16_t>(Channel::light_nav) &&
        id <= static_cast<uint16_t>(Channel::light_landing)) {
        s.is_switch = true;
        return s;
    }
    // Translators: strut compression (feet).
    if (id >= static_cast<uint16_t>(Channel::gear_strut_0) &&
        id <= static_cast<uint16_t>(Channel::gear_strut_7)) {
        s.min = -10.0f;
        s.max = 10.0f;
        s.unit = " ft";
        return s;
    }
    // Everything else is a rotation in radians — display degrees.
    s.deg_per_unit = 57.29577951308232f;
    s.unit = " deg";
    return s;
}

} // namespace

void ClassTableBrowser::draw_animation_doctor(
    const f4::renderer::RuntimeModel& model) {
    using namespace f4::anim;

    auto& values = doctor_anim_;

    ImGui::TextDisabled("%d animation node(s), %d channel(s)",
                        static_cast<int>(model.anim_map.nodes.size()),
                        static_cast<int>(model.anim_map.channels().size()));

    // ── Presets ─────────────────────────────────────────────────────────
    if (ImGui::Button("Parked defaults")) {
        values.set_parked_defaults();
    }
    ImGui::SameLine();
    if (ImGui::Button("All zero")) {
        values.reset();
    }
    ImGui::Separator();

    // ── Gear cycle (the M2 acceptance script) ───────────────────────────
    ImGui::Text("Gear cycle");
    if (ImGui::Button(g_cycle.playing ? "Pause" : "Play")) {
        g_cycle.playing = !g_cycle.playing;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset to down")) {
        g_cycle.playing = false;
        g_cycle.gear_pos = 1.0f;
        g_cycle.t_seconds = 0.0;
    }
    ImGui::SameLine();
    ImGui::Text("gear_pos %.2f", g_cycle.gear_pos);
    ImGui::SliderFloat("##gearpos", &g_cycle.gear_pos, 0.0f, 1.0f, "%.2f");

    GearStationParams params;
    params.num_gear = 3;
    for (uint16_t i = 0; i < 3; ++i) {
        // Doctor defaults — real per-airframe ranges come from the
        // airframe's auxaero data via the rig (M3).
        params.leg_range_rad[i] = 40.0f * 0.017453292519943295f;
        params.door_range_rad[i] = 90.0f * 0.017453292519943295f;
    }
    if (g_cycle.playing) {
        const double dt = 1.0 / 60.0;   // panel refresh — good enough for a doctor tool
        g_cycle.t_seconds += dt;
        constexpr float kGearRate = 0.3f;   // FreeFalcon airframe.cpp:883 (per second)
        g_cycle.gear_pos = std::clamp(
            g_cycle.gear_pos + g_cycle.direction * kGearRate *
                static_cast<float>(dt), 0.0f, 1.0f);
        if (g_cycle.gear_pos <= 0.0f) g_cycle.direction = 1;
        if (g_cycle.gear_pos >= 1.0f) g_cycle.direction = -1;
    }
    {
        const auto cmd = eval_gear(g_cycle.gear_pos, nullptr, params);
        if (ImGui::Button("Apply gear sequence")) {
            apply_gear_command(cmd, values);
        }
        if (g_cycle.playing) apply_gear_command(cmd, values);
    }
    ImGui::Separator();

    // ── Channel list ────────────────────────────────────────────────────
    ImGui::Text("Channels");
    for (const auto& chan : model.anim_map.channels()) {
        const auto c = channel_from_name(chan);
        if (!c) {
            // Unknown channel name — show it read-only.
            ImGui::TextDisabled("? %s (not in f4-anim vocabulary)", chan.c_str());
            continue;
        }
        const auto spec = slider_spec_for(*c);
        float v = values[*c];
        if (spec.is_switch) {
            const bool on = f4::gltf::switch_mask_visible(v, 0, false);
            bool set = on;
            if (ImGui::Checkbox(chan.c_str(), &set)) {
                values[*c] = set ? 1.0f : 0.0f;
            }
        } else {
            // Display degrees for rotations, raw for translators.
            const float disp_min = spec.min * spec.deg_per_unit;
            const float disp_max = spec.max * spec.deg_per_unit;
            float disp = v * spec.deg_per_unit;
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::SliderFloat(chan.c_str(), &disp, disp_min, disp_max,
                                   "%.1f%s", ImGuiSliderFlags_Logarithmic)) {
                values[*c] = disp / spec.deg_per_unit;
            }
        }
    }
}

} // namespace f4::viewer
