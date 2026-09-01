// f4-ai/include/f4/ai/modules/collision_avoid_module.hpp
//
// CollisionAvoidModule — mid-air collision avoidance, the DigitalBrain
// priority ladder's rung 2 (AI_IMPLEMENTATION_PLAN.md §3.3; FreeFalcon
// digi_cavoid.cpp — CollisionCheck + CollisionAvoid, reference §13).
//
// The FreeFalcon algorithm, ported 1:1 (digi_cavoid.cpp:12):
//
//   CONSTANTS (reference §13.1):
//     hRange   = 200 ft   (miss distance — the protected bubble)
//     reactFact = 0.55
//     GS_LIMIT  = 9.0 G   (the G budget a reaction may assume)
//     reactTime = (GS_LIMIT / maxGs) * reactFact
//                (maxGs = the OWN aircraft's G capability, clamped to a
//                 2.5 G floor — the reference's rule; 9 G jet -> 0.55 s,
//                 4 G jet -> 1.24 s: a sluggish airframe gets MORE
//                 warning, not less)
//
//   Detection (per intruder):
//     timeToImpact = (range - hRange) / -rangedot
//     if (timeToImpact > reactTime AND range > hRange): not a threat
//     else: linear extrapolation for dt = 0.05 .. reactTime step 0.1 —
//           predict BOTH aircraft; if separation < hRange at any step a
//           collision is predicted; break early when separation starts
//           diverging (the geometry resolved itself).
//
//   Response (reference §13.2):
//     Escape point at 45 deg azimuth / 45 deg elevation, placed OPPOSITE
//     to the target's roll (its roll RATE, droll — the reference's
//     SimObjectLocalData field), at 10,000 ft range; fly to it at max
//     performance (TrackPoint(maxGs, cornerSpeed)).
//
// Tiebreak when the target is not rolling (droll ~ 0): break to the RIGHT
// of the bearing-to-target. FreeFalcon does not specify this case; the
// universal aviation convention for head-on convergence does — both
// aircraft turning right separates them (two jets each offsetting 45 deg
// right open ~2,100 ft/s of lateral separation; both offsetting the SAME
// way from opposite headings does not — the tiebreak is load-bearing).
//
// The module needs ALL airborne traffic (friendlies included — formation
// mates are exactly the aircraft you do not want to fly through), so the
// HOST pushes the traffic picture every tick via set_traffic(): the
// engine-agnostic contract means the module cannot scan the world itself.
// The host gates the list by range (the 1 NM bubble — everything the
// extrapolation could ever care about) and airborne status.
//
// Both aircraft see each other and both break — with opposite roll states
// the escapes diverge; with level flight both break right. The avoidance
// HOLDS for avoid_hold_sec after the last predicted collision (the break
// is a maneuver, not a one-tick twitch — the same linger discipline the
// missile-defeat and gun modules use).
//
// ENGINE-AGNOSTIC CONTRACT: no world, no bus. Pure function of the
// ownship state + the pushed traffic list.
//
// Dependencies: f4-flight-api (IAircraftState), f4-geo, f4-ai
// (AirSteering, AIControlOutput). C++20.

#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include <f4/flight/api/i_aircraft_state.hpp>
#include <f4/geo/position.hpp>

#include "f4/ai/ai_output.hpp"
#include "f4/ai/air_steering.hpp"

namespace f4::ai::modules {

class CollisionAvoidModule {
public:
    /// Tuning (FreeFalcon digi_cavoid.cpp constants + the escape tune).
    struct Config {
        /// hRange: the protected miss distance bubble (ft).
        double h_range_ft{200.0};
        /// reactFact: fraction of the G-limited reaction time used.
        double react_fact{0.55};
        /// GS_LIMIT: the G budget a reaction may assume.
        double gs_limit{9.0};
        /// The OWN aircraft's max G (clamped to a 2.5 floor in the react
        /// time computation — the reference's rule).
        double own_max_g{7.0};
        /// Escape point range (ft) — the TrackPoint distance.
        double escape_range_ft{10000.0};
        /// Escape azimuth offset from the bearing-to-intruder (deg).
        double escape_az_deg{45.0};
        /// Escape elevation (deg) — the escape goes UP as well as off.
        double escape_el_deg{45.0};
        /// Hold the break this long after the last predicted collision (s).
        double avoid_hold_sec{1.5};
        /// Escape speed (KCAS) — corner-ish, max performance territory.
        double escape_speed_kts{450.0};
        /// VS cap during the break (fpm) — lifted from the nav default.
        double max_vs_fpm{12000.0};
        /// Bank cap during the break (rad) — a fighting break, not a nav
        /// turn.
        double max_bank_rad{1.0};
        /// Master arm (host/test A-B switch).
        bool enabled{true};
    };

