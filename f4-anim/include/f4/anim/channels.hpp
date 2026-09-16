// f4-anim/include/f4/anim/channels.hpp
//
// Semantic animation channels — the vocabulary shared by the simulation
// (rig), the converter (tag binding), and the renderer (application).
//
// Design (Docs/AIRCRAFT_ANIMATION_PLAN.md §3.2):
//   - FreeFalcon hard-codes DOF/switch indices per model family and
//     drives them directly from sim code (src/sim/include/dofsnswitches.h,
//     src/sim/aircraft/surface.cpp). F4 replaces the indices with named
//     channels: the converter binds each model's DOF/switch nodes to a
//     channel via the family tables (f4-import/vocab/family/*.json), and
//     the rig/sim writes channel values. A model that lacks a channel's
//     node simply doesn't animate that channel — the command is a no-op.
//   - Units mirror what FreeFalcon fed DrawableBSP::SetDOF /
//     SetSwitch (the sim-layer units):
//       * rotational DOFs    → radians
//       * translator DOFs    → feet  (the translator's own vector scales)
//       * scale DOFs         → 0..1  (1 = target scale reached)
//       * switch channels    → bitmask (bit k set = switch child k drawn;
//         FreeFalcon BSwitchNode::Draw walks children with mask >>= 1 —
//         NOT a child index. BXSwitchNode inverts the mask.)
//   - The channel enum is a fixed, additive-only contract. Names are the
//     serialized form (extras "channel" field, vocab JSON, doctor
//     output); ids are the hot-path form (AnimValues arrays).

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace f4::anim {

// ── Channel ids ───────────────────────────────────────────────────────────
//
// Grouped as in the plan: gear, control surfaces, engine/effects,
// airframe, lights. Instance suffixes (.0..7) follow the §6 grammar's
// convention for gear stations / mirrors (.l/.r for left/right parts).
// Cockpit DOFs (COMP_3DPIT_*) are deliberately NOT enumerated here —
// the range is reserved so cockpit tags can never collide with these.
enum class Channel : uint16_t {
    // ── Landing gear (complex family, 8 stations) ─────────────────────
    gear_pos = 0,            // 0 up .. 1 down (airframe gearPos)
    gear_leg_pos_0,          // leg retraction angle (rad), station 0..7
    gear_leg_pos_1,
    gear_leg_pos_2,
    gear_leg_pos_3,
    gear_leg_pos_4,
    gear_leg_pos_5,
    gear_leg_pos_6,
    gear_leg_pos_7,
    gear_door_pos_0,         // door angle (rad), station 0..7
    gear_door_pos_1,
    gear_door_pos_2,
    gear_door_pos_3,
    gear_door_pos_4,
    gear_door_pos_5,
    gear_door_pos_6,
    gear_door_pos_7,
    gear_strut_0,            // strut extension (ft, translator), station 0..7
    gear_strut_1,
    gear_strut_2,
    gear_strut_3,
    gear_strut_4,
    gear_strut_5,
    gear_strut_6,
    gear_strut_7,
    wheel_angle_0,           // wheel spin (rad, accumulates), station 0..7
    wheel_angle_1,
    wheel_angle_2,
    wheel_angle_3,
    wheel_angle_4,
    wheel_angle_5,
    wheel_angle_6,
    wheel_angle_7,
    sw_gear_leg_0,           // gear visible (bitmask), station 0..7
    sw_gear_leg_1,
    sw_gear_leg_2,
    sw_gear_leg_3,
    sw_gear_leg_4,
    sw_gear_leg_5,
    sw_gear_leg_6,
    sw_gear_leg_7,
    sw_gear_door_0,          // gear door visible, station 0..7
    sw_gear_door_1,
    sw_gear_door_2,
    sw_gear_door_3,
    sw_gear_door_4,
    sw_gear_door_5,
    sw_gear_door_6,
    sw_gear_door_7,
    sw_gear_hole_0,          // gear bay hole visible, station 0..7
    sw_gear_hole_1,
    sw_gear_hole_2,
    sw_gear_hole_3,
    sw_gear_hole_4,
    sw_gear_hole_5,
    sw_gear_hole_6,
    sw_gear_hole_7,
    sw_gear_broken_0,        // broken-gear variant visible, station 0..7
    sw_gear_broken_1,
    sw_gear_broken_2,
    sw_gear_broken_3,
    sw_gear_broken_4,
    sw_gear_broken_5,
    sw_gear_broken_6,
    sw_gear_broken_7,
    sw_nose_gear_rod,        // nose gear steering rod (COMP_NOS_GEAR_ROD)
    nose_gear_rot,           // nose wheel steering angle (COMP_NOS_GEAR_ROT)

    // ── Control surfaces ────────────────────────────────────────────────
    stab_l,                  // left stabilator (COMP_LT_STAB)
    stab_r,                  // right stabilator (COMP_RT_STAB)
    flap_l,                  // left flaperon/aileron (COMP_LT_FLAP)
    flap_r,                  // right flaperon/aileron (COMP_RT_FLAP)
    rudder,                  // COMP_RUDDER
    lef_l,                   // left leading-edge flap (COMP_LT_LEF)
    lef_r,                   // right leading-edge flap (COMP_RT_LEF)
    tef_l,                   // left trailing-edge flap (COMP_LT_TEF)
    tef_r,                   // right trailing-edge flap (COMP_RT_TEF)
    airbrake_top_l,          // COMP_LT_AIR_BRAKE_TOP
    airbrake_bot_l,          // COMP_LT_AIR_BRAKE_BOT
    airbrake_top_r,          // COMP_RT_AIR_BRAKE_TOP
    airbrake_bot_r,          // COMP_RT_AIR_BRAKE_BOT
    spoiler_l1,              // COMP_LT_SPOILER1
    spoiler_r1,              // COMP_RT_SPOILER1
    spoiler_l2,              // COMP_LT_SPOILER2
    spoiler_r2,              // COMP_RT_SPOILER2
    swing_wing,              // wing sweep (COMP_SWING_WING)

    // ── Engine / effects ────────────────────────────────────────────────
    ab_scale,                // afterburner plume scale 0..1 (COMP_ABDOF)
    ab_scale_2,              // second engine (COMP_ABDOF2)
    sw_ab,                   // afterburner plume visible (COMP_AB, bitmask)
    sw_ab_2,                 // second engine (COMP_AB2)
    nozzle_pos,              // exhaust nozzle 0..1 (COMP_EXH_NOZ)
    nozzle_pos_2,            // second engine (COMP_EXH_NOZ2)
    sw_nozzle,               // nozzle stage mask (COMP_EXH_NOZZLE, 1<<stage)
    sw_nozzle_2,             // second engine (COMP_EXH_NOZZLE2)
    effect_vapor,            // wing vapor sheet mask (COMP_WING_VAPOR)
    throttle,                // fan/needle anim (COMP_THROTTLE)
    rpm,                     // fan/needle anim (COMP_RPM)
    prop_spin,               // propeller rotation (COMP_PROPELLOR)

    // ── Airframe ────────────────────────────────────────────────────────
    canopy,                  // canopy opening (COMP_CANOPY_DOF)
    sw_canopy,               // canopy variant (COMP_CANOPY switch)
    hook,                    // tailhook angle (COMP_TAILHOOK)
    sw_hook,                 // tailhook variant (COMP_HOOK)
    dragchute,               // drag chute angle (COMP_DRAGCHUTE DOF)
    sw_dragchute,            // drag chute visible (COMP_DRAGCHUTE switch)
    refuel_probe,            // probe/boom extension (COMP_REFUEL)
    sw_refuel_door,          // refuel door open (COMP_REFUEL_DR switch)
    weapon_bay_0,            // bay door angle (COMP_WEAPON_BAY_0..4)
    weapon_bay_1,
    weapon_bay_2,
    weapon_bay_3,
    weapon_bay_4,
    sw_weapon_bay_0,         // bay door visible (COMP_WEAPON_BAY_0_SW..)
    sw_weapon_bay_1,
    sw_weapon_bay_2,
    sw_weapon_bay_3,
    sw_weapon_bay_4,
    intake_ramp_l1,          // COMP_INTAKE_1_RAMP_1..3
    intake_ramp_l2,
    intake_ramp_l3,
    intake_ramp_r1,          // COMP_INTAKE_2_RAMP_1..3
    intake_ramp_r2,
    intake_ramp_r3,

    // ── Helicopter ──────────────────────────────────────────────────────
    rotor_main,              // main rotor spin (HELI_MAIN_ROTOR)
    rotor_tail,              // tail rotor spin (HELI_TAIL_ROTOR)
    sw_rotors,               // rotor blur variants (HELI_ROTORS switch)

    // ── Lights (patterns live in the rig; channels are mask/intensity) ──
    light_nav,               // nav lights on (COMP_NAV_LIGHTS)
    light_strobe,            // tail strobe on (COMP_TAIL_STROBE)
    light_landing,           // landing lights on (COMP_LAND_LIGHTS)

    // ── Sentinel ────────────────────────────────────────────────────────
    Count
};

constexpr uint16_t kChannelCount = static_cast<uint16_t>(Channel::Count);
constexpr uint16_t kGearStations = 8;

// ── Name ↔ id ─────────────────────────────────────────────────────────────

/// The serialized name of a channel (e.g. "gear_leg_pos.0", "sw.ab").
/// Dots separate the group from the instance suffix; underscores within
/// the group name. Never nullptr; for Channel::Count returns "unknown".
const char* channel_name(Channel c) noexcept;

/// Parse a serialized channel name. Returns std::nullopt for unknown
/// names (the caller decides whether that's an error or an advisory).
std::optional<Channel> channel_from_name(std::string_view name) noexcept;

// ── Per-instance channel values ───────────────────────────────────────────

/// Per-instance animation state. Lives beside the visual model state
/// (VisualModelComponent) and is written by the rig / doctor panel,
/// read by the renderer. Values default to 0 — which for switch
/// channels means "all children hidden", matching FreeFalcon's
/// zero-initialized SwitchValues[].
///
/// Convention: a value of NaN means "no value this frame" — the
/// renderer leaves the bound node at its last applied transform. The
/// rig never writes NaN; the doctor panel may park channels there.
struct AnimValues {
    std::array<float, kChannelCount> value{};

    float& operator[](Channel c) noexcept {
        return value[static_cast<uint16_t>(c)];
    }
    float operator[](Channel c) const noexcept {
        return value[static_cast<uint16_t>(c)];
    }
    float& operator[](uint16_t i) noexcept { return value[i]; }
    float operator[](uint16_t i) const noexcept { return value[i]; }

    /// Zero everything (all switches hidden, all DOFs at 0).
    void reset() noexcept { value.fill(0.0f); }

    /// FreeFalcon-style parked-aircraft preset: gear shown, doors and
    /// holes shown, everything else off. Used by viewers staging
    /// aircraft on the ground so gear geometry doesn't vanish.
    void set_parked_defaults() noexcept;
};

// ── DOF value processing (FreeFalcon Process_DOFRot port) ────────────────

/// XDOF flag constants (FreeFalcon src/graphics/bsplib/bspnodes.cpp).
constexpr int32_t kXdofNegate   = (1 << 0);  // negate the DOF value
constexpr int32_t kXdofMinmax   = (1 << 1);  // clamp to [min, max]
constexpr int32_t kXdofSubrange = (1 << 2);  // rescale to [0, 1] over [min, max]
constexpr int32_t kXdofIsDof    = (1 << 31); // value is degrees, not radians

/// Process a raw DOF value through the XDOF flags — the canonical port
/// of FreeFalcon's Process_DOFRot(). Shared by the offline extractor
/// (f4-models), the glTF animation evaluator (f4-gltf), and the rig
/// (f4-anim) so all three can never drift apart.
///
/// @param dof_value  raw channel value (sim units: radians/feet/0..1)
/// @param flags      kXdof* flag bits from the BSP node / glTF extras
/// @param min        DOF minimum from the node
/// @param max        DOF maximum from the node
/// @param multiplier DOF multiplier from the node
/// @return processed value = (flag-adjusted value) * multiplier
float process_dof_value(float dof_value, int32_t flags,
                        float min, float max, float multiplier) noexcept;

} // namespace f4::anim
