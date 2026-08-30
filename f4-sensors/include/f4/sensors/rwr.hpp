// f4-sensors/include/f4/sensors/rwr.hpp
//
// Radar Warning Receiver — the "threat badge".
//
// The RWR is a PASSIVE receiver: it hears active radars. Three things matter
// to a pilot, and they map to three warning types:
//
//   Search — an enemy radar is sweeping and its beam currently covers us
//            (the rotating "strobing" line on a real RWR scope)
//   Lock   — an enemy radar is parked on us in single-target track (the
//            steady line; the classic "locked" warning)
//   Launch — a missile is in flight within RWR range (the missile's seeker
//            and rocket plume are emitters of their own; FreeFalcon models
//            launch detection separately from the launching radar)
//
// Layering:
//   RwrModel       — PURE: classify a list of EmitterReadings into warnings.
//                    Unit-testable with no world at all.
//   RwrComponent   — PASSIVE ECS state: the current warning list + the
//                    lock/launch activity flags + the new_lock/new_launch
//                    transition flags that brains consume (and clear).
//   update_rwr()   — WORLD-LEVEL sweep: gathers EmitterReadings for every
//                    RwrComponent in the world (from RadarSimComponents and
//                    missile-role entities), runs the pure model, diffs
//                    against the previous picture, publishes
//                    RwrWarningMessage ON TRANSITIONS ONLY (search strobes
//                    are queryable via the component — publishing them
//                    every scan would flood the bus). Hosts call it between
//                    ticks, like sweep_spent_missiles().
//
// Determinism: no RNG anywhere; warnings are sorted Launch < Lock < Search,
// then by emitter id.

#pragma once

#include <cstdint>
#include <vector>

#include <f4/entities/entity.hpp>
#include <f4/geo/position.hpp>
#include <f4/messaging/bus.hpp>
#include <f4/sensors/radar_component.hpp>

namespace f4::sensors {

// ============================================================================
// Warnings
// ============================================================================

enum class RwrWarningType {
    Search,
    Lock,
    Launch,
};

struct RwrWarning {
    RwrWarningType type{};
    std::uint64_t emitter_id = 0;
    double bearing_rad = 0.0;   // CW from north, at the victim
    double range_ft = 0.0;      // slant range emitter -> victim
    double time_s = 0.0;
};

/// What one emitter looks like to one victim's RWR this tick. Built by the
/// world-level gatherer (update_rwr); the pure model only consumes these.
struct EmitterReading {
    std::uint64_t emitter_id = 0;
    f4::geo::WorldPosition position{};
    bool is_missile = false;           // missile in flight => Launch
    bool is_locked_on_self = false;    // radar in Track mode on this victim
    bool is_illuminating_self = false; // search beam currently covers victim
};

// ============================================================================
// RwrModel — pure classification
// ============================================================================

struct RwrConfig {
    double max_range_nm = 100.0;   // receiver range (missiles + radars)
};

class RwrModel {
public:
    explicit RwrModel(RwrConfig cfg = {}) : cfg_(cfg) {}

    /// Classify readings into warnings. Readings beyond RWR range are
    /// ignored. Sorting: Launch first, then Lock, then Search; ties broken
    /// by emitter id ascending.
    [[nodiscard]] std::vector<RwrWarning> evaluate(
        const std::vector<EmitterReading>& readings,
        const f4::geo::WorldPosition& own_pos,
        double time_s) const;

    [[nodiscard]] const RwrConfig& config() const noexcept { return cfg_; }

private:
    RwrConfig cfg_{};
};

// ============================================================================
// RwrComponent — passive per-victim state
// ============================================================================

struct RwrComponent : public entities::Component<RwrComponent> {
    std::vector<RwrWarning> warnings;   // current picture (rebuilt each update)
    bool lock_active = false;           // any Lock warning present
    bool launch_active = false;         // any Launch warning present
    // Transition flags: set when the corresponding activity STARTS this tick
    // (none -> active). Brains consume and clear them.
    bool new_lock = false;
    bool new_launch = false;

    void clear_transitions() noexcept { new_lock = false; new_launch = false; }
};

// ============================================================================
// update_rwr — world-level sweep (call between ticks)
// ============================================================================

/// Published when a victim's RWR first sees a Lock or Launch (transition
/// only; Search strobes are component-state, not bus traffic).
struct RwrWarningMessage {
    std::uint64_t victim_id = 0;
    RwrWarningType type{};
    std::uint64_t emitter_id = 0;
    double bearing_rad = 0.0;
    double range_ft = 0.0;
    double time_s = 0.0;
};

/// Rebuild every RwrComponent's warning picture from the current world
/// state:
///   - entities with a RadarSimComponent in Track mode locked on the victim
///     produce a Lock reading;
///   - radar emitters whose scan volume currently contains the victim and
///     whose detection range reaches it produce a Search reading;
///   - entities tagged ROLE="missile" within RWR range produce Launch.
///
/// Publishes RwrWarningMessage for NEW lock/launch emitters (not present in
/// the previous warning list). Returns the number of victims updated.
std::size_t update_rwr(entities::EntityWorld& world,
                       messaging::MessageBus& bus,
                       double time_s,
                       const RwrConfig& cfg = {});

} // namespace f4::sensors
