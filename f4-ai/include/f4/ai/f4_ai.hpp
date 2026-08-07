// f4-ai/include/f4/ai/f4_ai.hpp
//
// Umbrella header for f4-ai — the engine-agnostic AI brain.
//
// Include this to get the full public API. The AI is composed of focused
// modules (SensorFusion, TakeoffModule, NavigationModule, ...) orchestrated
// by DigitalBrain. Each module has its own state machine and trace.
//
// Components (current):
//   f4::ai::AIControlOutput       — per-frame output to the FlightModel
//   f4::ai::SkillLevel            — Recruit/Rookie/Veteran/Ace enum
//   f4::ai::IAIBrain              — abstract brain interface
//   f4::ai::TargetInfo            — per-target snapshot
//   f4::ai::SensorFusion          — target list + threat scoring
//
// Components (planned, see AI_IMPLEMENTATION_PLAN.md §5):
//   f4::ai::DigitalBrain          — orchestrator (LayeredStateMachine)
//   f4::ai::TakeoffModule         — takeoff state machine
//   f4::ai::LandingModule         — landing/ATC state machine
//   f4::ai::NavigationModule      — waypoint following + autopilot
//   f4::ai::RefuelModule          — air refueling
//   f4::ai::CollisionAvoidModule  — collision avoidance (always-on overlay)
//   f4::ai::BVRModule             — beyond-visual-range tactics
//   f4::ai::WVRModule             — within-visual-range tactics
//   f4::ai::MissileModule         — missile engage + defeat
//   f4::ai::WingmanModule         — formation + wingman commands
//
// Dependencies: f4-flight-model, f4-entities, f4-messaging, f4-state-machine,
// f4-geo, f4-data, f4-math. C++20.

#pragma once

#include <f4/ai/ai_brain.hpp>
#include <f4/ai/ai_output.hpp>
#include <f4/ai/sensor_fusion.hpp>
#include <f4/ai/target_info.hpp>
