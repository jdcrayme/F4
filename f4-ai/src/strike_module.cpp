// f4-ai/src/strike_module.cpp — the release-trigger decision (a CCIP-style
// predicted-impact-point gate + the salvo state machine).

#include "f4/ai/modules/strike_module.hpp"

#include <cmath>
#include <algorithm>

namespace f4::ai::modules {

void StrikeModule::update(double dt, const flight::IAircraftState* state,
                          const geo::WorldPosition& aim, bool aim_valid) {
    pulse_ = false;

    if (target_id_ == 0 || state == nullptr) return;

    // Target died or became unresolvable mid-stick: abort (delivered —
    // the brain will not re-arm for this target).
    if (!aim_valid) {
        if (since_release_s >= 0.0 || salvo_fired_ > 0) {
            delivered_ = true;
            armed_ = false;
        }
        return;
    }

    // A fresh stick (first armed update for this target — set_target()
    // already reset the counters; this only flips armed so the very first
    // update can release immediately if already in the envelope).
    if (!armed_) {
        armed_ = true;
    }

    // Stick complete.
    if (delivered_) return;

    // Stick pacing: hold between releases. (The accumulated-seconds
    // compare carries a small epsilon: repeated dt addition drifts a few
    // ULPs, and a 0.5 s interval at 60 Hz must fire on tick 30, not 31.)
    if (since_release_s >= 0.0) {
        since_release_s += dt;
        if (since_release_s + 1.0e-9 < config.salvo_interval_s) return;
    }

    // --- Solve the release geometry for THIS tick --------------------------
    // Fall time from the QUADRATIC, not the level-release vacuum form: a
    // descending aircraft hands the bomb an initial downward velocity, and
    // the level form overestimates the fall time by ~10% at a 2,500 fpm
    // sink — the first TestCamp stick landed ~1,000 ft short of exactly
    // this error. w = sink rate (ft/s, positive DOWN).
    const double dx = state->position_east_ft() - aim.x;
    const double dy = state->position_north_ft() - aim.y;
    const double dist = std::sqrt(dx * dx + dy * dy);
    const double dz = state->altitude_msl_ft() - aim.z;

    if (dz < config.min_release_agl_ft) {
        computed_range_ft_ = 0.0;   // too low — the trigger disarms
        predicted_miss_ft_ = dist;
        return;
    }
    const double w = std::max(0.0, -state->vertical_speed_fpm() / 60.0);
    // 0.5*g*t^2 + w*t - dz = 0  =>  t = (-w + sqrt(w^2 + 2*g*dz)) / g
    // (the -w: an initial downward velocity SHORTENS the fall).
    const double fall_time =
        (-w + std::sqrt(w * w + 2.0 * Config::kGravityFps2 * dz)) /
        Config::kGravityFps2;

    // The throw: horizontal distance the bomb flies from release, along
    // the aircraft's velocity vector.
    computed_range_ft_ = state->ground_speed_fps() * fall_time
                       * config.drag_factor;

    // --- CCIP: the predicted impact point -----------------------------------
    // The bomb lands WHERE THE AIRCRAFT IS HEADED (plus drag), not toward
    // the aim — an aircraft 8,000 ft from the target mid-turn has the
    // target inside its THROW range but pointed 60 deg off, and the bomb
    // sails 7,000 ft wide (the first TestCamp A-G QC run's other failure
    // mode). So the gate is the PREDICTED IMPACT POINT's distance to the
    // aim: release only when the pipper is on the target.
    const double hdg = state->heading_rad();
    const double ip_x = state->position_east_ft() +
                        std::sin(hdg) * computed_range_ft_;
    const double ip_y = state->position_north_ft() +
                        std::cos(hdg) * computed_range_ft_;
    const double ip_miss_x = ip_x - aim.x;
    const double ip_miss_y = ip_y - aim.y;
    predicted_miss_ft_ = std::sqrt(ip_miss_x * ip_miss_x +
                                   ip_miss_y * ip_miss_y);

    // The release gate. The FIRST bomb of a stick needs the pipper on the
    // target (CCIP); the rest of the stick is COMMITTED — real doctrine
    // drops the stick at fixed intervals once the pickle is pressed, and
    // re-gating each bomb mid-stick would abort the second the pipper
    // wanders (it walks THROUGH the target as the aircraft closes).
    // Sanity ring: never start a stick wildly beyond the throw range.
    if (since_release_s < 0.0) {
        const bool pipper_on = predicted_miss_ft_ <= config.impact_tolerance_ft;
        const bool in_range = dist <= 2.0 * computed_range_ft_;
        if (!pipper_on || !in_range || config.hold_fire) return;
    }

    pulse_ = true;
    ++salvo_fired_;
    since_release_s = 0.0;
    if (salvo_fired_ >= config.salvo_max) {
        delivered_ = true;
        armed_ = false;
    }
}

} // namespace f4::ai::modules
