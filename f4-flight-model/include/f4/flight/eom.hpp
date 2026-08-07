// f4-flight-model/eom.hpp
//
// Equations of motion: rigid-body 6-DOF integration.
//
// Integrates the aircraft state (position, velocity, orientation, body rates)
// forward in time using the aerodynamic and thrust forces computed by the
// other subsystems.
//
// The integration uses the exponential map on SO(3) for the quaternion
// orientation (geodesic-preserving, no drift off the unit sphere), and
// Forward Euler for position/velocity. The FCS integrators (in fcs.cpp)
// use Adams-Bashforth 2nd order.
//
// Coordinate frames:
//   World: NED (North-East-Down), Z-down, feet. Altitude = -z.
//   Body:  X-forward, Y-right, Z-down.
//   alpha, beta in DEGREES; euler angles in RADIANS; body rates in rad/s.
//
// Ported from F4Flight's eom.cpp, which is a port of FreeFalcon's eom.cpp.

#pragma once

#include "f4/flight/aircraft_state.hpp"
#include "f4/flight/constants.hpp"
#include "f4/data/aircraft_config.hpp"

namespace f4::flight {

/// Equations of motion: integrates the rigid-body 6-DOF state.
class EquationsOfMotion {
public:
    EquationsOfMotion() = default;

    /// Construct with config pointers. Pointers must remain valid for the
    /// lifetime of this object. Null pointers are rejected via assert().
    explicit EquationsOfMotion(const data::AircraftGeometry* geom,
                               const data::AuxAero* aux);

    /// Run the full EOM update for one time step.
    ///
    /// Reads the current forces (in state.aero and state.engine) and
    /// integrates the rigid-body state forward by dt.
    ///
    ///   dt    : step size (seconds)
    ///   input : pilot input (for nose-wheel steering on ground)
    ///   state : [in,out] full aircraft state
    void update(double dt, const PilotInput& input, AircraftState& state) const;

private:
    /// Cached trig values consumed by calcBodyRates.
    /// Groups the 6 sin/cos values that were previously passed as 6
    /// positional doubles — a transposition (e.g. cosmu where cosgam
    /// belongs) compiled cleanly and produced silently wrong body rates.
    struct TrigCache {
        double cosmu{1.0};
        double cosgam{1.0};
        double singam{0.0};
        double cosbet{1.0};
        double cosalp{1.0};
        double sinalp{0.0};
    };

    /// Load factors consumed by calcBodyRates.
    /// Groups the load-factor values previously passed as positional
    /// doubles alongside the roll rate.
    struct LoadFactors {
        double nzcgs{0.0};   // stability-axis normal load factor
        double nycgw{0.0};   // wind-axis side load factor
        double pstab{0.0};   // filtered roll rate (rad/s, from FCS)
    };

    /// Compute body rates (p, q, r) from the commanded G and roll rate.
    void calcBodyRates(double dt, double qsom, double cnalpha,
                       const TrigCache& trig,
                       const LoadFactors& loads,
                       double pitchMomentum, double pitchElasticity,
                       AircraftState& state) const;

    /// Integrate the quaternion orientation from body rates.
    void calcBodyOrientation(double dt, AircraftState& state) const;

    /// Recompute the kinematic trig cache (sin/cos of all angles).
    void trigonometry(AircraftState& state) const;

    /// Integrate true airspeed, accounting for gravity and ground friction.
    void calculateVt(double dt, double muFric, double singam,
                     double xwaero, double xwprop,
                     AircraftState& state) const;

    /// Integrate world-frame position and apply ground clamp.
    void integratePosition(double dt, double cosgam, double singam,
                           double cossig, double sinsig,
                           double windX, double windY,
                           AircraftState& state) const;

    const data::AircraftGeometry* geom_{nullptr};
    const data::AuxAero*          aux_{nullptr};
};

}  // namespace f4::flight
