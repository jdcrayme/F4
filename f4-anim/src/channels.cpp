// f4-anim/src/channels.cpp
//
// Channel name table + AnimValues presets + Process_DOFRot port.
// The name table order MUST match the Channel enum declaration order
// in channels.hpp exactly — a static_assert-checkable mismatch guard
// is in the tests (test_channels.cpp round-trips every id).

#include <f4/anim/channels.hpp>

#include <cmath>
#include <cstring>

namespace f4::anim {

const char* channel_name(Channel c) noexcept {
    static constexpr const char* kNames[] = {
        // gear
        "gear_pos",
        "gear_leg_pos.0", "gear_leg_pos.1", "gear_leg_pos.2", "gear_leg_pos.3",
        "gear_leg_pos.4", "gear_leg_pos.5", "gear_leg_pos.6", "gear_leg_pos.7",
        "gear_door_pos.0", "gear_door_pos.1", "gear_door_pos.2", "gear_door_pos.3",
        "gear_door_pos.4", "gear_door_pos.5", "gear_door_pos.6", "gear_door_pos.7",
        "gear_strut.0", "gear_strut.1", "gear_strut.2", "gear_strut.3",
        "gear_strut.4", "gear_strut.5", "gear_strut.6", "gear_strut.7",
        "wheel_angle.0", "wheel_angle.1", "wheel_angle.2", "wheel_angle.3",
        "wheel_angle.4", "wheel_angle.5", "wheel_angle.6", "wheel_angle.7",
        "sw.gear_leg.0", "sw.gear_leg.1", "sw.gear_leg.2", "sw.gear_leg.3",
        "sw.gear_leg.4", "sw.gear_leg.5", "sw.gear_leg.6", "sw.gear_leg.7",
        "sw.gear_door.0", "sw.gear_door.1", "sw.gear_door.2", "sw.gear_door.3",
        "sw.gear_door.4", "sw.gear_door.5", "sw.gear_door.6", "sw.gear_door.7",
        "sw.gear_hole.0", "sw.gear_hole.1", "sw.gear_hole.2", "sw.gear_hole.3",
        "sw.gear_hole.4", "sw.gear_hole.5", "sw.gear_hole.6", "sw.gear_hole.7",
        "sw.gear_broken.0", "sw.gear_broken.1", "sw.gear_broken.2", "sw.gear_broken.3",
        "sw.gear_broken.4", "sw.gear_broken.5", "sw.gear_broken.6", "sw.gear_broken.7",
        "sw.nose_gear_rod", "nose_gear_rot",
        // control surfaces
        "stab.l", "stab.r", "flap.l", "flap.r", "rudder",
        "lef.l", "lef.r", "tef.l", "tef.r",
        "airbrake.top.l", "airbrake.bot.l", "airbrake.top.r", "airbrake.bot.r",
        "spoiler.l1", "spoiler.r1", "spoiler.l2", "spoiler.r2",
        "swing_wing",
        // engine / effects
        "ab_scale", "ab_scale.2", "sw.ab", "sw.ab.2",
        "nozzle_pos", "nozzle_pos.2", "sw.nozzle", "sw.nozzle.2",
        "effect.vapor", "throttle", "rpm", "prop_spin",
        // airframe
        "canopy", "sw.canopy", "hook", "sw.hook",
        "dragchute", "sw.dragchute", "refuel_probe", "sw.refuel_door",
        "weapon_bay.0", "weapon_bay.1", "weapon_bay.2", "weapon_bay.3", "weapon_bay.4",
        "sw.weapon_bay.0", "sw.weapon_bay.1", "sw.weapon_bay.2", "sw.weapon_bay.3", "sw.weapon_bay.4",
        "intake_ramp.l1", "intake_ramp.l2", "intake_ramp.l3",
        "intake_ramp.r1", "intake_ramp.r2", "intake_ramp.r3",
        // helicopter
        "rotor.main", "rotor.tail", "sw.rotors",
        // lights
        "light.nav", "light.strobe", "light.landing",
    };
    constexpr uint16_t kTableSize =
        static_cast<uint16_t>(sizeof(kNames) / sizeof(kNames[0]));
    static_assert(kTableSize == kChannelCount,
                  "channel_name table out of sync with Channel enum");
    const uint16_t i = static_cast<uint16_t>(c);
    if (i >= kChannelCount) return "unknown";
    return kNames[i];
}

std::optional<Channel> channel_from_name(std::string_view name) noexcept {
    for (uint16_t i = 0; i < kChannelCount; ++i) {
        const char* n = channel_name(static_cast<Channel>(i));
        if (name == n) return static_cast<Channel>(i);
    }
    return std::nullopt;
}

void AnimValues::set_parked_defaults() noexcept {
    reset();
    // Gear down and locked: legs, doors, and bay holes visible on all
    // stations. FreeFalcon's switch masks are one-hot per variant, so a
    // plain 1.0 sets bit 0 (child 0 = the shown variant on 1-child
    // gear switches; multi-variant models get their masks from the rig).
    for (uint16_t s = 0; s < kGearStations; ++s) {
        value[static_cast<uint16_t>(Channel::sw_gear_leg_0) + s] = 1.0f;
        value[static_cast<uint16_t>(Channel::sw_gear_door_0) + s] = 1.0f;
        value[static_cast<uint16_t>(Channel::sw_gear_hole_0) + s] = 1.0f;
    }
    value[static_cast<uint16_t>(Channel::gear_pos)] = 1.0f;   // down
    value[static_cast<uint16_t>(Channel::nose_gear_rot)] = 0.0f;
    value[static_cast<uint16_t>(Channel::canopy)] = 0.0f;     // closed
    value[static_cast<uint16_t>(Channel::hook)] = 0.0f;       // retracted
    value[static_cast<uint16_t>(Channel::sw_refuel_door)] = 0.0f;
}

float process_dof_value(float dof_value, int32_t flags,
                        float min, float max, float multiplier) noexcept {
    float result = dof_value;

    if (flags & kXdofNegate) {
        result = -result;
    }

    if (flags & kXdofMinmax) {
        if (result < min) result = min;
        if (result > max) result = max;
    }

    if ((flags & kXdofSubrange) && min != max) {
        // Rescale so result is 0.0 at min and 1.0 at max
        result -= min;
        result /= (max - min);

        // If this is a rotational DOF stored in degrees, convert to radians
        if (flags & kXdofIsDof) {
            result *= 0.017453293f;  // PI / 180
        }
    }

    result *= multiplier;
    return result;
}

} // namespace f4::anim
