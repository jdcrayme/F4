// f4-ai/src/strike_module.cpp — the release-trigger decision (a CCIP-style
// predicted-impact-point gate + the salvo state machine).

#include "f4/ai/modules/strike_module.hpp"

#include <cmath>
#include <algorithm>

namespace f4::ai::modules {

void StrikeModule::update(double dt, const flight::IAircraftState* state,
                          const geo::WorldPosition& aim, bool aim_valid) {
    pulse_ = false;
    last_aim_ = aim;
    last_aim_valid_ = aim_valid;

    if (target_id_ == 0 || state == nullptr) return;

    // --- EMPL-1: track estimation (see the header's rationale) ----------
    // The throw direction is the VELOCITY over ground, differenced from
    // consecutive position samples. Nose direction is the fallback until
    // the second sample arrives (exact on a straight-in, where the two
    // coincide) and whenever the samples are stationary.
    {
        const double e = state->position_east_ft();
        const double n = state->position_north_ft();
        if (has_prev_pos_) {
            const double de = e - prev_east_ft_;
            const double dn = n - prev_north_ft_;
            const double dl = std::sqrt(de * de + dn * dn);
            if (dl > 1.0) {   // stationary samples keep the last track
                track_x_ = de / dl;
                track_y_ = dn / dl;
                has_track_ = true;
            }
        }
        prev_east_ft_ = e;
        prev_north_ft_ = n;
        has_prev_pos_ = true;
    }

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
    // The bomb lands ALONG THE AIRCRAFT'S TRACK (plus drag), not toward
    // the aim — an aircraft 8,000 ft from the target mid-turn has the
    // target inside its THROW range but pointed 60 deg off, and the bomb
    // sails 7,000 ft wide (the first TestCamp A-G QC run's other failure
    // mode). The predicted miss stays computed for diagnostics and for
    // the straight-in case, but the GATE below uses the module header's
    // own contract (range + alignment cone), not the pipper — see the
    // release-gate comment.
    const double hdg = state->heading_rad();
    const double dir_x = has_track_ ? track_x_ : std::sin(hdg);
    const double dir_y = has_track_ ? track_y_ : std::cos(hdg);
    const double ip_x = state->position_east_ft() +
                        dir_x * computed_range_ft_;
    const double ip_y = state->position_north_ft() +
                        dir_y * computed_range_ft_;
    const double ip_miss_x = ip_x - aim.x;
    const double ip_miss_y = ip_y - aim.y;
    predicted_miss_ft_ = std::sqrt(ip_miss_x * ip_miss_x +
                                   ip_miss_y * ip_miss_y);

    // The release gate. The FIRST bomb of a stick needs the target in
    // the envelope; the rest of the stick is COMMITTED — real doctrine
    // drops the stick at fixed intervals once the pickle is pressed, and
    // re-gating each bomb mid-stick would abort the second the pipper
    // wanders (it walks THROUGH the target as the aircraft closes).
    //
    // EMPL-1: the gate is the header's contract — release when the
    // horizontal distance to the aim enters the ballistic range, with
    // the aim inside a forward alignment cone. The earlier form gated on
    // the CCIP pipper (predicted miss <= 150 ft), which at the flat-world
    // campaign delivery geometry (dz ~3,000 ft -> R ~9,500 ft) demands
    // sub-0.9-deg alignment — tighter than any dynamic approach holds:
    // the LNAV run-in crossed the release point 400 ft wide, the homing
    // run-in 1,100 ft wide, and the stick never fell although the whole
    // rest of the chain (arming, target propagation, envelope) was
    // healthy. The cone keeps the 60-deg-off protection the pipper added
    // (a mid-turn aircraft's track must still POINT at the aim), while
    // the along-track timing settles for the range boundary: the stick
    // then walks along the aircraft's own track toward the aim.
    if (since_release_s < 0.0) {
        const double aim_dx = aim.x - state->position_east_ft();
        const double aim_dy = aim.y - state->position_north_ft();
        const double aim_dist = std::sqrt(aim_dx * aim_dx + aim_dy * aim_dy);
        double cos_cone = -1.0;
        if (aim_dist > 1.0) {
            cos_cone = (dir_x * aim_dx + dir_y * aim_dy) / aim_dist;
        }
        const bool aligned = cos_cone >= std::cos(config.release_cone_rad);
        const bool in_range = aim_dist <= computed_range_ft_;
        if (!aligned || !in_range || config.hold_fire) return;
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
