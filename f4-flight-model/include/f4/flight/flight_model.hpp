// f4-flight-model/flight_model.hpp
//
// Top-level flight model orchestrator.
//
// Owns all subsystems (atmosphere, aero, FCS, engine, gear, EOM) and runs
// the per-frame update. The host program constructs one FlightModel per
// aircraft, calls init() once, then calls update() every frame.
//
// Ported from F4Flight's flight_model.h/cpp, which is a port of FreeFalcon's
// AirframeClass::Exec().

#pragma once

#include "f4/flight/aerodynamics.hpp"
#include "f4/flight/aircraft_state.hpp"
#include "f4/flight/atmosphere.hpp"
#include "f4/flight/constants.hpp"
#include "f4/flight/engine.hpp"
#include "f4/flight/eom.hpp"
#include "f4/flight/fcs.hpp"
#include "f4/flight/gear.hpp"
#include "f4/data/aircraft_config.hpp"

namespace f4::flight {

/// Flight model orchestrator. Construct one per aircraft.
///
/// Usage:
///   FlightModel fm;
///   fm.init(config, initialAltitude_ft, initialVt_ftps, initialHeading_rad, inAir);
///   while (running) {
///       fm.update(dt, pilotInput, groundZ_ft, groundNormal);
///       // read fm.state() for results
///   }
class FlightModel {
public:
    FlightModel();

    /// Initialise the flight model with the given configuration.
    ///
    ///   initialAltitude_ft : initial altitude AGL (feet, positive up)
    ///   initialVt_ftps     : initial true airspeed (ft/s)
    ///   initialHeading_rad : initial heading (radians, 0 = North)
    ///   inAir              : if true, start airborne; otherwise on ground
    void init(const data::AircraftConfig& cfg,
              double initialAltitude_ft,
              double initialVt_ftps,
              double initialHeading_rad,
              bool inAir);

    /// Run the per-frame update.
    ///
    /// The host calls this once per major frame. The model sub-steps
    /// internally at the minor-frame rate (default: 6 sub-steps per major
    /// frame, giving a 360 Hz minor frame at 60 Hz major).
    ///
    ///   dt           : major-frame step (seconds)
    ///   input        : pilot/AI input
    ///   groundZ_ft   : terrain altitude at the aircraft position (ft)
    ///   groundNormal : up vector at the terrain point (unit, world frame)
    void update(double dt, const PilotInput& input,
                double groundZ_ft, const math::Vec3d& groundNormal);

    // --- Accessors ---
    const AircraftState& state() const noexcept { return state_; }
    AircraftState&       state()       noexcept { return state_; }
    const data::AircraftConfig& config() const noexcept { return cfg_; }
    const FlightControlSystem& fcs() const noexcept { return fcs_; }

    // --- Sub-stepping controls ---
    void   setMinorPerMajor(int n) noexcept { minorPerMajor_ = std::max(1, n); }
    int    minorPerMajor() const noexcept { return minorPerMajor_; }
    double minorFrameTime() const noexcept { return minorFrameTime_; }

    /// Set the ground state (terrain altitude + normal).
    void setGround(double groundZ_ft, const math::Vec3d& normal) {
        state_.gear.groundZ_ft = groundZ_ft;
        state_.gear.groundNormal = normal;
    }

    /// Recompute load factors from the current force state.
    void computeLoadFactors();

    /// Trim the aircraft at the current state.
    ///
    /// Iterates alpha and throttle to find a steady-state 1-G condition.
    /// Returns true on convergence, false if the trim solver didn't converge
    /// within the iteration limit.
    bool trim();

private:
    /// Run one minor-frame step.
    void minorStep(double dt, const PilotInput& input);

    /// Compute atmosphere + update aerodynamic state fields.
    void updateAtmosphere();

    /// Recompute strut compression and gear-on-ground flags.
    void updateGear(double dt);

    /// Compute load factors (G) from current force state.
    void accelerometers();

    data::AircraftConfig cfg_;
    AircraftState        state_;

    Aerodynamics          aero_;
    EngineModel           engine_;
    FlightControlSystem   fcs_;
    GearModel             gear_;
    EquationsOfMotion     eom_;

    /// Cached copy of the most recent PilotInput (needed by updateGear
    /// which runs before minorStep but needs the brake handle state).
    PilotInput lastInput_{};

    int    minorPerMajor_{6};
    double minorFrameTime_{1.0 / 360.0};  // 6 sub-steps of 1/60s = 1/10s major
};

}  // namespace f4::flight
