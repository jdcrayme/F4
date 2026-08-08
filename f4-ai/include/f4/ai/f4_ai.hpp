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
//   f4::ai::modules::TakeoffModule — takeoff state machine (9 states)
//   f4::ai::atc::StubATC          — stub ATC for testing/demos
//   f4::ai::atc::messages         — ATC message types
//   f4::ai::BrainComponent        — BehavioralComponent wrapping TakeoffModule
//
// Components (planned, see AI_IMPLEMENTATION_PLAN.md §5):
//   f4::ai::DigitalBrain          — orchestrator (LayeredStateMachine)
//   f4::ai::modules::LandingModule — landing/ATC state machine
//   f4::ai::modules::NavigationModule — waypoint following + autopilot
//   f4::ai::modules::RefuelModule — air refueling
//   f4::ai::modules::CollisionAvoidModule — collision avoidance (always-on overlay)
//   f4::ai::modules::BVRModule    — beyond-visual-range tactics
//   f4::ai::modules::WVRModule    — within-visual-range tactics
//   f4::ai::modules::MissileModule — missile engage + defeat
//   f4::ai::modules::WingmanModule — formation + wingman commands
//
// Dependencies: f4-flight-model, f4-entities, f4-messaging, f4-state-machine,
// f4-geo, f4-data, f4-math, f4-recorder. C++20.

#pragma once

#include <f4/ai/ai_brain.hpp>
#include <f4/ai/ai_output.hpp>
#include <f4/ai/sensor_fusion.hpp>
#include <f4/ai/target_info.hpp>

// AI tactic modules
#include <f4/ai/modules/takeoff_module.hpp>
#include <f4/ai/modules/scripted_tanker.hpp>

// Brain component (Phase A.2 — wraps a TakeoffModule as a BehavioralComponent)
#include <f4/ai/brain_component.hpp>

// ATC protocol
#include <f4/ai/atc/messages.hpp>
#include <f4/ai/atc/stub_atc.hpp>

// (ScenarioRunner was deleted in Phase A.2 — the sim loop now lives in the
//  f4-sim CLI app, built on EntityWorld::update_all + BrainComponent +
//  FlightModelComponent. See Docs/AI_IMPLEMENTATION_PLAN.md.)
