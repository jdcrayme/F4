// f4-flight-model/engine.cpp
//
// Engine model implementation.
//
// Ported from F4Flight's engine.cpp, which is a port of FreeFalcon's
// engine.cpp.
//
// Key design decisions (documented from F4Flight's bug fixes):
//   - RPM branch on RPM value (not throttle): slamming throttle to AB
//     must wait for RPM to cross 1.0 before the AB branch engages.
//   - Spool altitude sign: +alt/25000 - mach/2 (NOT -alt/25000).
//   - Thrust is stored as an acceleration (lbf/slug = ft/s^2).
//   - nEngines folded into thrust multiplicatively (single RPM state).

#include "f4/flight/engine.hpp"

#include <algorithm>
#include <cmath>

namespace f4::flight {

using namespace f4::data;
using f4::math::Table2D;
using f4::math::BoundaryMode;

// ---------------------------------------------------------------------------
// Construction: build Table2D views from the config's raw thrust/fuelflow vectors.
// ---------------------------------------------------------------------------
EngineModel::EngineModel(const EngineTable* table, const AuxAero* aux)
    : table_(table), aux_(aux) {
    if (table_) {
        hasAB_ = table_->hasAB();
        if (!table_->thrust_idle.empty()) {
            thrustIdle_ = makeThrustTable(*table_, ThrustTable::Idle);
        }
        if (!table_->thrust_mil.empty()) {
            thrustMil_ = makeThrustTable(*table_, ThrustTable::Mil);
        }
        if (!table_->thrust_ab.empty() && hasAB_) {
            thrustAb_ = makeThrustTable(*table_, ThrustTable::AB);
        }
        if (table_->hasFuelFlow()) {
            hasFuelFlowTables_ = true;
            using FlatTag = Table2D<double, double, double>::FlatDataTag;
            ffIdle_ = Table2D<double, double, double>(
                table_->alt_ft, table_->mach, table_->fuelflow_idle,
                FlatTag{}, BoundaryMode::Clamp);
            ffMil_ = Table2D<double, double, double>(
                table_->alt_ft, table_->mach, table_->fuelflow_mil,
                FlatTag{}, BoundaryMode::Clamp);
            ffAb_ = Table2D<double, double, double>(
                table_->alt_ft, table_->mach, table_->fuelflow_ab,
                FlatTag{}, BoundaryMode::Clamp);
        }
    }
}

// ---------------------------------------------------------------------------
// engineRpmMods: per-engine-type RPM schedule.
//
// Different engine types (PW-100/220/229, GE-110/129) have different
// RPM schedules at various altitudes and Mach numbers. This adjusts the
// commanded RPM based on the engine type and flight conditions.
//
// Ported from FreeFalcon engine.cpp's engineRpmMods().
// ---------------------------------------------------------------------------
double EngineModel::engineRpmMods(double rpmCmd, double alt_ft,
                                   double mach, double vcas) const noexcept {
    (void)vcas;
    const int typeEngine = aux_ ? aux_->typeEngine : 2;

    if (typeEngine == 1 || typeEngine == 2) {
        // PW-100 / PW-220
        if (mach >= 0.84 && mach <= 1.4) {
            rpmCmd = std::max(rpmCmd, mach / 1.4);
        }
        if (mach > 1.4) {
            rpmCmd = std::max(rpmCmd, 0.99);
        }
        if (alt_ft > 10000.0) {
            rpmCmd = std::max(rpmCmd, (alt_ft / 10000.0) / 30.0 + 0.7);
        }
        if (alt_ft >= 35000.0 && alt_ft <= 45000.0 && mach >= 0.4 && mach <= 0.8) {
            rpmCmd = std::min(rpmCmd, 1.025);
        }
        if (alt_ft > 45000.0 && alt_ft <= 55000.0 && mach >= 0.4 && mach <= 0.95) {
            rpmCmd = std::min(rpmCmd, 1.01);
        }
        if (alt_ft > 55000.0 || mach <= 0.4) {
            rpmCmd = std::min(rpmCmd, 0.99);
        }
    } else {
        // PW-229 / GE-110 / GE-129 (types 3, 4, 5)
        if (mach > 0.55 && mach < 1.1) {
            rpmCmd = std::max(rpmCmd, 0.79);
        }
        if (mach >= 1.1 && mach <= 1.4) {
            rpmCmd = std::max(rpmCmd, mach / 1.4);
        }
        if (alt_ft > 50000.0 && vcas < 250.0) {
            rpmCmd = std::min(rpmCmd, 0.99);  // AB no-light zone
        }
    }

    return rpmCmd;
}

// ---------------------------------------------------------------------------
// update: compute thrust, RPM, and fuel flow for one time step.
// ---------------------------------------------------------------------------
void EngineModel::update(double dt,
                         double alt_ft,
                         double mach,
                         double vt_ftps,
                         double mass_slugs,
                         double throttle,
                         double ethrst,
                         bool   simplified,
                         EngineState& state) const {
    // Guard: no table or zero mass
    if (!table_ || !aux_ || mass_slugs <= 1e-6) {
        state.thrust = 0.0;
        state.fuelFlow = 0.0;
        lastFuelFlow_ = 0.0;
        return;
    }

    // --- First-call sync: initialize the lag filter to current RPM ---
    // This prevents a spool-up transient on the first frame.
    static bool firstCall = true;
    if (firstCall && state.rpm > 0.0) {
        state.rpmLag.reset(state.rpm);
        firstCall = false;
    }

    // --- Throttle position ---
    const bool hasAB = table_->hasAB();
    const double pwrlev = std::clamp(throttle, 0.0, hasAB ? 1.5 : 1.0);

    // --- Spool rate ---
    // Increases with altitude (thinner air = faster spool), decreases with
    // Mach (ram pressure = slower spool).
    // IMPORTANT: altitude sign is +alt/25000 (NOT -alt/25000). Earlier
    // F4Flight versions had this sign-flipped.
    double spoolrate = aux_->normSpoolRate + (alt_ft / 25000.0) - (mach / 2.0);
    spoolrate = std::max(0.1, spoolrate);  // floor

    // --- RPM branch (branch on RPM value, not throttle) ---
    // This ensures the AB branch doesn't engage until RPM has spooled up
    // past 1.0 (MIL), even if the throttle is slammed to AB.
    double rpmCmd;
    double thrtb1;  // thrust acceleration (ft/s^2)

    if (state.rpm < 0.68 && state.engLit) {
        // Lightup zone: RPM is below idle, spool up to idle
        rpmCmd = 0.7;
        spoolrate = aux_->lightupSpoolRate;
        thrtb1 = 0.0;
    } else if (state.rpm <= 1.0) {
        // MIL or below: interpolate between idle and MIL thrust
        const double th1 = thrustIdle_(alt_ft, mach);
        const double th2 = thrustMil_(alt_ft, mach);
        thrtb1 = ((th2 - th1) * pwrlev + th1) / mass_slugs;
        // RPM command: 0.7 (idle) to 1.0+ (MIL). Allow exceeding 1.0 so
        // the AB branch engages on the next frame when throttle > 1.0.
        rpmCmd = 0.7 + 0.3 * pwrlev;
    } else if (state.rpm > 1.0 && hasAB) {
        // Afterburner: interpolate between MIL and AB thrust
        const double th1 = thrustMil_(alt_ft, mach);
        const double th2 = thrustAb_(alt_ft, mach);
        thrtb1 = (2.0 * (th2 - th1) * (pwrlev - 1.0) + th1) / mass_slugs;
        rpmCmd = 1.0 + 0.06 * (pwrlev - 1.0);
    } else {
        // No AB fitted or RPM in normal range
        thrtb1 = thrustMil_(alt_ft, mach) / mass_slugs;
        rpmCmd = 1.0;
    }

    // --- Apply per-engine-type RPM schedule ---
    const double vcas_kts = vt_ftps * FTPSEC_TO_KNOTS;
    rpmCmd = engineRpmMods(rpmCmd, alt_ft, mach, vcas_kts);

    // --- Spool dynamics (first-order lag) ---
    state.rpmCmd = rpmCmd;
    state.rpm = state.rpmLag.step(rpmCmd, spoolrate, dt);

    // --- AB lit flag ---
    state.aburnLit = (pwrlev > 1.0) && (state.rpm > 0.95) && hasAB;

    // --- Flameout ---
    if (!state.engLit) {
        state.rpm = state.rpmLag.step(0.0, aux_->flameoutSpoolRate, dt);
        state.aburnLit = false;
    }

    // --- Thrust scaling ---
    // Multiply by thrustFactor (global scale), nEngines (multi-engine),
    // and ethrst (thrust reverse coefficient, usually 1.0).
    const int nEngines = std::max(1, aux_->nEngines);
    state.thrust = thrtb1 * table_->thrustFactor * nEngines * ethrst;

    // --- Fuel flow ---
    double ff;
    if (hasFuelFlowTables_) {
        // Use fuel-flow tables
        if (state.aburnLit) {
            ff = 2.0 * (ffAb_(alt_ft, mach) - ffMil_(alt_ft, mach)) * (pwrlev - 1.0)
               + ffMil_(alt_ft, mach);
        } else {
            ff = (ffMil_(alt_ft, mach) - ffIdle_(alt_ft, mach)) * pwrlev
               + ffIdle_(alt_ft, mach);
        }
    } else {
        // Legacy factor model: ff = factor * thrust * mass
        const double factor = state.aburnLit ? aux_->fuelFlowFactorAb
                                              : aux_->fuelFlowFactorNormal;
        ff = factor * state.thrust * mass_slugs;
    }

    if (simplified) ff *= 0.75;               // AI simplified model
    ff = std::max(ff, static_cast<double>(aux_->minFuelFlow));
    ff *= nEngines;

    // --- Smooth fuel flow (first-order, tau = 0.1s) ---
    // This prevents fuel flow from jumping instantaneously when the throttle
    // moves, which would cause unrealistic fuel-burn spikes.
    const double alpha = dt / (dt + 0.1);
    state.fuelFlow += (ff - state.fuelFlow) * alpha;
    state.fuelFlow = std::max(state.fuelFlow, static_cast<double>(aux_->minFuelFlow));
    lastFuelFlow_ = state.fuelFlow;

    // --- FTIT (turbine temperature, 0..10 normalized) ---
    double ftitCmd;
    if (state.rpm < 0.7) {
        ftitCmd = 5.1 * (state.rpm / 0.7);
    } else if (state.rpm < 0.9) {
        ftitCmd = 5.1 + (state.rpm - 0.7) / 0.2 * 1.0;
    } else if (state.rpm < 1.0) {
        ftitCmd = 6.1 + (state.rpm - 0.9) / 0.1 * 1.5;
    } else {
        ftitCmd = 7.6 + (state.rpm - 1.0) / 0.03 * 0.1;
    }
    ftitCmd = std::clamp(ftitCmd, 0.0, 10.0);
    // FTIT lag (tau = 0.7s)
    state.ftit += (ftitCmd - state.ftit) * dt / (dt + 0.7);
}

// ---------------------------------------------------------------------------
// bodyForces: compute body-axis thrust forces from thrust acceleration.
//
// For normal (non-vectored) thrust, all force is along the body X axis.
// For vectored thrust (Harrier-style), the nozzle angle redirects some
// force to the body Z axis.
// ---------------------------------------------------------------------------
void EngineModel::bodyForces(double thrust_accel,
                              double sinAlpha,
                              double cosAlpha,
                              double nozzlePos,
                              double& xprop,
                              double& yprop,
                              double& zprop,
                              double& xsprop,
                              double& zsprop) {
    yprop = 0.0;  // thrust is always in the XZ plane

    if (nozzlePos <= 1e-6) {
        // Normal (non-vectored) thrust: along body X
        xprop = thrust_accel;
        zprop = 0.0;
    } else {
        // Vectored thrust: nozzle angle redirects force
        const double noz_rad = nozzlePos * DTR;
        xprop = thrust_accel * std::cos(noz_rad);
        zprop = -thrust_accel * std::sin(noz_rad);  // negative = upward
    }

    // Stability-axis components (rotated by alpha)
    xsprop = xprop * cosAlpha;
    zsprop = -xprop * sinAlpha + zprop * cosAlpha;
}

}  // namespace f4::flight