    /// One nearby aircraft, as seen by the host's traffic sweep (pushed
    /// per tick via set_traffic). Positions/velocities are ENU feet and
    /// ft/s from the host's last synced transforms; droll is the
    /// body-frame roll RATE (rad/s, + rolling right) — FreeFalcon's
    /// escape-side key.
    struct Intruder {
        std::uint64_t entity_id{0};
        geo::WorldPosition position{};
        geo::WorldPosition velocity{};
        double roll_rate_radps{0.0};
    };

    CollisionAvoidModule() = default;

    /// The committed firing pass (the brain's rule): while fighting an
    /// intruder in WVR inside the gun employment band, the BRAIN exempts
    /// that intruder here — the firing pass owns the geometry (the
    /// bullets arrive ~2.5x faster than the airframes converge; a break
    /// at 1,200 ft forfeits the shot that resolves the pass). This is
    /// the documented midair trade: real merges are committed, and real
    /// merges occasionally end in the midair the exemption risked
    /// (FreeFalcon's digi has the same character). CA still protects
    /// everything else — formation errors, crossing traffic, chase
    /// overshoots. 0 (the default) exempts nothing.
    void set_exempt_id(std::uint64_t entity_id) noexcept {
        exempt_id_ = entity_id;
    }
    [[nodiscard]] std::uint64_t exempt_id() const noexcept {
        return exempt_id_;
    }

    /// Host per-tick push: the traffic within the gate + the OWN
    /// velocity (ENU ft/s, from the same transform snapshot the traffic
    /// came from — the relative geometry the extrapolation runs on must
    /// be one consistent picture). An empty vector is a valid picture
    /// ("nobody near") and clears detection (the linger may still fly
    /// the last break). A nullopt own velocity (the standalone/unit-test
    /// case) falls back to deriving it from heading + CAS + VS.
    void set_traffic(std::vector<Intruder> traffic,
                     std::optional<geo::WorldPosition> own_velocity) {
        traffic_ = std::move(traffic);
        own_velocity_ = own_velocity;
    }
    [[nodiscard]] const std::vector<Intruder>& traffic() const noexcept {
        return traffic_;
    }

    /// One tick of the collision-avoid rung. Returns the break output
    /// (has_override=true while avoiding) or an empty output.
    AIControlOutput update(double dt, const flight::IAircraftState* state);

    // --- State ---
    /// True while flying a break (incl. the linger window).
    [[nodiscard]] bool is_avoiding() const noexcept { return avoiding_; }
    /// The intruder the current break is against (0 when clear).
    [[nodiscard]] std::uint64_t intruder_id() const noexcept {
        return intruder_id_;
    }
    /// The current escape azimuth (rad CW from north) — the TrackPoint's
    /// horizontal bearing. Exposed for tests + the FCS trace exporter.
    [[nodiscard]] double escape_az_rad() const noexcept {
        return escape_az_;
    }
    /// The react time the current own-G capability buys (s).
    [[nodiscard]] double react_time_sec() const noexcept;
    [[nodiscard]] const Config& config() const noexcept { return cfg_; }
    [[nodiscard]] Config&       config()       noexcept { return cfg_; }
    void set_config(const Config& c) noexcept { cfg_ = c; }
    /// Reset (disengage + clean steering state).
    void reset();

    /// The shared AirSteering instance (public fields — the break tune).
    [[nodiscard]] AirSteering& air_steering() noexcept { return air_; }

private:
    [[nodiscard]] AirSteering::Input steering_input(
        const flight::IAircraftState& s) const noexcept;

    Config cfg_{};
    AirSteering air_{};

    std::vector<Intruder> traffic_;
    std::optional<geo::WorldPosition> own_velocity_{};
    std::uint64_t exempt_id_{0};

    bool   avoiding_{false};
    std::uint64_t intruder_id_{0};
    double hold_timer_{0.0};
    double escape_az_{0.0};
    double escape_target_alt_ft_{0.0};
};

} // namespace f4::ai::modules
