// f4-flight-model/aerodynamics.hpp
//
// Aerodynamics force model.
//
// Computes lift, drag, side-force coefficients from the Mach x alpha tables,
// applies ground effect, flap factors, speed-brake/gear drag, and the stall
// model. Produces force ACCELERATIONS (ft/s^2 = force/mass) in body,
// stability, and wind axes.
//
// Ported from F4Flight's aerodynamics.cpp, which is a port of FreeFalcon's
// aero.cpp. Several bugs in the original F4Flight port were fixed by
// comparing against the FreeFalcon source — these fixes are documented
// inline.

#pragma once

#include "f4/flight/aircraft_state.hpp"
#include "f4/data/aircraft_config.hpp"
#include "f4/data/table_accessors.hpp"
#include "f4/math/table.hpp"
#include "f4/flight/constants.hpp"

#include <cmath>

namespace f4::flight {

/// Aerodynamics force model.
///
/// Computes aerodynamic forces from the aircraft's coefficient tables and
/// current flight state. The forces are stored as accelerations (ft/s^2)
/// in the AeroState so the EOM can integrate them directly.
///
/// The model does NOT compute aerodynamic moments (Cm, Cl, Cn). Moments
/// are synthesized by the FCS closed-loop control system — there are no
/// separate moment coefficient tables. This matches the original Falcon 4
/// approach, which uses a command-augmentation system rather than a
/// full 6-DOF moment model.
/// Input parameters for Aerodynamics::update().
///
/// Groups the 11 input values that were previously passed as positional
/// doubles — a transposition (e.g. groundZ_ft where altitude_ft belongs)
/// compiled cleanly and produced silently wrong forces. The in/out
/// AeroState& is kept separate to make the read/write boundary explicit.
struct AeroInputs {
    Angle  alpha{zero_angle()};     // angle of attack
    Angle  beta{zero_angle()};      // sideslip angle
    double mach{0.0};              // Mach number
    double vt_ftps{0.0};          // true airspeed (ft/s)
    double qbar{0.0};             // dynamic pressure (lb/ft^2)
    double qsom{0.0};             // normalized dynamic pressure q*S/m
    double altitude_ft{0.0};      // altitude above sea level (ft)
    double groundZ_ft{0.0};       // terrain altitude at aircraft position (ft)
    double z_ft{0.0};             // aircraft Z position (ft, NED: negative = up)
    double vcas_kts{0.0};         // calibrated airspeed (knots)
    double pstick{0.0};           // pitch stick input (-1..+1, for side-force shaping)
};

class Aerodynamics {
public:
    Aerodynamics() = default;

    /// Construct with pointers to the config tables.
    ///
    /// LIFETIME CONTRACT: The pointed-to objects must outlive this Aerodynamics
    /// instance. In practice, FlightModel stores AircraftConfig as a value member
    /// and subsystems are reconstructed in initSubsystems(), so the contract holds.
    /// Debug builds enforce this via assert() in both the constructor and update().
    ///
    /// Null pointers are rejected via assert().
    Aerodynamics(const data::AeroTable* table,
                 const data::AircraftGeometry* geom,
                 const data::AuxAero* aux);

    /// Recompute aerodynamic forces for one time step.
    ///
    /// Reads the current flight state from `in` and writes the computed
    /// forces and coefficients into `aero` (in/out).
    ///
    /// PRECONDITION: this object was constructed with non-null config pointers.
    /// Violated precondition → assert failure in debug builds.
    void update(const AeroInputs& in, AeroState& aero) const;

private:
    const data::AeroTable*         table_{nullptr};
    const data::AircraftGeometry*  geom_{nullptr};
    const data::AuxAero*           aux_{nullptr};

    /// Cached Table2D views of the CL/CD/CY coefficient tables.
    /// Built once at construction; the cached-index optimization makes
    /// lookups O(1) for the typical smooth-frame-to-frame access pattern.
    math::Table2D<double, double, double> cl_;
    math::Table2D<double, double, double> cd_;
    std::optional<math::Table2D<double, double, double>> cy_;
};

}  // namespace f4::flight
