// f4-ai/src/missile_module.cpp
//
// MissileModule implementation — fire control (mengage.cpp FireControl) +
// missile defeat (mdefeat.cpp). See bvr header / missile_module.hpp for
// design notes.

#include "f4/ai/modules/missile_module.hpp"

#include <algorithm>
#include <cmath>

namespace f4::ai::modules {

namespace {

constexpr double PI = 3.14159265358979323846;

/// Any one detection source = visible (SensorFusion::can_see's rule,
/// inlined so this module does not depend on sensor_fusion.hpp — the
/// module layer consumes TargetInfo snapshots only).
[[nodiscard]] inline bool can_see(const TargetInfo& t) noexcept {
    return t.detected_by_radar || t.detected_by_rwr ||
           t.detected_by_visual || t.detected_by_gci;
}

} // anonymous namespace

// ============================================================================
// Role 1 — fire control
// ============================================================================

double MissileModule::compute_pk(const TargetInfo& t) const {
    const double r = t.range_nm;
    const double r_min = cfg_.min_pk_range_nm;
    const double r_max = cfg_.max_pk_range_nm;
    if (r_max <= r_min) return 0.0;                 // degenerate envelope
    if (r < r_min || r > r_max) return 0.0;          // out of envelope

    // Range factor: 1.0 at the minimum employment range (the shot cannot
    // miss), decaying linearly to 0.25 at the boundary (a max-range shot
    // still has a fighting chance but is never doctrine-automatic).
    const double fr = (r_max - r) / (r_max - r_min);
    const double range_factor = 0.25 + 0.75 * std::clamp(fr, 0.0, 1.0);

    // Aspect factor from the target's Aspect Tail Angle: tail-on (ATA ~ pi,
    // target running) is the best radar-missile shot — the missile
    // overtakes a target that cannot open the range. Nose-on (ATA ~ 0)
    // compresses the flyout against high closure.
    const double aspect_factor = 0.7 + 0.3 * (t.ata_rad / PI);

    return std::clamp(cfg_.pk_base * range_factor * aspect_factor, 0.0, 1.0);
}

bool MissileModule::should_fire(const TargetInfo& t) const {
    if (!t.is_hostile) return false;
    if (!can_see(t)) return false;
    if (t.is_missile) return false;                  // guns/defeat territory
    // WEAPONS-GRADE PICTURE: RWR alone is a bearing warning, not a track
    // you can put a missile on — FreeFalcon's digi fires off radar/GCI
    // quality only (dlogic.cpp fire gating). The M3 radar-backed policy
    // sets detected_by_radar for a live track; a bandit that only knows it
    // is LOCKED may crank and defend but must not launch blind.
    if (!t.detected_by_radar && !t.detected_by_gci) return false;
    if (cooldown_ > 0.0) return false;
    if (shots_ >= cfg_.shoot_shoot_max_shots) return false;
    return compute_pk(t) >= cfg_.shoot_shoot_threshold;
}

void MissileModule::note_fired() {
    cooldown_ = cfg_.fire_cooldown_sec;
    ++shots_;
}

void MissileModule::tick_cooldown(double dt) {
    if (cooldown_ > 0.0) cooldown_ = std::max(0.0, cooldown_ - dt);
}

void MissileModule::reset_engagement() {
    shots_ = 0;
    // cooldown_ deliberately survives: it is the shooter's launch cadence
    // (4 s between missiles off the rail), not per-target state.
}

// ============================================================================
// Role 2 — missile defeat
// ============================================================================

AirSteering::Input MissileModule::steering_input(
    const flight::IAircraftState& s) const noexcept {
    AirSteering::Input in;
    in.position = geo::WorldPosition(s.position_east_ft(),
                                     s.position_north_ft(),
                                     s.altitude_msl_ft());
    in.heading_rad = s.heading_rad();
    in.pitch_rad = s.pitch_angle_rad();
    in.roll_rad = s.roll_angle_rad();
    in.roll_rate_radps = s.roll_rate_radps();
    in.pitch_rate_radps = s.pitch_rate_radps();
    in.vs_fpm = s.vertical_speed_fpm();
    in.vcas_kts = s.vcas_kts();
    in.alt_msl_ft = s.altitude_msl_ft();
    return in;
}

AIControlOutput MissileModule::update(double dt,
                                      const flight::IAircraftState* state,
                                      const TargetInfo* incoming) {
    AIControlOutput out{};

    // No state to steer with: pass through (also clears intents).
    if (!state) {
        defeating_ = false;
        chaff_ = false;
        flare_ = false;
        incoming_id_ = 0;
        linger_timer_ = 0.0;
        return out;
    }

    // Threat validity: a hostile missile we can actually see. The
    // shooter's own missile is excluded upstream (hostile-only
    // missile_threat), but a brain wiring change must not be able to make
    // us defend against ourselves — double-check here.
    const bool threat_valid = incoming != nullptr && incoming->is_hostile &&
                              incoming->is_missile && can_see(*incoming);

    if (!threat_valid) {
        incoming_id_ = 0;
        if (!defeating_) {
            chaff_ = false;
            flare_ = false;
            return out;
        }
        // Linger: the missile went terminal (the detonation sweep destroys
        // the entity — the picture goes empty the tick the fuze fires) —
        // finish the jink on the last-known geometry before releasing the
        // override, otherwise the aircraft snaps back wings-level inside
        // the lethal radius. Countermeasure intents go false with the
        // picture; the maneuver persists for the linger window.
        linger_timer_ -= dt;
        if (linger_timer_ <= 0.0) {
            defeating_ = false;
            chaff_ = false;
            flare_ = false;
            beam_heading_rad_ = 0.0;
            return out;
        }
        chaff_ = false;
        flare_ = false;
        air_steering_.throttle_max = 1.5;
        air_steering_.throttle_min = 0.9;
        out = air_steering_.steer(beam_heading_rad_, defeat_alt_ft_, 550.0,
                                  steering_input(*state));
        out.has_override = true;
        return out;
    }

    // --- A live threat: (re)arm the linger window + entry capture --------
    linger_timer_ = cfg_.defeat_linger_sec;
    incoming_id_ = incoming->entity_id;
    if (!defeating_) {
        defeating_ = true;
        defeat_alt_ft_ = state->altitude_msl_ft();
    }

    // --- Steering: the beam (notch) maneuver -------------------------------
    // Desired heading = threat bearing +/- 90 deg — put the missile on the
    // 3/9 line. Side selection: the NEARER beam (whichever of +/- 90 deg
    // requires the smaller turn from the current heading) — a real pilot
    // rolls the shortest way to the beam, and so does mdefeat.cpp's
    // BreakTurn.
    const geo::WorldPosition own_pos{
        state->position_east_ft(), state->position_north_ft(),
        state->altitude_msl_ft()};
    const double dx = incoming->position.x - own_pos.x;  // east, own->threat
    const double dy = incoming->position.y - own_pos.y;  // north, own->threat
    const double threat_bearing = (dx == 0.0 && dy == 0.0)
        ? state->heading_rad()
        : std::atan2(dx, dy);

    auto heading_error = [](double desired, double current) {
        double e = desired - current;
        while (e > PI) e -= 2.0 * PI;
        while (e < -PI) e += 2.0 * PI;
        return e;
    };

    const double beam_r = threat_bearing + PI / 2.0;
    const double beam_l = threat_bearing - PI / 2.0;
    beam_heading_rad_ =
        (std::fabs(heading_error(beam_r, state->heading_rad())) <=
         std::fabs(heading_error(beam_l, state->heading_rad())))
            ? beam_r : beam_l;

    // Outrun: full afterburner (throttle_max lifted to 1.5 = AB; the
    // AIControlOutput contract allows [0, 1.5]). Speed target above what a
    // loaded F-16 sustains so the PI parks at the rail.
    air_steering_.throttle_max = 1.5;
    air_steering_.throttle_min = 0.9;
    out = air_steering_.steer(beam_heading_rad_, defeat_alt_ft_, 550.0,
                              steering_input(*state));

    // Countermeasure intents. Guidance class is not in the RWR Launch
    // warning yet (M4: the policy can pass the missile's WeaponClassRecord
    // through) — radar-guided is the correct default for a BVR missile.
    chaff_ = incoming->range_nm >= cfg_.chaff_min_range_nm;
    flare_ = incoming->range_nm <= cfg_.ir_envelope_nm;

    // DEFENSIVE OVERRIDE: while defeating, this output preempts every other
    // module (the brain's priority ladder, AI_IMPLEMENTATION_PLAN §5
    // Step 12: collision/ground avoid > missile defeat > BVR > mission).
    out.has_override = true;
    return out;
}

} // namespace f4::ai::modules
