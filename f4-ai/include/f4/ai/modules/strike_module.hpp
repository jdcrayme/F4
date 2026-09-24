// f4-ai/include/f4/ai/modules/strike_module.hpp
//
// StrikeModule — air-to-ground bomb release fire control (the M5 A-G
// employment slice; FreeFalcon mengage.cpp's strike pass + FCC bomb
// release, reduced to the release-trigger decision).
//
// The module does not fly the aircraft (the NavigationModule does — the
// strike waypoint IS a route waypoint) and does not know what a bomb is
// (f4-ai never links f4-weapons). It watches the AIM POINT and pulses a
// release INTENT when the geometry says "drop now":
//
//    release when horizontal distance to the aim point
//                 <= ballistic_range(dz, groundspeed, drag_factor)
//
//    ballistic_range = v * sqrt(2 * dz / g) * drag_factor
//
//      dz          = aircraft altitude - aim point altitude
//      v           = ground speed (IAircraftState::ground_speed_fps)
//      drag_factor = a scalar the host computes from the weapon class
//                    card (drag shortens the vacuum throw; the Mk-82's
//                    own ODE measures 0.999 — a slick body sheds ~2 fps
//                    of 675 over the whole fall) — set at spawn, one
//                    number, no weapon types cross the boundary.
//
// The trigger evaluates EVERY tick (dz and v change continuously), so a
// descending or accelerating aircraft re-solves the release point rather
// than inheriting a stale one.
//
// Salvo: a stick of releases spaced `salvo_interval_s` apart, up to
// `salvo_max` bombs, then the module reports `delivered()` and never
// pulses again for that target. The host's release path debits the real
// store — a dry station makes the host's release fail silently, and the
// module's count still advances (a pulse the host can't fulfill is the
// module's "tried" — matching the missile modules' note_fired() contract
// where a pulse IS the shot).
//
// ENGINE-AGNOSTIC CONTRACT: no world, no bus, no weapon types. The brain
// resolves the aim point (a world position + target entity id) and calls
// update() with it each tick.
//
// Dependencies: f4-flight-api (IAircraftState), f4-geo. C++20.

#pragma once

#include <cstdint>

#include <f4/flight/api/i_aircraft_state.hpp>
#include <f4/geo/position.hpp>

