// f4-anim/src/rig.cpp
//
// Gear sequencer + blink evaluator implementations. See rig.hpp for the
// FreeFalcon source references every formula is ported from.

#include <f4/anim/rig.hpp>

namespace f4::anim {

namespace {

constexpr float kTwoPi = 6.283185307179586f;

/// clamp(v, lo, hi)
float clampf(float v, float lo, float hi) noexcept {
    return v < lo ? lo : (v > hi ? hi : v);
}

/// xorshift32 — deterministic, dependency-free phase scrambler. Small
/// seed differences must avalanche: FreeFalcon's `rand() % period`
/// de-sync relied on rand() high-bit entropy, and a single xorshift
/// round is linear (seed 2n produces exactly 2·xorshift(n)), which
/// made adjacent seeds blink nearly in phase. Iterating the generator
/// 8 rounds breaks that correlation.
uint32_t xorshift32(uint32_t x) noexcept {
    // Mix in a golden-ratio constant first so the seed 0 isn't a fixed
    // point, then avalanche.
    x ^= 0x9E3779B9u;
    for (int i = 0; i < 8; ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
    }
    return x;
}

} // namespace

// ── Gear sequencer ────────────────────────────────────────────────────────

GearCommand eval_gear(float gear_pos,
                      const GearStationFlags* flags,
                      const GearStationParams& params) noexcept {
    GearCommand cmd{};

    const float pos = clampf(gear_pos, 0.0f, 1.0f);
    // FreeFalcon surface.cpp:1689-1707:
    //   doors run during the FIRST half of gearPos   (pos * 2, clamped)
    //   legs  run during the SECOND half             ((pos - .5) * 2, clamped)
    const float door_frac = clampf(pos * 2.0f, 0.0f, 1.0f);
    const float leg_frac = clampf((pos - 0.5f) * 2.0f, 0.0f, 1.0f);

    const uint16_t n = params.num_gear > kGearStations
                           ? kGearStations : params.num_gear;

    for (uint16_t i = 0; i < kGearStations; ++i) {
        const bool station_exists = i < n;
        const GearStationFlags f =
            (flags && station_exists) ? flags[i] : GearStationFlags{};

        // ── Door angle ────────────────────────────────────────────────
        // surface.cpp:1691-1703: doors driven at pos*2 unless stuck or
        // broken, in which case they park at full range.
        const bool door_parked =
            !station_exists || f.door_stuck || f.door_broken;
        cmd.door_pos[i] = door_parked
            ? params.door_range_rad[i]
            : door_frac * params.door_range_rad[i];

        // ── Leg angle ─────────────────────────────────────────────────
        // surface.cpp:1708-1722: legs driven at (pos-0.5)*2 unless stuck
        // or broken, in which case they park at range * 0.6.
        const bool leg_parked =
            !station_exists || f.gear_stuck || f.gear_broken;
        cmd.leg_pos[i] = leg_parked
            ? params.leg_range_rad[i] * params.broken_clamp
            : leg_frac * params.leg_range_rad[i];

        // ── Visibility hysteresis ─────────────────────────────────────
        // surface.cpp:1724-1757: the show/hide switches are derived
        // from the DOF values with a 5° threshold, with the broken/
        // stuck overrides forcing their variants on at gearPos >= 0.9.
        const float thr = params.show_threshold_rad;

        if (station_exists && pos >= 0.9f &&
            (f.door_broken || f.door_stuck || f.gear_broken || f.gear_stuck)) {
            if (f.door_broken || f.door_stuck) {
                cmd.door_visible |= (1u << i);
                cmd.hole_visible |= (1u << i);
            }
            if (f.gear_broken || f.gear_stuck) {
                cmd.leg_visible |= (1u << i);
            }
            cmd.broken_visible |= (1u << i);
        } else {
            // Doors: shown while the door DOF is open past threshold.
            if (cmd.door_pos[i] >= thr) cmd.door_visible |= (1u << i);
            // Bay hole: shown while the door DOF is open past threshold
            // (the hole must never be visible with the door shut).
            if (cmd.door_pos[i] > thr) cmd.hole_visible |= (1u << i);
            // Legs: shown once extended past threshold.
            if (cmd.leg_pos[i] > thr) cmd.leg_visible |= (1u << i);
        }
    }

    return cmd;
}

void apply_gear_command(const GearCommand& cmd, AnimValues& out) noexcept {
    for (uint16_t i = 0; i < kGearStations; ++i) {
        out[static_cast<Channel>(
                static_cast<uint16_t>(Channel::gear_leg_pos_0) + i)] =
            cmd.leg_pos[i];
        out[static_cast<Channel>(
                static_cast<uint16_t>(Channel::gear_door_pos_0) + i)] =
            cmd.door_pos[i];
        out[static_cast<Channel>(
                static_cast<uint16_t>(Channel::sw_gear_leg_0) + i)] =
            static_cast<float>((cmd.leg_visible >> i) & 1u);
        out[static_cast<Channel>(
                static_cast<uint16_t>(Channel::sw_gear_door_0) + i)] =
            static_cast<float>((cmd.door_visible >> i) & 1u);
        out[static_cast<Channel>(
                static_cast<uint16_t>(Channel::sw_gear_hole_0) + i)] =
            static_cast<float>((cmd.hole_visible >> i) & 1u);
        out[static_cast<Channel>(
                static_cast<uint16_t>(Channel::sw_gear_broken_0) + i)] =
            static_cast<float>((cmd.broken_visible >> i) & 1u);
    }
}

// ── Blink patterns ────────────────────────────────────────────────────────

float eval_blink(const BlinkPattern& p, double t_seconds) noexcept {
    const float period = p.on_seconds + p.off_seconds;
    if (period <= 0.0f) return 1.0f;

    // Deterministic phase from the seed: map the avalanched seed into
    // [0, period). FreeFalcon's `rand() % FlashOff` de-sync becomes a
    // pure function of the seed.
    const uint32_t scrambled = xorshift32(p.seed);
    const float phase =
        (static_cast<float>(scrambled >> 8) / 16777216.0f) * period;

    // Position within the current cycle.
    double tt = std::fmod(t_seconds + phase, static_cast<double>(period));
    if (tt < 0.0) tt += static_cast<double>(period);

    return (static_cast<float>(tt) < p.on_seconds) ? 1.0f : 0.0f;
}

float integrate_spin(float current_angle_rad, float rate_rad_per_sec,
                     double dt_seconds) noexcept {
    float angle = current_angle_rad + rate_rad_per_sec *
                                       static_cast<float>(dt_seconds);
    // Wrap to [0, 2π) the same way FreeFalcon fmods WheelAngle
    // (gear.cpp:105-107). std::fmod keeps the sign of the dividend, so
    // negative results get one more 2π added.
    angle = std::fmod(angle, kTwoPi);
    if (angle < 0.0f) angle += kTwoPi;
    return angle;
}

// ── Spinners ──────────────────────────────────────────────────────────────

void seed_spinners(uint32_t seed, AnimValues& inout) noexcept {
    // One avalanche per channel so the three phases de-correlate.
    inout[Channel::rotor_main] =
        static_cast<float>(xorshift32(seed ^ 0x1u)) / 4294967296.0f * kTwoPi;
    inout[Channel::rotor_tail] =
        static_cast<float>(xorshift32(seed ^ 0x9E37u)) / 4294967296.0f * kTwoPi;
    inout[Channel::radar_dish_spin] =
        static_cast<float>(xorshift32(seed ^ 0x51EDu)) / 4294967296.0f * kTwoPi;
}

// ── Pilot surfaces ────────────────────────────────────────────────────────

void apply_surface_command(const SurfaceCommand& cmd,
                           AnimValues& inout) noexcept {
    // Stabilators: symmetric — both sides follow pitch stick. Pull
    // (positive stick) = trailing edge up.
    inout[Channel::stab_l] = -cmd.pitch_stick * kStabMaxRad;
    inout[Channel::stab_r] = -cmd.pitch_stick * kStabMaxRad;

    // Flaperons: TEF schedule is COMMON (droop together), roll is
    // DIFFERENTIAL (right roll → left TE up, right TE down).
    const float tef = cmd.tef * kFlapMaxRad;
    const float roll = cmd.roll_stick * kFlapRollMaxRad;
    inout[Channel::flap_l] = tef + roll;
    inout[Channel::flap_r] = tef - roll;

    // Leading-edge flaps: symmetric, follow the schedule.
    inout[Channel::lef_l] = cmd.lef * kLefMaxRad;
    inout[Channel::lef_r] = cmd.lef * kLefMaxRad;

    inout[Channel::rudder] = cmd.yaw_pedal * kRudderMaxRad;

    // Speed brake: all four panels together.
    const float brake = cmd.brake * kBrakeMaxRad;
    inout[Channel::airbrake_top_l] = brake;
    inout[Channel::airbrake_bot_l] = brake;
    inout[Channel::airbrake_top_r] = brake;
    inout[Channel::airbrake_bot_r] = brake;

    inout[Channel::hook] = cmd.hook * kHookMaxRad;
    inout[Channel::dragchute] = cmd.chute * kChuteMaxRad;

    // Engine presentation.
    inout[Channel::nozzle_pos] = cmd.nozzle;
    inout[Channel::sw_ab] = cmd.afterburner ? 1.0f : 0.0f;
    inout[Channel::ab_scale] = cmd.afterburner ? 1.0f : 0.0f;
    inout[Channel::rpm] = cmd.rpm;
}

} // namespace f4::anim
