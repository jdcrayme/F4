// f4-anim/include/f4/anim/rig.hpp
//
// Animation rig — the data-driven layer between semantic simulation
// state and animation channels (Docs/AIRCRAFT_ANIMATION_PLAN.md §5.3).
//
// FreeFalcon's presentation logic lives here ONCE, generically:
//   - GearSequencer: the doors-first / legs-second choreography from
//     src/sim/aircraft/surface.cpp RunGearSurfaces() (lines ~1684-1757),
//     including the ~5° visibility hysteresis and the broken/stuck
//     clamp at 0.6 * range. Parameterized by per-station ranges
//     (FreeFalcon's auxaeroData NosGearRng) instead of hard-coded to
//     one airframe.
//   - BlinkWave: the randomized-phase on/off patterns from
//     RunLightSurfaces() — nav (0.4 s on / 0.5 s off), strobe
//     (0.08 s on / 2.0 s off) — with a seeded phase so replays stay
//     deterministic (FreeFalcon used rand() % period).
//
// f4-anim is pure C++: no glTF, no rendering, no sim dependencies. The
// caller (f4-simulation components or the world-viewer's doctor panel)
// owns the state and time; the rig only computes channel writes.

#pragma once

#include <f4/anim/channels.hpp>

#include <cmath>
#include <cstdint>

namespace f4::anim {

// ── Gear sequencer ────────────────────────────────────────────────────────

/// Per-station gear data the sequencer needs. FreeFalcon reads these
/// from auxaeroData (readin.cpp → Data/Aircraft/*.json auxAero section);
/// stations beyond num_gear are ignored.
struct GearStationParams {
    /// Leg retraction range in radians (FreeFalcon aeroData NosGearRng
    /// + i*4 — the DOF angle the model's leg travels). 0 = no leg DOF
    /// on this station.
    float leg_range_rad[kGearStations] = {};

    /// Door range in radians (same aero slot). 0 = no door DOF.
    float door_range_rad[kGearStations] = {};

    /// Number of gear stations the airframe has (1..8).
    uint16_t num_gear = 3;

    /// Broken/stuck clamp fraction: a broken leg parks at this fraction
    /// of its range (FreeFalcon: range * 0.6f in surface.cpp:1720).
    float broken_clamp = 0.6f;

    /// Visibility hysteresis threshold as a fraction of the DOF range —
    /// FreeFalcon uses 5° (5.0F * DTR) in surface.cpp:1741-1748.
    float show_threshold_rad = 5.0f * 0.017453293f;
};

/// Per-station damage flags (FreeFalcon GearData:: flags, airframe.h).
struct GearStationFlags {
    bool door_stuck = false;
    bool door_broken = false;
    bool gear_stuck = false;
    bool gear_broken = false;
};

/// Command outputs of the gear sequencer. The caller applies these to
/// the matching channel groups (gear_leg_pos.*, gear_door_pos.*,
/// sw.gear_leg.*, ...) on the entity's AnimValues.
struct GearCommand {
    float leg_pos[kGearStations] = {};     // radians
    float door_pos[kGearStations] = {};    // radians
    uint32_t leg_visible = 0;              // bitmask, station i → bit i
    uint32_t door_visible = 0;             // bitmask, station i → bit i
    uint32_t hole_visible = 0;             // bitmask, station i → bit i
    uint32_t broken_visible = 0;           // bitmask, station i → bit i
};

/// Evaluate the gear choreography for one frame. Deterministic and
/// stateless apart from the inputs — the same (gear_pos, flags,
/// params) always produces the same command, so remote entities can
/// run the sequencer locally from replicated gear_pos.
///
/// Port of surface.cpp RunGearSurfaces():
///   door angle = clamp(gear_pos * 2, 0, 1) * door_range
///   leg angle  = clamp((gear_pos - 0.5) * 2, 0, 1) * leg_range
///   broken/stuck doors park at full range; broken/stuck legs at
///   range * broken_clamp; visibility switches follow the DOF values
///   with the 5°-equivalent hysteresis.
GearCommand eval_gear(float gear_pos,
                      const GearStationFlags* flags,   // num_gear entries, may be null
                      const GearStationParams& params) noexcept;

/// Apply a GearCommand to an AnimValues instance (writes the
/// gear_leg_pos.*/gear_door_pos.*/sw.gear_.* channel groups).
void apply_gear_command(const GearCommand& cmd, AnimValues& out) noexcept;

// ── Blink patterns ────────────────────────────────────────────────────────

/// An on/off blink pattern with a deterministic pseudo-random phase.
/// FreeFalcon's RunLightSurfaces(): nav = 0.4 s on / 0.5 s off with a
/// random start, strobe = 0.08 s on / 2.0 s off, both phase-shifted per
/// aircraft so a formation doesn't blink in unison. The random phase
/// becomes a seed here so replays and remote clients compute the
/// identical waveform from (sim_time, seed) alone.
struct BlinkPattern {
    float on_seconds = 0.4f;    ///< time the light stays on
    float off_seconds = 0.5f;   ///< time the light stays off
    uint32_t seed = 0;          ///< per-entity phase seed (0..0xFFFFFFFF)

