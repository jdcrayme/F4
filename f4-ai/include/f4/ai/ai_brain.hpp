// f4-ai/include/f4/ai/ai_brain.hpp
//
// IAIBrain — abstract interface for AI brains.
//
// The DigitalBrain (composed of SensorFusion + tactic modules) is the
// concrete implementation. This interface exists so consumers (scripting,
// replay, network) can substitute alternative brains without depending on
// the full DigitalBrain class hierarchy.
//
// SkillLevel affects 8 parameters (target update interval, reaction delay,
// gun jink timing, max G, formation tolerance, shoot-shoot doctrine, fuel
// awareness, missile PK threshold). See AI_IMPLEMENTATION_PLAN §9 for the
// full table.

#pragma once

#include <cstdint>
#include <string>

#include <f4/entities/entity.hpp>
#include <f4/messaging/f4_messaging.hpp>

#include "ai_output.hpp"

namespace f4::ai {

enum class SkillLevel : std::uint8_t {
    Recruit = 0,
    Rookie  = 1,
    Veteran = 2,
    Ace     = 3,
};

class IAIBrain {
public:
    virtual ~IAIBrain() = default;

    virtual void initialize(
        std::uint64_t ownship_id,
        entities::EntityWorld& world,
        messaging::MessageBus& bus,
        SkillLevel skill
    ) = 0;

    virtual AIControlOutput update(double dt) = 0;

    [[nodiscard]] virtual std::string current_mode() const = 0;
};

} // namespace f4::ai
