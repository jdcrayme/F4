// f4-flight-model/fcs.hpp
//
// Flight Control System: F-16-like AOA / G command system with limiters.
//
// The FCS translates pilot stick/pedal inputs into commanded alpha, beta,
// and roll rate. It uses a closed-loop control system with:
//   - Pitch: G-command PI controller with anti-windup, lead-lag filter
//   - Roll: Rate command with alpha-based rate limiting
//   - Yaw: Beta-command PI controller (mostly stubbed — the EOM has no
//     rudder-to-yaw dynamics, so beta is forced to 0)
//
// Ported from F4Flight's fcs.cpp, which is a port of FreeFalcon's
// fcs.cpp, gain.cpp, pitch.cpp, roll.cpp, yaw.cpp.

#pragma once

#include "f4/flight/aircraft_state.hpp"
#include "f4/flight/constants.hpp"
#include "f4/data/aircraft_config.hpp"
#include "f4/data/table_accessors.hpp"
#include "f4/math/table.hpp"

#include <optional>

namespace f4::flight {

/// Flight Control System.
///
/// Translates pilot inputs into commanded aerodynamic state (alpha, beta,
/// roll rate). The FCS runs once per minor frame, after the atmosphere
/// update and before the aerodynamics update.
class FlightControlSystem {
public:
    FlightControlSystem() = default;

    /// Construct with config pointers. Pointers must remain valid.
    FlightControlSystem(const data::AircraftConfig* cfg,
                        const data::AircraftGeometry* geom,
                        const data::AuxAero* aux);

    /// Run the full FCS update for one time step.
    ///
    /// Reads pilot input and current flight state, computes gains, then
    /// runs the pitch/roll/yaw channels. Writes commanded alpha, beta,
    /// and roll rate into `aero` and `fcs`.
    ///
    ///   dt                  : time step (seconds)
    ///   qbar, qsom, mach    : current atmosphere outputs
    ///   vt_ftps, vcas_kts   : true/calibrated airspeed
    ///   alpha_deg, beta_deg : current alpha/beta
    ///   cosmu, cosgam       : velocity-vector trig
    ///   singam              : sin(flight path angle)
    ///   costhe, cosphi      : body attitude trig
    ///   phi_rad             : body roll angle (radians)
    ///   loadingFraction     : weight / emptyWeight (affects gains)
    ///   inAir               : airborne flag (affects ground fade)
    ///   nzcgs, nycgw        : current load factors (feedback)
    ///   gearDown            : gear is down (enables landing gains)
    ///   refueling           : refueling mode (enables landing gains)
    ///   landingGainsActive  : explicitly request landing gains
    ///   input               : pilot input
    ///   fcs                 : [in,out] FCS state (filter states, gains)
    ///   aero                : [in,out] aero state (reads gearPos, writes
    ///                         alpha_deg, beta_deg)
    void update(double dt,
                double qbar,
                double qsom,
                double mach,
                double vt_ftps,
                double vcas_kts,
                double alpha_deg,
                double beta_deg,
                double cosmu,
                double cosgam,
                double singam,
                double costhe,
                double cosphi,
                double phi_rad,
                double loadingFraction,
                bool   inAir,
                double nzcgs,
                double nycgw,
                bool   gearDown,
                bool   refueling,
                bool   landingGainsActive,
                const PilotInput& input,
                FcsState& fcs,
                AeroState& aero) const;

    /// Apply a named limiter to input x. Returns the limited value.
    double applyLimiter(data::LimiterKey key, double x) const;

private:
    /// Compute FCS gains (pitch/roll/yaw) from current flight state.
    /// This is the port of FreeFalcon's gain.cpp.
    void computeGains(double qbar, double qsom, double vt, double alpha_deg,
                      double clift0, double clalph0, double clalpha, double cnalpha,
                      double cy,
                      double cosgam, double cosmu, double costhe, double cosphi,
                      double loadingFraction, bool inAir,
                      bool landingGains, double gearPos,
                      FcsState& fcs) const;

    /// Run the pitch channel: G-command PI controller with anti-windup.
    void runPitch(double dt, double qbar, double qsom, double vt, double vcas_kts,
                  double alpha_deg, double cosmu, double cosgam, double singam,
                  double nzcgs, double cl, double clalpha, double clalph0,
                  double cnalpha, double aoamin, double aoamax, double maxGs,
                  const PilotInput& input,
                  FcsState& fcs, AeroState& aero) const;

    /// Run the roll channel: rate command with alpha-based rate limiting.
    void runRoll(double dt, double qbar, double vcas_kts, double alpha_deg,
                 double gearPos, double phi_rad,
                 const PilotInput& input,
                 FcsState& fcs) const;

    /// Run the yaw channel: beta-command PI (mostly stubbed).
    void runYaw(double dt, double qbar, double qsom, double vt, double vcas_kts,
                double beta_deg, double nycgw, double betmin, double betmax,
                const PilotInput& input,
                FcsState& fcs, AeroState& aero) const;

    const data::AircraftConfig*   cfg_{nullptr};
    const data::AircraftGeometry* geom_{nullptr};
    const data::AuxAero*          aux_{nullptr};

    /// Cached roll-rate command table (alpha x qbar -> rollRate deg/s).
    std::optional<math::Table2D<double, double, double>> rollCmdTable_;
};

}  // namespace f4::flight
