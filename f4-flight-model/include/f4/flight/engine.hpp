// f4-flight-model/engine.hpp
//
// Turbofan engine model with afterburner.
//
// Computes thrust, RPM, and fuel flow from the engine thrust tables, with:
//   - 2-D altitude x Mach thrust tables (3 power settings: idle, mil, AB)
//   - Spool dynamics via first-order lag
//   - Per-engine-type RPM schedules (PW-100/220/229, GE-110/129)
//   - Fuel flow from separate tables or legacy factor model
//
// Ported from F4Flight's engine.cpp, which is a port of FreeFalcon's
// engine.cpp.
//
// All Imperial units (ft, lb, slugs, lb/hr).

#pragma once

#include "f4/flight/aircraft_state.hpp"
#include "f4/data/aircraft_config.hpp"
#include "f4/data/table_accessors.hpp"
#include "f4/math/table.hpp"
#include "f4/flight/constants.hpp"

#include <optional>

namespace f4::flight {

/// Engine model for a single engine.
///
/// The FlightModel owns one EngineModel instance; multi-engine aircraft
/// (B-52, C-130) use a single RPM state with nEngines folded into the
/// thrust multiplicatively. Per-engine flameout modeling is a future
/// enhancement.
class EngineModel {
public:
    EngineModel() = default;

    /// Construct with config pointers. Pointers must remain valid.
    explicit EngineModel(const data::EngineTable* table,
                         const data::AuxAero* aux);

    /// Recompute thrust, RPM, and fuel flow for one time step.
    ///
    ///   dt         : step size (seconds)
    ///   alt_ft     : altitude (feet, positive upward)
    ///   mach       : Mach number
    ///   vt_ftps    : true airspeed (ft/s)
    ///   mass_slugs : current aircraft mass (slugs)
    ///   throttle   : 0..1.5 (1.0 = MIL, 1.5 = full AB)
    ///   ethrst     : thrust-reverse coefficient (1.0 = no reverse)
    ///   simplified : if true, scale fuel flow by 0.75 (for AI)
    ///   state      : [in,out] engine state (RPM, thrust, fuel flow, etc.)
    void update(double dt,
                double alt_ft,
                double mach,
                double vt_ftps,
                double mass_slugs,
                double throttle,
                double ethrst,
                bool   simplified,
                EngineState& state);

    /// Compute body-axis thrust forces (ft/s^2) from thrust acceleration.
    /// Used by the FlightModel to augment the aerodynamic forces.
    static void bodyForces(double thrust_accel,
                           double sinAlpha,
                           double cosAlpha,
                           double nozzlePos,
                           double& xprop,
                           double& yprop,
                           double& zprop,
                           double& xsprop,
                           double& zsprop);

    /// Return the last computed fuel flow from the given engine state.
    /// This is a convenience accessor — fuel flow is stored in EngineState
    /// (per-aircraft mutable state), not in EngineModel.
    static double fuelFlow(const EngineState& state) noexcept { return state.fuelFlow; }

private:
    /// Per-engine-type RPM schedule modifications.
    /// Adjusts the commanded RPM based on engine type (PW-100/220/229,
    /// GE-110/129) and flight conditions (altitude, Mach, airspeed).
    double engineRpmMods(double rpmCmd, double alt_ft, double mach, double vcas) const noexcept;

    const data::EngineTable* table_{nullptr};
    const data::AuxAero*     aux_{nullptr};

    /// Cached Table2D views of the thrust and fuel-flow tables.
    /// thrustAb_ is only built if the aircraft has afterburner; check
    /// hasAB_ before using it.
    math::Table2D<double, double, double> thrustIdle_;
    math::Table2D<double, double, double> thrustMil_;
    math::Table2D<double, double, double> thrustAb_;
    math::Table2D<double, double, double> ffIdle_;
    math::Table2D<double, double, double> ffMil_;
    math::Table2D<double, double, double> ffAb_;
    bool hasAB_{false};
    bool hasFuelFlowTables_{false};
};

}  // namespace f4::flight
