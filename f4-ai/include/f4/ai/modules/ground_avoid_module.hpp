// f4-ai/include/f4/ai/modules/ground_avoid_module.hpp
//
// GroundAvoidModule — terrain-avoidance pull-up, the DigitalBrain priority
// ladder's rung 1 (AI_IMPLEMENTATION_PLAN.md §3.3; FreeFalcon runs it as the
// "Actions overlay" rather than a mode — the reference's FrameExec step 13
// consults groundAvoidNeeded, and wvrengage.cpp bails out of tactic choices
// while it is set: ground avoid preempts EVERYTHING except takeoff).
//
// FreeFalcon tuning constants (reference §18.3, ported verbatim where they
// have a home here):
//   MIN_ALTT       = 1500 ft   -> Config::min_clearance_ft (combat
//                                 clearance floor)
//   g_fGALookAhead -> Config::lookahead_sec (how far ahead of the jet the
//                                 terrain picture must be probed)
//   g_fPullupTime  -> Config::pullup_hold_sec (the pullupTimer — how long
//                                 the recovery CONTINUES after the threat
//                                 clears, so the jet does not snap back to
//                                 level flight the instant the probe goes
//                                 clean)
//
// The module is a pure function of three inputs: the ownship state, the
// host's TERRAIN PICTURE, and time. The picture (TerrainPicture below) is
// pushed every tick by the host — the engine-agnostic contract means the
// module cannot query terrain itself:
//
//   terrain_here_ft  — elevation under the jet right now (MSL ft)
//   terrain_ahead_ft — MAXIMUM elevation within the look-ahead cone along
//                      the ground track (the host samples the same
//                      TerrainSource the FM's ground plane uses — one
//                      source of truth for both the ground and the brain)
//
// Decision (both terms, whichever is worse):
//   clearance_now  = alt - terrain_here
//   clearance_pred = (alt + sink*lookahead) - terrain_ahead
//                    (sink = the DOWNWARD vertical speed only — a climb
//                    never triggers the predictor; a 6,000 fpm dive over
//                    flat ground at 2,000 ft is a ground-avoid event)
//   pull up when min(clearance_now, clearance_pred) < min_clearance_ft,
//   release when it rises above min_clearance + clear_margin (hysteresis —
//   FreeFalcon's pullupTimer does the same job with a clock; we keep both:
//   margin AND hold, because a ridge line can oscillate a pure-margin gate).
//
// Recovery maneuver (the classic GPWS/terrain escape): hold the current
// heading (roll toward level, bank capped low), full throttle (AB is legal
// — FrameExec's "no AB" rule is about fuel emergencies, not terrain), climb
// hard toward terrain_ahead + 2*min_clearance. Output has has_override=true:
// the brain must fly this verbatim, preempting every other rung.
//
// Gating (FreeFalcon DecisionLogic's rule 1: GroundCheck runs "if not
// LandingMode"): the brain only consults this module while Enroute. The
// landing module already owns the last 1,500 ft on final; a pull-up there
// would fight the approach. On the ground the module idles.
//
// ENGINE-AGNOSTIC CONTRACT: no world, no bus, no terrain types. The host
// configures, feeds the picture, and reads the output.
//
// Dependencies: f4-flight-api (IAircraftState), f4-geo, f4-ai
// (AirSteering, AIControlOutput). C++20.

#pragma once

#include <cstdint>

#include <f4/flight/api/i_aircraft_state.hpp>
#include <f4/geo/position.hpp>

#include "f4/ai/ai_output.hpp"
#include "f4/ai/air_steering.hpp"

namespace f4::ai::modules {

class GroundAvoidModule {
public:
    /// Tuning (FreeFalcon reference §18.3 + the pull-up doctrine).
    struct Config {
        /// MIN_ALTT: minimum terrain clearance in combat (ft).
        double min_clearance_ft{1500.0};
        /// g_fGALookAheadTime: how far ahead the picture is probed (s).
        double lookahead_sec{6.0};
        /// g_fPullupTime: keep flying the recovery this long after the
        /// threat clears (s) — the pullupTimer.
        double pullup_hold_sec{3.0};
        /// Release margin above min_clearance (ft) — hysteresis.
        double clear_margin_ft{750.0};
        /// Recovery climb target: terrain_ahead + this multiple of the
        /// clearance floor (ft). 2x = well clear before resuming.
        double climb_clearance_mult{2.0};
        /// Escape speed (KCAS) — corner-ish: fast enough to convert to
        /// climb rate, slow enough to stay inside the envelope.
        double escape_speed_kts{400.0};
        /// VS cap during the recovery (fpm). The AirSteering default
        /// (2,500) is a nav-comfort cap; a terrain escape commands more.
        double max_vs_fpm{10000.0};
        /// Bank cap during the recovery (rad). Roll toward level, do not
        /// turn — the escape is vertical.
        double max_bank_rad{0.35};
        /// Master arm (host/test A-B switch; scenarios can disarm the
        /// rung the way they disarm combat).
        bool enabled{true};
    };

    /// The host's per-tick terrain picture (see header comment). valid
    /// defaults false: a host that never pushes a picture gets a module
    /// that never pulls (the standalone-brain case — unit tests of other
    /// modules are not terrain tests).
    struct TerrainPicture {
        bool   valid{false};
        double terrain_here_ft{0.0};
        double terrain_ahead_ft{0.0};
    };

    GroundAvoidModule() = default;

    /// One tick of the ground-avoid rung. Returns the recovery output
    /// (has_override=true while pulling up) or an empty output.
    AIControlOutput update(double dt, const flight::IAircraftState* state,
                           const TerrainPicture& picture);

    // --- State ---
    /// True while the module is flying the recovery (incl. the hold).
    [[nodiscard]] bool pulling_up() const noexcept { return pulling_; }
    /// FreeFalcon's groundAvoidNeeded — the gate other logic consults.
    [[nodiscard]] bool ground_avoid_needed() const noexcept {
        return pulling_;
    }
    /// Last computed clearance (ft; the worse of now/predicted). Exposed
    /// for tests + the FCS trace exporter. Large positive when parked.
    [[nodiscard]] double clearance_ft() const noexcept {
        return clearance_ft_;
    }
    [[nodiscard]] const Config& config() const noexcept { return cfg_; }
    [[nodiscard]] Config&       config()       noexcept { return cfg_; }
    void set_config(const Config& c) noexcept { cfg_ = c; }
    /// Reset (disengage + clean steering state).
    void reset();

    /// The shared AirSteering instance (public fields — the recovery tune).
    [[nodiscard]] AirSteering& air_steering() noexcept { return air_; }

private:
    [[nodiscard]] AirSteering::Input steering_input(
        const flight::IAircraftState& s) const noexcept;

    Config cfg_{};
    AirSteering air_{};

    bool   pulling_{false};
    double hold_timer_{0.0};
    double clearance_ft_{1.0e6};
};

} // namespace f4::ai::modules
