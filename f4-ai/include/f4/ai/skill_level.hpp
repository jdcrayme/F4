// f4-ai/include/f4/ai/skill_level.hpp
//
// SkillLevel — pilot proficiency for AI parameter scaling.
//
// Affects 8 parameters (target update interval, reaction delay,
// gun jink timing, max G, formation tolerance, shoot-shoot doctrine,
// fuel awareness, missile PK threshold). See AI_IMPLEMENTATION_PLAN §9
// for the full table.
//
// Extracted from the retired ai_brain.hpp: the IAIBrain interface had
// no implementation and no consumer; the enum is the part that lives.

#pragma once

#include <cstdint>

namespace f4::ai {

enum class SkillLevel : std::uint8_t {
    Recruit = 0,
    Rookie  = 1,
    Veteran = 2,
    Ace     = 3,
};

} // namespace f4::ai