    /// Default FreeFalcon nav-light pattern.
    static BlinkPattern nav(uint32_t seed) noexcept {
        return {0.4f, 0.5f, seed};
    }
    /// Default FreeFalcon tail-strobe pattern.
    static BlinkPattern strobe(uint32_t seed) noexcept {
        return {0.08f, 2.0f, seed};
    }
};

/// Evaluate the blink waveform at time t (seconds, sim time). Returns
/// 1.0 when on, 0.0 when off. Deterministic for (t, pattern).
///
/// The phase is derived from the seed via a xorshift step so entities
/// with different seeds de-sync, while one entity's waveform is a pure
/// function of time — scrubbing playback replays identically.
float eval_blink(const BlinkPattern& p, double t_seconds) noexcept;

/// Advance a wheel-spin / rotor integrator. Returns the new angle:
/// angle + rate * dt, wrapped to [0, 2π). FreeFalcon's RunLandingGear
/// accumulates WheelAngle from ground speed / wheel radius and wraps
/// with fmod(angle, 2π) (gear.cpp:49-110); rotors accumulate RPM the
/// same way. Wrapping keeps channel values bounded for floats.
float integrate_spin(float current_angle_rad, float rate_rad_per_sec,
                     double dt_seconds) noexcept;

// ── Spinners (continuous rotation channels) ───────────────────────────────

/// Seed the continuous-spinner channels (rotor.main, rotor.tail,
/// radar.dish_spin) with a deterministic per-entity phase derived from
/// `seed` (typically the entity id) so a formation of helicopters or a
/// base full of radar sites doesn't spin in lockstep. Same avalanche
/// idea as the blink patterns. Call once per entity before the first
/// integrate pass.
void seed_spinners(uint32_t seed, AnimValues& inout) noexcept;

/// Advance one continuous spinner: angle += rate·dt, wrapped to [0,2π).
/// rate ≤ 0 (or dt ≤ 0) leaves the value untouched — a stopped rotor or
/// a dead radar holds its last angle. Unlike the state channels (gear),
/// spinner angles have NO rest pose: the rig always owns the value.
inline void integrate_spinner(Channel c, float rate_rad_s, double dt,
                              AnimValues& inout) noexcept {
    if (rate_rad_s <= 0.0f || dt <= 0.0) return;
    inout[c] = integrate_spin(inout[c], rate_rad_s, dt);
}

// ── Pilot surfaces (FCS → visual deflection) ──────────────────────────────

/// Normalized cockpit commands and scheduled surface positions — exactly
/// the signals the flight model's FCS/aero state already computes each
/// tick (pshape / rshape / yshape / tefPos / lefPos / dbrake / ...). The
/// rig maps them onto the control-surface channels with FreeFalcon-style
/// max deflections.
struct SurfaceCommand {
    float pitch_stick = 0.0f;   // −1 (push) .. +1 (pull)
    float roll_stick  = 0.0f;   // −1 (left) .. +1 (right)
    float yaw_pedal   = 0.0f;   // −1 .. +1
    float tef         = 0.0f;   // 0..1 trailing-edge flaperon schedule
    float lef         = 0.0f;   // 0..1 leading-edge flap schedule
    float brake       = 0.0f;   // 0..1 speed brake
    float hook        = 0.0f;   // 0..1 tail hook
    float chute       = 0.0f;   // 0..1 drag chute
    float nozzle      = 0.0f;   // 0..1 exhaust nozzle
    bool  afterburner = false;  // AB lit (plume visible/scaled)
    float rpm         = 0.0f;   // 0..1+ engine RPM
};

/// Max visual deflections (radians) — presentation constants pending
/// auxaero-sourced per-airframe ranges (M3).
inline constexpr float kStabMaxRad     = 25.0f * 0.017453292519943295f;
inline constexpr float kFlapMaxRad     = 20.0f * 0.017453292519943295f;
inline constexpr float kFlapRollMaxRad = 15.0f * 0.017453292519943295f;
inline constexpr float kLefMaxRad      = 25.0f * 0.017453292519943295f;
inline constexpr float kRudderMaxRad   = 30.0f * 0.017453292519943295f;
inline constexpr float kBrakeMaxRad    = 55.0f * 0.017453292519943295f;
inline constexpr float kHookMaxRad     = 60.0f * 0.017453292519943295f;
inline constexpr float kChuteMaxRad    = 90.0f * 0.017453292519943295f;

/// Map the surface commands onto the control-surface channels:
///   stabs         — symmetric, follow pitch stick (pull = TE up)
///   flaperons     — TEF schedule common + roll differential
///   LEFs          — symmetric, follow the schedule
///   rudder        — follows pedals
///   airbrakes     — all four panels follow the brake position
///   hook / chute  — follow their positions
///   nozzle / AB   — follow the engine state
/// Signs assume the converter's node frames (verified on model 1052);
/// a model whose frames differ is corrected per-model via overrides.
void apply_surface_command(const SurfaceCommand& cmd, AnimValues& inout) noexcept;

} // namespace f4::anim