namespace f4::ai::modules {

/// True when a route waypoint's WP_ACTION is an A/G ordnance delivery
/// action (FreeFalcon campwp.h): 14 GNDSTRIKE, 15 NAVSTRIKE, 17 STRIKE,
/// 18 BOMB, 19 SEAD. The brain arms the StrikeModule only on these.
[[nodiscard]] constexpr bool is_ag_delivery_action(std::uint8_t action) noexcept {
    switch (action) {
        case 14:  // WP_GNDSTRIKE
        case 15:  // WP_NAVSTRIKE
        case 17:  // WP_STRIKE
        case 18:  // WP_BOMB
        case 19:  // WP_SEAD
            return true;
        default:
            return false;
    }
}

/// Tranche D (AAR): the waypoint action that arms the RefuelModule rung.
/// FreeFalcon campwp.h has no dedicated WP_REFUEL constant (refuel in the
/// original was triggered by the DigitalBrain's fuel-gate + a proximity
/// check on the tanker entity, not a waypoint action). We reserve 20 here
/// as the engine-agnostic scenario marker: a route waypoint carrying this
/// action tells the host (Simulation) to arm the receiver's refuel rung
/// and push the tanker picture while the receiver flies this leg. The
/// value 20 is the next free slot after the SEAD action (19) and below
/// the campaign bridge's internal range — it round-trips through the
/// scenario JSON as `"action": 20` and through MissionPlan::route as the
/// Waypoint::action byte.
inline constexpr std::uint8_t WP_REFUEL{20};

[[nodiscard]] constexpr bool is_refuel_action(std::uint8_t action) noexcept {
    return action == WP_REFUEL;
}

/// EMPL-2 — the CAMPAIGN wire's WP_REFUEL byte (campwp.h WP_REFUEL = 4;
/// the vocabulary the real saves' receiver routes carry — TestCamp: 158
/// flights — and the RouteBuilder's tanker/receiver stamps emit). This
/// is a DIFFERENT vocabulary from the scenario marker above, and the
/// two must never merge: in the campaign vocabulary action 20 is
/// WP_ELINT (the AWACS/ECM station legs — merging would arm AWACS
/// orbits as refuel receivers), and in the scenario vocabulary 4 is an
/// unassigned slot. The brain-level leg gate (at_refuel_waypoint) runs
/// on CAMPAIGN routes only, so it keys the campaign byte.
inline constexpr std::uint8_t WP_REFUEL_CAMPAIGN{4};

[[nodiscard]] constexpr bool is_campaign_refuel_action(
    std::uint8_t action) noexcept {
    return action == WP_REFUEL_CAMPAIGN;
}

class StrikeModule {
public:
    /// Release-trigger parameters. Defaults are the doctrine fill for a
    /// 2-station slick-bomb stick; hosts configure from the weapon class
    /// card + loadout at spawn.
    struct Config {
        /// Vacuum-to-real range scale (host computes from the bomb card:
        /// drag area/mass at delivery speed). 1.0 = vacuum ballistics.
        /// EMPL-1a calibration: the Mk-82 card's own ODE (bomb.cpp's drag
        /// model, 5,000 ft / 675 fps — campaign_bridge's
        /// bomb_drag_factor_for) measures 0.999 of vacuum: over a 13-20 s
        /// fall the slick body sheds ~2 fps of the 675 it starts with.
        /// The old 0.85 folklore default under-predicted the throw ~15%
        /// (~1,300 ft at dz 3,000) — the gate fired early and the stick
        /// landed systematically long. The host's computed factor
        /// overrides this anyway; 1.0 is the physically honest default
        /// (hosts set < 1.0 for draggy shapes).
        double drag_factor{1.0};
        /// Stick spacing (seconds between releases).
        double salvo_interval_s{0.25};
        /// Stick size (bombs per target). The host's store may run dry
        /// first — the module counts pulses, not hits.
        int    salvo_max{4};
        /// Minimum altitude above the aim point to release (a terrain-
        /// clearance floor for the delivery pass; the trigger is skipped
        /// below it and re-arms if the aircraft climbs back).
        double min_release_agl_ft{500.0};
        /// Impact-point acceptance distance (ft): the predicted impact
        /// must sit well inside the blast footprint to start a stick on
        /// a STRAIGHT-IN delivery (the harness's geometry). The host sets
        /// it from the weapon's lethal radius (~half). EMPL-1: the
        /// campaign gate no longer reads this field — at the flat-world
        /// campaign delivery geometry (dz ~3,000 ft -> throw ~9,500 ft)
        /// a 150-ft pipper demands sub-0.9-deg alignment, which no
        /// dynamic approach holds (the "armed, no release" exit 4); the
        /// gate is range + release_cone_rad instead. Kept settable for
        /// host compatibility and as the recorded diagnostic threshold;
        /// a per-mission-mode gate (pipper for aligned run-ins, cone for
        /// campaign passes) is the named refinement.
        double impact_tolerance_ft{150.0};
        /// EMPL-1: forward alignment cone (radians) — the aim must sit
        /// inside this cone off the release track for the trigger to
        /// arm. This is the mid-turn protection (a 60-deg-off aircraft
        /// holds its stick) without the pipper gate's sub-degree demand,
        /// which the flat-world campaign delivery geometry (dz ~3,000 ft
        /// -> throw ~9,500 ft -> 150 ft = 0.9 deg) made unsatisfiable —
        /// the TestCamp INTSTRIKE sweep's "armed, no release" exit 4.
        double release_cone_rad{0.35};
        /// ROE: weapons tight — never pulse (same gate semantics as the
        /// missile fire controls: gate here, not at the intent, so no
        /// phantom shot is ever counted).
        bool   hold_fire{false};
        /// Gravitational constant for the ballistic solution (ft/s^2).
        /// Public so tests can pin it against the weapons layer's.
        static constexpr double kGravityFps2 = 32.174;
    };

    StrikeModule() = default;

