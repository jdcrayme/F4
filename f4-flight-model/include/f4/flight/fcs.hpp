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

/// Flight-condition parameters passed to FlightControlSystem::update().
///
/// Groups the atmospheric, kinematic, and load-factor state that the FCS
/// reads each frame. Constructed by the flight model from AircraftState
/// fields before calling FCS::update().
struct FlightConditions {
    // --- Atmospheric / airspeed ---
    double qbar{0.0};            // dynamic pressure (lb/ft²)
    double qsom{0.0};            // normalized dynamic pressure
    double mach{0.0};            // Mach number
    double vt{0.0};              // true airspeed (ft/s)
    double vcas{0.0};            // calibrated airspeed (knots)

    // --- Angle of attack / sideslip ---
    Angle alpha{zero_angle()};   // angle of attack
    Angle beta{zero_angle()};    // sideslip
    double sinalp{0.0};          // sin(alpha)
    double cosalp{1.0};          // cos(alpha)
    double sinbet{0.0};          // sin(beta)
    double cosbet{1.0};          // cos(beta)

    // --- Attitude trig (velocity vector) ---
    double cosmu{1.0};           // cos(velocity roll)
    double cosgam{1.0};          // cos(flight path angle)
    double singam{0.0};          // sin(flight path angle)

    // --- Attitude trig (body) ---
    double costhe{1.0};          // cos(body pitch)
    double cosphi{1.0};          // cos(body roll)
    Angle phi{zero_angle()};     // body roll angle

    // --- Loading / status ---
    double loadingFraction{1.0}; // weight / emptyWeight
    bool inAir{true};            // airborne flag

    // --- Load factors ---
    double nzcgs{0.0};          // stability-axis normal load factor
    double nycgw{0.0};          // wind-axis side load factor
};

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
    /// Reads pilot input and current flight conditions, computes gains,
    /// then runs the pitch/roll/yaw channels. Writes commanded alpha,
    /// beta, and roll rate into `aeroState` and `fcsState`.
    ///
    ///   pilot     : pilot input (stick, pedal, refueling flag)
    ///   fc        : flight-condition snapshot (atmosphere, kinematics, loads)
    ///   fcsState  : [in,out] FCS state (filter states, gains)
    ///   aeroState : [in,out] aero state (reads gearPos, writes alpha, beta)
    ///   dt        : time step (seconds)
    void update(const PilotInput& pilot,
               const FlightConditions& fc,
               FcsState& fcsState,
               AeroState& aeroState,
               double dt) const;

    /// Apply a named limiter to input x. Returns the limited value.
    double applyLimiter(data::LimiterKey key, double x) const;

private:
    /// Compute FCS gains (pitch/roll/yaw) from current flight state.
    /// This is the port of FreeFalcon's gain.cpp.
    void computeGains(double qbar, double qsom, double vt, Angle alpha,
                      double clift0, double clalph0, double clalpha, double cnalpha,
                      double cy,
                      double cosgam, double cosmu, double costhe, double cosphi,
                      double loadingFraction, bool inAir,
                      bool landingGains, double gearPos,
                      FcsState& fcs) const;

    /// Run the pitch channel: G-command PI controller with anti-windup.
    void runPitch(double dt, double qbar, double qsom, double vt, double vcas_kts,
                  Angle alpha, double cosmu, double cosgam, double singam,
                  double nzcgs, double cl, double clalpha, double clalph0,
                  double cnalpha, double aoamin, double aoamax, double maxGs,
                  const PilotInput& input,
                  FcsState& fcs, AeroState& aero) const;

    /// Run the roll channel: rate command with alpha-based rate limiting.
    void runRoll(double dt, double qbar, double vcas_kts, Angle alpha,
                 double gearPos, Angle phi,
                 const PilotInput& input,
                 FcsState& fcs) const;

    /// Run the yaw channel: beta-command PI (mostly stubbed).
    void runYaw(double dt, double qbar, double qsom, double vt, double vcas_kts,
                Angle beta, double nycgw, double betmin, double betmax,
                const PilotInput& input,
                FcsState& fcs, AeroState& aero) const;

    const data::AircraftConfig*   cfg_{nullptr};
    const data::AircraftGeometry* geom_{nullptr};
    const data::AuxAero*          aux_{nullptr};

    /// Cached roll-rate command table (alpha x qbar -> rollRate deg/s).
    std::optional<math::Table2D<double, double, double>> rollCmdTable_;
};

}  // namespace f4::flight
