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
class Aerodynamics {
public:
    Aerodynamics() = default;

    /// Construct with pointers to the config tables.
    /// The pointers must remain valid for the lifetime of this object.
    Aerodynamics(const data::AeroTable* table,
                 const data::AircraftGeometry* geom,
                 const data::AuxAero* aux);

    /// Recompute aerodynamic forces for one time step.
    ///
    /// Reads the current flight state (alpha, beta, mach, etc.) and writes
    /// the computed forces and coefficients into `aero`.
    ///
    ///   alpha_deg   : angle of attack (degrees)
    ///   beta_deg    : sideslip angle (degrees)
    ///   mach        : Mach number
    ///   vt_ftps     : true airspeed (ft/s)
    ///   qbar        : dynamic pressure (lb/ft^2)
    ///   qsom        : normalized dynamic pressure q*S/m (ft/s^2 per unit CL)
    ///   altitude_ft : altitude above sea level (ft)
    ///   groundZ_ft  : terrain altitude at aircraft position (ft)
    ///   z_ft        : aircraft Z position (ft, NED: negative = up)
    ///   vcas_kts    : calibrated airspeed (knots)
    ///   pstick      : pitch stick input (-1..+1, for side-force shaping)
    ///   aero        : [in,out] aero state (reads tefPos/lefPos/dbrake/gearPos,
    ///                 writes cl/cd/cy/forces/stall)
    void update(double alpha_deg,
                double beta_deg,
                double mach,
                double vt_ftps,
                double qbar,
                double qsom,
                double altitude_ft,
                double groundZ_ft,
                double z_ft,
                double vcas_kts,
                double pstick,
                AeroState& aero) const;

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
