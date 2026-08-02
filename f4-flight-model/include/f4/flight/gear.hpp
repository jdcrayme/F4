// f4-flight-model/gear.hpp
//
// Gear / ground model.
//
// Computes strut compression, gear position (up/down actuation), and ground
// friction. Does NOT generate ground reaction forces — the EOM's ground
// clamp handles contact. Strut compression is primarily for visuals; the
// onGround flags drive the in-air <-> ground transition.
//
// Ported from F4Flight's gear.cpp.

#pragma once

#include "f4/flight/aircraft_state.hpp"
#include "f4/flight/constants.hpp"
#include "f4/data/aircraft_config.hpp"

namespace f4::flight {

/// Gear / ground model.
class GearModel {
public:
    GearModel() = default;

    /// Construct with config pointers. Pointers must remain valid.
    GearModel(const data::AircraftGeometry* geom, const data::AuxAero* aux);

    /// Initialize the gear state (sizes the wheels vector).
    void init(GearState& gear) const;

    /// Compute the minimum body clearance (the lowest point of the aircraft
    /// when gear is at the given position). Used by the EOM ground clamp.
    double computeMinHeight(const GearState& gear, double gearPos) const;

    /// Compute the friction coefficient from brake/ground state.
    static double calcMuFric(bool wheelBrakes, bool parkingBrake,
                             bool onObject, bool overRunway);

    /// Update strut compression for each wheel.
    void updateStrutCompression(GearState& gear,
                                double groundZ_ft, double z_ft,
                                double vt_ftps, double dt) const;

    /// Actuate the gear position toward the handle command.
    ///   gearPos_state : [in,out] current gear position (0..1)
    ///   gearHandle    : pilot input (-1 = up, +1 = down)
    ///   dt            : time step
    /// Returns the new gear position.
    double updateGearPos(double& gearPos_state, double gearHandle, double dt) const;

private:
    const data::AircraftGeometry* geom_{nullptr};
    const data::AuxAero*          aux_{nullptr};
};

}  // namespace f4::flight