    // --- Target management (the brain drives this) ---
    // Set when the current route waypoint is an A/G delivery point with a
    // resolvable target; cleared when the waypoint passes or the target
    // dies. A NEW target id resets the stick (fresh salvo); re-setting the
    // same id is a no-op — the brain may call this every tick.
    void set_target(std::uint64_t target_entity_id) {
        if (target_entity_id == target_id_) return;
        target_id_ = target_entity_id;
        armed_ = false;
        delivered_ = false;
        salvo_fired_ = 0;
        since_release_s = -1.0;
    }
    void clear_target() {
        target_id_ = 0;
        armed_ = false;
    }
    [[nodiscard]] std::uint64_t target_id() const noexcept { return target_id_; }
    [[nodiscard]] bool has_target() const noexcept { return target_id_ != 0; }

    // --- Per-tick update ---
    // `aim` is the target's world position (ENU ft); `aim_valid` false
    // disarms the trigger for this tick (target dead/unresolvable — the
    // stick stops, matching FreeFalcon's abort-on-target-death).
    void update(double dt, const flight::IAircraftState* state,
                const geo::WorldPosition& aim, bool aim_valid);

    // --- Intent output (read by the brain after update) ---
    /// One-tick release pulse for the assigned target.
    [[nodiscard]] bool release_pulse() const noexcept { return pulse_; }
    [[nodiscard]] std::uint64_t release_target_id() const noexcept {
        return target_id_;
    }
    /// True once the stick is complete (or the target went invalid after
    /// a partial stick): the brain stops arming the module for this
    /// target.
    [[nodiscard]] bool delivered() const noexcept { return delivered_; }
    /// Pulses emitted so far this stick (QC diagnostics).
    [[nodiscard]] int salvo_fired() const noexcept { return salvo_fired_; }
    /// EMPL-2d — the last aim position update() was driven with (the
    /// brain's resolved aim point — the objective center, the indexed
    /// feature, or the first-alive fallback). QC diagnostics + the
    /// aim-point tests.
    [[nodiscard]] const geo::WorldPosition& last_aim() const noexcept {
        return last_aim_;
    }
    [[nodiscard]] bool last_aim_valid() const noexcept {
        return last_aim_valid_;
    }
    /// True while the trigger is armed (target set, stick incomplete).
    [[nodiscard]] bool armed() const noexcept { return armed_; }
    /// EMPL-1 diagnostics: true once a ground track was differenced.
    [[nodiscard]] bool has_track() const noexcept { return has_track_; }

    /// Computed release range for the current geometry (ft; 0 when the
    /// aircraft is below the min release AGL) — the THROW distance the
    /// bomb would fly from right now. Exposed for the QC trace + tests:
    /// the brain's strike state reads as "R=12,340 ft" in the flight
    /// recorder's ai_state column.
    [[nodiscard]] double computed_release_range_ft() const noexcept {
        return computed_range_ft_;
    }

    /// The predicted impact point's miss distance from the aim (ft) at
    /// the last update — the CCIP pipper's offset. QC diagnostics.
    [[nodiscard]] double predicted_miss_ft() const noexcept {
        return predicted_miss_ft_;
    }

    Config config;

private:
    std::uint64_t target_id_ = 0;
    bool   armed_ = false;
    bool   pulse_ = false;
    bool   delivered_ = false;
    int    salvo_fired_ = 0;
    geo::WorldPosition last_aim_{};
    bool   last_aim_valid_ = false;
    double since_release_s = -1.0;   // <0 = not in a stick
    double computed_range_ft_ = 0.0;
    double predicted_miss_ft_ = 0.0;

    // EMPL-1: the throw direction comes from the aircraft's TRACK
    // (velocity over ground), not its nose. The bomb inherits the
    // aircraft's VELOCITY; during the attack run's homing turn the nose
    // leads the track by a couple of degrees, which at a ~9,500 ft
    // throw is hundreds of feet of pipper offset — the TestCamp
    // INTSTRIKE run's release gate never satisfied although the flight
    // path itself crossed the aim within feet. Track is differenced
    // from consecutive position samples (the engine-agnostic interface
    // exposes positions, not a velocity vector); until a second sample
    // exists (or the samples are stationary) the nose is the fallback,
    // which is exact on a straight-in.
    bool   has_prev_pos_ = false;
    bool   has_track_ = false;
    double prev_east_ft_ = 0.0;
    double prev_north_ft_ = 0.0;
    double track_x_ = 0.0;          // unit track vector, ENU
    double track_y_ = 0.0;
};

} // namespace f4::ai::modules
