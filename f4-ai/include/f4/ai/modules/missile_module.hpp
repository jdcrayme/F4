// f4-ai/include/f4/ai/modules/missile_module.hpp
//
// MissileModule — weapon employment fire control + missile-defeat tactics
// (AI_IMPLEMENTATION_PLAN.md §5 Step 10; FreeFalcon mengage.cpp FireControl
// + mdefeat.cpp).
//
// TWO roles, one class — exactly like the FreeFalcon pair it mirrors:
//
//  1. FIRE CONTROL (offensive). should_fire()/compute_pk() are pure
//     functions of a TargetInfo snapshot: employment envelope
//     [min_pk_range, max_pk_range], a deterministic Pk model (range +
//     aspect), a per-shot cooldown, and a shoot-shoot shot limit.
//     BVRModule composes one instance and consults it each tick while
//     Employing; note_fired() starts the cooldown and counts the shot.
//
//  2. MISSILE DEFEAT (defensive). update() takes the INCOMING hostile
//     missile's TargetInfo (SensorFusion::missile_threat() under the
//     radar-backed policy: the RWR launch warning makes the missile
//     visible) and flies the classic beam/break turn: desired heading
//     places the threat 90 deg off the nose (nearest beam side), full
//     throttle, altitude hold. The output carries has_override=true —
//     per the DigitalBrain priority ladder, defending preempts
//     everything (FreeFalcon mdefeat runs from digimain regardless of
//     the engaged mode).
//
//     Countermeasure INTENTS: should_chaff() (radar-guided default —
//     the RWR Launch warning does not carry the weapon's guidance class
//     yet) and should_flare() (inside the IR-capable envelope, ~3 NM).
//     No countermeasure consumption model exists in f4-weapons yet, so
//     these are intents the M4+ host executes when flares/chaff exist.
//
// ENGINE-AGNOSTIC CONTRACT: no world, no bus, no f4-weapons types. The
// envelope numbers are configured BY the host from the weapon class
// table (or left at the plan defaults).
//
// Dependencies: f4-flight-api (IAircraftState), f4-geo, f4-ai
// (AirSteering, AIControlOutput, TargetInfo). C++20.

#pragma once

#include <cstdint>

#include <f4/flight/api/i_aircraft_state.hpp>
#include <f4/geo/position.hpp>

#include "f4/ai/ai_output.hpp"
#include "f4/ai/air_steering.hpp"
#include "f4/ai/target_info.hpp"

namespace f4::ai::modules {

class MissileModule {
public:
    /// Fire-control parameters (plan §5 Step 10 Config).
    struct Config {
        double fire_cooldown_sec{4.0};      // between shots (shoot-shoot)
        double max_pk_range_nm{20.0};       // employment boundary (Rmax-safety)
        double min_pk_range_nm{5.0};        // minimum employment
        double shoot_shoot_threshold{0.5};  // Pk needed to fire
        int    shoot_shoot_max_shots{2};    // shots per engagement (doctrine)
        /// Pk model base (AMRAAM-class vs non-defending target).
        double pk_base{0.95};
        /// Defeat: keep beaming this long after the threat stops being
        /// visible (detonation sweeps the missile; the jink must not stop
        /// on the same tick the picture goes empty).
        double defeat_linger_sec{2.0};
        /// Defeat: inside this range a missile is IR-capable => flare intent.
        double ir_envelope_nm{3.0};
        /// Defeat: chaff intent only while the threat is outside this
        /// range (inside it, nothing beats kinematics).
        double chaff_min_range_nm{1.0};
    };

    MissileModule() = default;

