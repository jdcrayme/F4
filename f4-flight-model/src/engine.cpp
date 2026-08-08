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
#include "f4/data/engine_rpm_schedule.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace f4::flight {

using f4::data::EngineTable;
using f4::data::AuxAero;
using f4::data::ThrustTable;
using f4::data::makeThrustTable;
using f4::math::Table2D;
using f4::math::BoundaryMode;

// ---------------------------------------------------------------------------
// Construction: build Table2D views from the config's raw thrust/fuelflow vectors.
// ---------------------------------------------------------------------------
EngineModel::EngineModel(const EngineTable* table, const AuxAero* aux)
    : table_(table), aux_(aux) {
    assert(table_ != nullptr && "EngineModel: table must not be null");
    assert(aux_   != nullptr && "EngineModel: aux must not be null");
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
// The schedule is now DATA (f4::data::EngineRpmSchedule) rather than an
// if/else chain. The two built-in schedules (PW-100/220 family and
// PW-229/GE-110/129 family) are defined in
// f4-data/src/engine_rpm_schedule.cpp. Adding a new engine family is now
// "add another builtin" rather than "extend this if/else". When f4-data's
// JSON schema grows to load schedules from config, this function doesn't
// change at all — the schedule is just data.
// ---------------------------------------------------------------------------
double EngineModel::engineRpmMods(double rpmCmd, double alt_ft,
                                   double mach, double vcas) const noexcept {
    const int typeEngine = aux_ ? aux_->typeEngine : 2;
    const auto& schedule = data::EngineRpmSchedule::builtin(typeEngine);
    return schedule.apply(rpmCmd, mach, alt_ft, vcas);
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
                         EngineState& state) {
    assert(table_ != nullptr && "EngineModel: table must not be null");
    assert(aux_   != nullptr && "EngineModel: aux must not be null");

    // Guard: no table or zero mass
    if (!table_ || !aux_ || mass_slugs <= MASS_FLOOR) {
        state.thrust = 0.0;
        state.fuelFlow = 0.0;
        return;
    }

    // --- First-call sync: initialize the lag filter to current RPM ---
    // This prevents a spool-up transient on the first frame. The seed flag
    // lives on EngineState (not EngineModel) so that:
    //   - multi-aircraft scenarios work (each aircraft has its own state)
    //   - resetting/replacing the state (retrim, scenario reset) also
    //     resets the seed, so the lag filter is re-seeded on the next call
    // The original bug was a function-local `static bool firstCall` which
    // was shared process-wide — only the first aircraft ever got seeded.
    if (!state.rpmLagSeeded && state.rpm > 0.0) {
        state.rpmLag.reset(state.rpm);
        state.rpmLagSeeded = true;
    }

    // --- Throttle position ---
    const bool hasAB = table_->hasAB();
    const double pwrlev = std::clamp(throttle, 0.0, hasAB ? 1.5 : 1.0);

    // --- Spool rate ---
    // Increases with altitude (thinner air = faster spool), decreases with
    // Mach (ram pressure = slower spool).
    // IMPORTANT: altitude sign is +alt/25000 (NOT -alt/25000). Earlier
    // F4Flight versions had this sign-flipped.
    double spoolrate = aux_->normSpoolRate + (alt_ft / SPOOL_ALT_DIV) - (mach / SPOOL_MACH_DIV);
    spoolrate = std::max(SPOOL_RATE_FLOOR, spoolrate);

    // --- RPM branch (branch on RPM value, not throttle) ---
    // This ensures the AB branch doesn't engage until RPM has spooled up
    // past 1.0 (MIL), even if the throttle is slammed to AB.
    double rpmCmd;
    double thrtb1;  // thrust acceleration (ft/s^2)

    if (state.rpm < RPM_LIGHTUP_THRESH && state.engLit) {
        // Lightup zone: RPM is below idle, spool up to idle
        rpmCmd = RPM_IDLE;
        spoolrate = aux_->lightupSpoolRate;
        thrtb1 = 0.0;
    } else if (state.rpm <= 1.0) {
        // MIL or below: interpolate between idle and MIL thrust
        const double th1 = thrustIdle_(alt_ft, mach);
        const double th2 = thrustMil_(alt_ft, mach);
        thrtb1 = ((th2 - th1) * pwrlev + th1) / mass_slugs;
        // RPM command: idle (0.7) to 1.0+ (MIL). Allow exceeding 1.0 so
        // the AB branch engages on the next frame when throttle > 1.0.
        rpmCmd = RPM_IDLE + RPM_MIL_RANGE * pwrlev;
    } else if (state.rpm > 1.0 && hasAB) {
        // Afterburner: interpolate between MIL and AB thrust
        const double th1 = thrustMil_(alt_ft, mach);
        const double th2 = thrustAb_(alt_ft, mach);
        thrtb1 = (2.0 * (th2 - th1) * (pwrlev - 1.0) + th1) / mass_slugs;
        rpmCmd = 1.0 + RPM_AB_GAIN * (pwrlev - 1.0);
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
    state.aburnLit = (pwrlev > 1.0) && (state.rpm > RPM_AB_LIGHTUP) && hasAB;

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

    if (simplified) ff *= AI_FUEL_FLOW_FACTOR;    // AI simplified model
    ff = std::max(ff, static_cast<double>(aux_->minFuelFlow));
    ff *= nEngines;

    // --- Smooth fuel flow (first-order, tau = 0.1s) ---
    // This prevents fuel flow from jumping instantaneously when the throttle
    // moves, which would cause unrealistic fuel-burn spikes.
    const double alpha = dt / (dt + FUEL_FLOW_TAU);
    state.fuelFlow += (ff - state.fuelFlow) * alpha;
    state.fuelFlow = std::max(state.fuelFlow, static_cast<double>(aux_->minFuelFlow));

    // --- FTIT (turbine temperature, 0..10 normalized) ---
    double ftitCmd;
    if (state.rpm < RPM_IDLE) {
        ftitCmd = FTIT_IDLE_TEMP * (state.rpm / RPM_IDLE);
    } else if (state.rpm < FTIT_MIL_LOW_RPM) {
        ftitCmd = FTIT_IDLE_TEMP + (state.rpm - RPM_IDLE) / FTIT_MIL_LOW_RANGE * FTIT_MIL_LOW_GAIN;
    } else if (state.rpm < 1.0) {
        ftitCmd = 6.1 + (state.rpm - FTIT_MIL_LOW_RPM) / FTIT_MIL_HIGH_RANGE * FTIT_MIL_HIGH_GAIN;
    } else {
        ftitCmd = 7.6 + (state.rpm - 1.0) / FTIT_AB_RPM_RANGE * FTIT_AB_GAIN;
    }
    ftitCmd = std::clamp(ftitCmd, 0.0, FTIT_MAX);
    // FTIT lag
    state.ftit += (ftitCmd - state.ftit) * dt / (dt + FTIT_TAU);
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

    if (nozzlePos <= NOZZLE_POS_THRESH) {
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
