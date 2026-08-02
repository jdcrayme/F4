// f4-state-machine/include/f4/fsm/layered.hpp
//
// LayeredStateMachine: a priority ladder of independent StateMachines.
//
// WHY THIS EXISTS
//   The FreeFalcon AI's 24 DigiMode values form a priority ladder:
//   GroundAvoid > MissileDefeat > ... > Waypoint. Higher-priority modes
//   preempt lower ones. In the original code this is a single enum plus
//   scattered addMode() interlocks ("LandingMode can't be bumped by WVR").
//   That structure is exactly what made behavior "impossible to track down":
//   the effective mode is an emergent property of interlocks spread across
//   dlogic.cpp, not a thing you can inspect.
//
//   The layered model makes priority EXPLICIT and INSPECTABLE: each concern
//   (ground-avoid, missile-defeat, navigation, landing, ...) is its own
//   StateMachine with an idle state. The effective mode is the highest-
//   priority (lowest index) layer whose current state is not idle. You can
//   ask "which layer is active?" and "what is each layer doing?" at any time.
//
// EVENT BROADCAST
//   process(e) delivers the event to EVERY layer, in priority order. Each
//   layer independently decides whether to transition. This matches the
//   original semantics where every mode-evaluation sees the same inputs.
//   A layer in its idle state that receives an event it cares about will
//   leave idle — preempting all lower-priority layers automatically.
//
// LIFETIME
//   Layers are added with add_layer(priority, idle_state, sm). The vector is
//   kept sorted by priority ascending (lowest number = highest priority).
//   effective_mode() returns the first non-idle layer's current state, or
//   the lowest-priority layer's current state if all are idle (so there is
//   always a sensible default — typically the navigation/waypoint layer).

#pragma once

#include "f4/fsm/state_machine.hpp"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace f4::fsm {

template<typename ModeEnum, typename Event>
class LayeredStateMachine {
public:
    using LayerSM = StateMachine<ModeEnum, Event>;

    /// Add a layer. Lower priority number = higher precedence.
    /// idle_state is the state meaning "this layer is inactive".
    void add_layer(int priority, ModeEnum idle_state, LayerSM sm) {
        layers_.push_back(Layer{priority, idle_state, std::move(sm)});
        // Keep sorted by priority ascending (highest precedence first).
        std::stable_sort(layers_.begin(), layers_.end(),
            [](const Layer& a, const Layer& b){ return a.priority < b.priority; });
    }

    /// Deliver the event to every layer, in priority order.
    void process(const Event& e) {
        for (auto& l : layers_) l.sm.process(e);
    }

    /// The effective mode: the highest-precedence (lowest index) layer whose
    /// current state is not its idle state. If all layers are idle, returns
    /// the lowest-precedence layer's current state (the fallback/default —
    /// conventionally the navigation layer, which is never truly "idle").
    [[nodiscard]] ModeEnum effective_mode() const {
        for (const auto& l : layers_) {
            if (l.sm.current() != l.idle_state) return l.sm.current();
        }
        return layers_.back().sm.current();
    }

    /// Index of the active (highest-precedence non-idle) layer, or the last
    /// layer if all are idle. Useful for "which concern is driving the AI?".
    [[nodiscard]] std::size_t active_layer_index() const {
        for (std::size_t i = 0; i < layers_.size(); ++i) {
            if (layers_[i].sm.current() != layers_[i].idle_state) return i;
        }
        return layers_.size() - 1;
    }

    [[nodiscard]] std::size_t layer_count() const noexcept { return layers_.size(); }
    [[nodiscard]] const LayerSM& layer(std::size_t i) const { return layers_[i].sm; }
    [[nodiscard]] int layer_priority(std::size_t i) const noexcept { return layers_[i].priority; }
    [[nodiscard]] ModeEnum layer_idle(std::size_t i) const noexcept { return layers_[i].idle_state; }
    [[nodiscard]] ModeEnum layer_state(std::size_t i) const noexcept { return layers_[i].sm.current(); }

    /// Reset every layer to its initial state.
    void reset() { for (auto& l : layers_) l.sm.reset(); }

private:
    struct Layer {
        int      priority;
        ModeEnum idle_state;
        LayerSM  sm;
    };
    std::vector<Layer> layers_;
};

}  // namespace f4::fsm