    // =====================================================================
    // Role 1 — fire control (offensive)
    // =====================================================================
    /// Deterministic probability-of-kill model for a shot at `t` NOW:
    ///   pk = pk_base * range_factor * aspect_factor
    ///     range_factor  = 0.25 + 0.75 * (max-r)/(max-min), clamped [0,1]
    ///                      (1.0 at the min range, 0.25 at the boundary)
    ///     aspect_factor = 0.7 + 0.3 * ATA/pi
    ///                      (1.0 target running away — tail chase, the
    ///                       missile overtakes; 0.7 nose-on — high closure
    ///                       compresses the flyout)
    /// Monotonic in range; deterministic (no RNG). Out-of-envelope inputs
    /// return 0.
    [[nodiscard]] double compute_pk(const TargetInfo& t) const;

    /// True when a shot at `t` is legal NOW: hostile, visible, in the
    /// envelope, Pk >= threshold, cooldown expired, and the shoot-shoot
    /// allotment is not spent.
    [[nodiscard]] bool should_fire(const TargetInfo& t) const;

    /// Consume a shot: starts the cooldown, increments the count.
    void note_fired();

    /// Burn the cooldown by dt. The fire-control instance owned by
    /// BVRModule never runs update() (that is the defensive path), so the
    /// host of the offensive instance ticks this explicitly.
    void tick_cooldown(double dt);

    /// Reset the engagement bookkeeping (new target / BVR reset). The
    /// cooldown is NOT reset — it is the shooter's cadence, not the
    /// target's.
    void reset_engagement();

    // --- Fire-control state ---
    [[nodiscard]] int shots_fired() const noexcept { return shots_; }
    [[nodiscard]] double cooldown_remaining_sec() const noexcept {
        return cooldown_;
    }
    [[nodiscard]] const Config& config() const noexcept { return cfg_; }
    [[nodiscard]] Config&       config()       noexcept { return cfg_; }
    void set_config(const Config& c) noexcept { cfg_ = c; }
    /// Envelope shortcut used by the host (weapon-table-derived) and by
    /// BVRModule::entry_range_nm().
    void set_envelope_nm(double min_nm, double max_nm) noexcept {
        cfg_.min_pk_range_nm = min_nm;
        cfg_.max_pk_range_nm = max_nm;
    }

    // =====================================================================
    // Role 2 — missile defeat (defensive)
    // =====================================================================
    /// Fly the beam maneuver against `incoming` (may be nullptr: no
    /// visible hostile missile). While defending the returned output has
    /// has_override = true (preempts every other module per the brain's
    /// priority ladder); with no threat the output is EMPTY (all zeros —
    /// the brain falls through to the mission module).
    AIControlOutput update(double dt, const flight::IAircraftState* state,
                           const TargetInfo* incoming);

    // --- Defeat state ---
    [[nodiscard]] bool is_defeating() const noexcept { return defeating_; }
    [[nodiscard]] bool should_chaff() const noexcept { return chaff_; }
    [[nodiscard]] bool should_flare() const noexcept { return flare_; }
    /// The threat this module is currently defeating (0 when none).
    [[nodiscard]] std::uint64_t incoming_target_id() const noexcept {
        return incoming_id_;
    }
    /// Beam heading chosen for the current threat (rad CW from north) —
    /// exposed for tests + the FCS trace exporter.
    [[nodiscard]] double beam_heading_rad() const noexcept {
        return beam_heading_rad_;
    }

    /// The shared AirSteering instance (public fields — the defeat tune).
    [[nodiscard]] AirSteering& air_steering() noexcept { return air_steering_; }

private:
    [[nodiscard]] AirSteering::Input steering_input(
        const flight::IAircraftState& s) const noexcept;

    Config cfg_{};
    AirSteering air_steering_{};

    // Fire-control bookkeeping.
    int shots_{0};
    double cooldown_{0.0};

    // Defeat bookkeeping.
    bool defeating_{false};
    bool chaff_{false};
    bool flare_{false};
    std::uint64_t incoming_id_{0};
    double beam_heading_rad_{0.0};
    double defeat_alt_ft_{0.0};
    double linger_timer_{0.0};
};

} // namespace f4::ai::modules
