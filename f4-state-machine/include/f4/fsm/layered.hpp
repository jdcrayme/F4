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
// INTER-LAYER INHIBITION
//   When a higher-priority layer activates, it can inhibit (suppress)
//   lower-priority layers. The inhibit_mask on each layer specifies which
//   lower-priority layers should be forced to their idle state when this
//   layer is active. This replaces FreeFalcon's scattered addMode()
//   interlocks (e.g. "LandingMode can't be bumped by WVR") with an
//   explicit, inspectable data structure.
//
//   After process(e), the inhibition pass runs: for each active (non-idle)
//   layer, any layers in its inhibit_mask are reset to idle. This ensures
//   that a high-priority concern (e.g. GroundAvoid) can suppress a
//   lower-priority one (e.g. WaypointFollow) without the lower layer
//   fighting back on the next event.
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
#include <set>
#include <vector>

namespace f4::fsm {

template<typename ModeEnum, typename Event>
class LayeredStateMachine {
public:
    using LayerSM = StateMachine<ModeEnum, Event>;

    /// Add a layer. Lower priority number = higher precedence.
    /// idle_state is the state meaning "this layer is inactive".
    /// inhibit_mask is the set of layer indices that this layer suppresses
    /// when active (inter-layer inhibition). Empty by default.
    void add_layer(int priority, ModeEnum idle_state, LayerSM sm,
                   std::set<std::size_t> inhibit_mask = {}) {
        layers_.push_back(Layer{priority, idle_state, std::move(sm), std::move(inhibit_mask)});
        // Keep sorted by priority ascending (highest precedence first).
        std::stable_sort(layers_.begin(), layers_.end(),
            [](const Layer& a, const Layer& b){ return a.priority < b.priority; });
    }

    /// Deliver the event to every layer, in priority order.
    /// Then apply inter-layer inhibition: active layers force their
    /// inhibited layers back to idle.
    void process(const Event& e) {
        for (auto& l : layers_) l.sm.process(e);
        applyInhibition();
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

    /// Get the inhibition mask for a layer (for inspection/debugging).
    [[nodiscard]] const std::set<std::size_t>& layer_inhibits(std::size_t i) const noexcept {
        return layers_[i].inhibit_mask;
    }

private:
    /// Apply inter-layer inhibition: for each active (non-idle) layer,
    /// force any layers in its inhibit_mask back to their idle state.
    ///
    /// C2 FIX: Uses force_to_state(idle_state) instead of reset().
    /// reset() fires the initial state's entry action (which may have
    /// side effects like starting timers or publishing messages), but
    /// inhibition is an administrative suppression — the suppressed layer
    /// should silently return to idle without side effects. force_to_state()
    /// sets the state without firing any actions.
    void applyInhibition() {
        for (std::size_t i = 0; i < layers_.size(); ++i) {
            if (layers_[i].sm.current() != layers_[i].idle_state) {
                // Layer i is active — suppress its inhibited layers
                for (const auto& j : layers_[i].inhibit_mask) {
                    if (j < layers_.size() && j != i) {
                        layers_[j].sm.force_to_state(layers_[j].idle_state);
                    }
                }
            }
        }
    }

    struct Layer {
        int      priority;
        ModeEnum idle_state;
        LayerSM  sm;
        std::set<std::size_t> inhibit_mask;  // layers this one suppresses when active
    };
    std::vector<Layer> layers_;
};

}  // namespace f4::fsm
