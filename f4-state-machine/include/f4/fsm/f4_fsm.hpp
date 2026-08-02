// f4-state-machine/include/f4/fsm/f4_fsm.hpp
//
// Umbrella header for the f4-state-machine library. Include this to get the
// full API:
//
//   #include <f4/fsm/f4_fsm.hpp>
//
// Components:
//   f4::fsm::Transition            — one (from,event)→(to,action) row
//   f4::fsm::StateMachine          — transition-table FSM with builder
//   f4::fsm::LayeredStateMachine   — priority ladder of FSMs (AI DigiMode)
//   f4::fsm::Trace / TransitionRecord — bounded transition log, text-emitting
//   f4::fsm::to_text / summary_text   — table & trace serialization (no deps)
//
// Zero dependencies. C++20. Header-only.

#pragma once

#include "f4/fsm/transition.hpp"
#include "f4/fsm/state_machine.hpp"
#include "f4/fsm/layered.hpp"
#include "f4/fsm/trace.hpp"
#include "f4/fsm/serialize.hpp"
