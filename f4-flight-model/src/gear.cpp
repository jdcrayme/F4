// f4-flight-model/gear.cpp
//
// Gear / ground model implementation.
//
// Ported from F4Flight's gear.cpp.

#include "f4/flight/gear.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace f4::flight {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------
GearModel::GearModel(const data::AircraftGeometry* geom, const data::AuxAero* aux)
    : geom_(geom), aux_(aux) {
    assert(geom_ != nullptr && "GearModel: geom must not be null");
    assert(aux_  != nullptr && "GearModel: aux must not be null");
}

// ---------------------------------------------------------------------------
// init: size the wheels vector to match the config's gear points.
// ---------------------------------------------------------------------------
void GearModel::init(GearState& gear) const {
    if (!geom_) return;
    gear.wheels.resize(geom_->gear.size());
    for (auto& w : gear.wheels) {
        w.strutCompression_ft = 0.0;
        w.strutVel_fps = 0.0;
        w.wheelAngle_rad = 0.0;
        w.onGround = false;
        w.broken = false;
        w.stuck = false;
    }
}

// ---------------------------------------------------------------------------
// computeMinHeight: the lowest point of the aircraft when gear is extended.
//
// This is the maximum gear strut extended length (z is positive downward
// in body frame). The EOM ground clamp uses this to keep the body at the
// correct height above terrain.
// ---------------------------------------------------------------------------
double GearModel::computeMinHeight(const GearState& gear, double gearPos) const {
    (void)gear;
    if (!geom_ || geom_->gear.empty()) return 0.0;
    double h = 0.0;
    for (const auto& gp : geom_->gear) {
        if (gp.z > h) h = gp.z;  // z is strut extended length (positive down)
    }
    return h * gearPos;  // gear-up = 0 clearance, gear-down = full strut
}

// ---------------------------------------------------------------------------
// calcMuFric: compute the rolling friction coefficient.
//
//   onObject     : carrier deck / hard surface -> very high friction (20.0)
//   parkingBrake : parked -> high friction (0.7)
//   wheelBrakes  : braking -> high friction (0.7)
//   overRunway   : paved surface -> low rolling resistance (0.04)
//   else         : grass / dirt -> higher resistance (0.5)
// ---------------------------------------------------------------------------
double GearModel::calcMuFric(bool wheelBrakes, bool parkingBrake,
                              bool onObject, bool overRunway) {
    if (onObject)     return MU_CARRIER;  // carrier deck (effectively infinite)
    if (parkingBrake) return MU_BRAKING;
    if (wheelBrakes)  return MU_BRAKING;
    if (overRunway)   return MU_PAVED;  // paved
    return MU_GRASS;                      // grass / dirt
}

// ---------------------------------------------------------------------------
// updateStrutCompression: compute strut compression for each wheel.
//
// For each gear point, the strut compression is:
//   compression = strutMax - AGL
// where strutMax is the extended strut length and AGL is the aircraft's
// height above ground. Clamped to [0, strutMax].
// ---------------------------------------------------------------------------
void GearModel::updateStrutCompression(GearState& gear,
                                        double groundZ_ft, double z_ft,
                                        double vt_ftps, double dt) const {
    if (!geom_) return;

    const double agl_ft = std::fabs(groundZ_ft - z_ft);

    for (std::size_t i = 0; i < gear.wheels.size() && i < geom_->gear.size(); ++i) {
        const double strutMax = geom_->gear[i].z;  // extended length (positive down)
        double compression = strutMax - agl_ft;
        compression = std::clamp(compression, 0.0, strutMax);

        auto& w = gear.wheels[i];
        w.strutCompression_ft = compression;
        w.onGround = (compression > 0.0);

        // Wheel spin (visual only)
        const double wheelRadius = std::max(0.5, strutMax * 0.3);
        w.wheelAngle_rad += (vt_ftps * dt) / wheelRadius;
        if (w.wheelAngle_rad > TWO_PI) w.wheelAngle_rad -= TWO_PI;
        if (w.wheelAngle_rad < 0.0)    w.wheelAngle_rad += TWO_PI;
    }
}

// ---------------------------------------------------------------------------
// updateGearPos: actuate the gear toward the handle command.
//
// The gear moves at a constant rate (GEAR_RATE = 1/3 per second, so full
// travel takes 3 seconds). When the remaining distance is less than one
// step, snap to the target.
// ---------------------------------------------------------------------------
double GearModel::updateGearPos(double& gearPos_state, double gearHandle, double dt) const {
    const double target = (gearHandle > 0.0) ? 1.0 : 0.0;
    const double diff = target - gearPos_state;
    const double step = GEAR_RATE * dt;

    if (std::fabs(diff) < step) {
        gearPos_state = target;  // snap to target
    } else {
        gearPos_state += (diff > 0.0 ? step : -step);
    }
    return gearPos_state;
}

}  // namespace f4::flight
