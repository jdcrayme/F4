// f4-flight-model/flight_model.cpp
//
// Flight model orchestrator implementation.
//
// Ported from F4Flight's flight_model.cpp, which is a port of FreeFalcon's
// AirframeClass::Exec().
//
// The per-frame update flow:
//   1. Store ground state and cache pilot input
//   2. Map speed brake handle to dbrake position
//   3. Actuate flaps (TEF/LEF) toward commanded positions
//   4. Update gear (strut compression, friction, touchdown/lift-off)
//   5. Sub-step N times:
//      a. Update atmosphere (rho, qbar, mach, etc.)
//      b. Update FCS (computes commanded alpha, beta, roll rate)
//      c. Update aerodynamics (CL/CD/CY -> forces)
//      d. Update engine (thrust, RPM, fuel flow)
//      e. Augment aero forces with thrust
//      f. Compute load factors (G)
//      g. Update EOM (integrate body rates, quaternion, position)
//      h. Burn fuel
//
// Key bug fixes from F4Flight (documented inline):
//   - Thrust must augment ALL 4 force sums (xaero, xsaero, zsaero, xwaero),
//     not just xwaero. Failing this gives ~0.13 G error in nzcgs.

#include "f4/flight/flight_model.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <stdexcept>

namespace f4::flight {

// PilotInput::validate() has been moved to f4-flight-api/src/pilot_input.cpp.
// It was extracted so that the API library is self-contained and f4-ai
// can depend on it without pulling in the full flight model.

using f4::math::Vec3d;
using f4::math::Quatd;
using f4::data::AircraftConfig;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------
FlightModel::FlightModel()
    : aero_(), engine_(), fcs_(), gear_(), eom_(), stallSM_(makeStallMachine()) {}

// ---------------------------------------------------------------------------
// init: initialize the flight model with a config and initial conditions.
// ---------------------------------------------------------------------------
void FlightModel::init(const AircraftConfig& cfg,
                       double initialAltitude_ft,
                       double initialVt_ftps,
                       double initialHeading_rad,
                       bool inAir) {
    initSubsystems(cfg);
    initKinematics(initialAltitude_ft, initialVt_ftps, initialHeading_rad);
    initFuelState();
    initGearAndEngine(inAir);
    initTrimAndAtmosphere(initialAltitude_ft);
}

// ---------------------------------------------------------------------------
// initSubsystems: validate config, reset state, reconstruct subsystems,
// rebuild stall SM.
// ---------------------------------------------------------------------------
void FlightModel::initSubsystems(const AircraftConfig& cfg) {
    // Validate config: aero tables must be non-empty
    if (cfg.aero.mach.empty() || cfg.aero.alpha_deg.empty() || cfg.aero.clift.empty()) {
        throw std::runtime_error("FlightModel::init: aero tables are empty");
    }

    cfg_ = cfg;
    state_.reset();

    // Reconstruct subsystems with config pointers
    aero_   = Aerodynamics(&cfg_.aero, &cfg_.geometry, &cfg_.aux);
    engine_ = EngineModel(&cfg_.engine, &cfg_.aux);
    fcs_    = FlightControlSystem(&cfg_, &cfg_.geometry, &cfg_.aux);
    gear_   = GearModel(&cfg_.geometry, &cfg_.aux);
    eom_    = EquationsOfMotion(&cfg_.geometry, &cfg_.aux);

    // Rebuild the stall state machine (picks up any config changes)
    stallSM_ = makeStallMachine(stallCfg_);
    stallTimer_ = 0.0;
    prevAeroStalled_ = false;
    state_.aero.stallState = StallState::None;
}

// ---------------------------------------------------------------------------
// initKinematics: set initial position, velocity, heading, and quaternion
// (level, wings level).
// ---------------------------------------------------------------------------
void FlightModel::initKinematics(double initialAltitude_ft,
                                 double initialVt_ftps,
                                 double initialHeading_rad) {
    KinematicState& k = state_.kin;
    k.z = -initialAltitude_ft;  // NED: altitude = -z
    k.vt = initialVt_ftps;
    k.psi = angle_from_radians(initialHeading_rad);
    k.sigma = angle_from_radians(initialHeading_rad);

    // Initial quaternion from heading (level, wings level)
    // ZYX euler: psi=heading, theta=0, phi=0
    const double cy = std::cos(initialHeading_rad * 0.5);
    const double sy = std::sin(initialHeading_rad * 0.5);
    k.quat = Quatd(cy, 0.0, 0.0, sy).normalized();

    // Initial velocity in world frame (heading = psi, level flight)
    k.xdot = initialVt_ftps * std::cos(initialHeading_rad);
    k.ydot = initialVt_ftps * std::sin(initialHeading_rad);
    k.zdot = 0.0;
}

// ---------------------------------------------------------------------------
// initFuelState: initialise fuel state from config.
// ---------------------------------------------------------------------------
void FlightModel::initFuelState() {
    FuelState& fuel = state_.fuel;
    fuel.emptyWeight_lbs = cfg_.geometry.emptyWeight.value();
    fuel.fuel_lbs = cfg_.geometry.internalFuel.value();
    fuel.weight_lbs = fuel.emptyWeight_lbs + fuel.fuel_lbs + fuel.externalFuel_lbs;
    fuel.mass_slugs = fuel.weight_lbs / GRAVITY;
    fuel.loadingFraction = fuel.weight_lbs / std::max(1.0, fuel.emptyWeight_lbs);
}

// ---------------------------------------------------------------------------
// initGearAndEngine: initialise gear state and engine to idle.
// ---------------------------------------------------------------------------
void FlightModel::initGearAndEngine(bool inAir) {
    // --- Gear ---
    gear_.init(state_.gear);
    state_.gear.inAir = inAir;
    state_.gear.planted = !inAir;
    state_.aero.gearPos = inAir ? 0.0 : 1.0;

    // --- Engine ---
    state_.engine.rpm = RPM_IDLE;  // idle
    state_.engine.engLit = true;
}

// ---------------------------------------------------------------------------
// initTrimAndAtmosphere: atmosphere, alpha trim, FCS filter init,
// quaternion rebuild from euler, trig cache, aero recompute, load factors.
// ---------------------------------------------------------------------------
void FlightModel::initTrimAndAtmosphere(double initialAltitude_ft) {
    KinematicState& k = state_.kin;

    // --- Atmosphere ---
    updateAtmosphere();

    // --- Initial alpha (trim estimate) ---
    // Find the alpha that produces CL = W / (q*S) = g / qsom
    state_.aero.alpha = zero_angle();
    state_.aero.beta = zero_angle();
    AeroInputs aeroIn;
    aeroIn.alpha      = state_.aero.alpha;
    aeroIn.beta       = state_.aero.beta;
    aeroIn.mach       = state_.mach;
    aeroIn.vt_ftps    = k.vt;
    aeroIn.qbar       = state_.qbar;
    aeroIn.qsom       = state_.qsom;
    aeroIn.altitude_ft = initialAltitude_ft;
    aeroIn.groundZ_ft = state_.gear.groundZ_ft;
    aeroIn.z_ft       = k.z;
    aeroIn.vcas_kts   = state_.vcas;
    aeroIn.pstick     = 0.0;
    aero_.update(aeroIn, state_.aero);
    if (state_.gear.inAir && state_.qsom > QSOM_FLOOR) {
        // Target CL for 1-G level flight
        const double targetCl = GRAVITY / state_.qsom;
        // Scan the mach=0 row of the CL table for the nearest alpha
        double bestAlpha = 0.0;
        double bestErr = 1e9;
        const auto& clift = cfg_.aero.clift;
        const std::size_t numAlpha = cfg_.aero.alpha_deg.size();
        if (!clift.empty() && numAlpha > 0) {
            for (std::size_t i = 0; i < numAlpha && i < clift.size(); ++i) {
                double err = std::fabs(clift[i] - targetCl);
                if (err < bestErr) {
                    bestErr = err;
                    bestAlpha = cfg_.aero.alpha_deg[i];
                }
            }
        }
        state_.aero.alpha = angle_from_degrees(std::clamp(bestAlpha, -2.0, 10.0));
    }

    // --- Engine at MIL for trim ---
    state_.engine.rpm = 1.0;
    state_.engine.rpmLag.reset(1.0);

    // --- FCS filter initialization ---
    // With the alpha_bias feedforward added AFTER the lead-lag filter
    // (fcs.cpp runPitch), the lead-lag now shapes ONLY the PI correction,
    // not the full alpha. So the lead-lag and integrator are seeded to 0
    // (no PI correction at trim) — the bias handles trim by construction.
    // trim() re-seeds with the converged value if called.
    state_.fcs.pitchAlphaLag.reset(0.0);
    state_.fcs.pitchRateLag.reset(0.0);
    state_.fcs.pitchIntegral.reset(0.0);
    state_.fcs.aoacmd = state_.aero.alpha;

    // --- Set theta so gamma = theta - alpha = 0 (level flight) ---
    // Both are Angle; theta = alpha (in radians) for level flight.
    k.theta = state_.aero.alpha;
    // Rebuild quaternion from euler
    const double cr = std::cos(0.0);           // phi = 0
    const double sr = std::sin(0.0);
    const double cp = std::cos(to_radians(k.theta) * 0.5);
    const double sp = std::sin(to_radians(k.theta) * 0.5);
    const double cy2 = std::cos(to_radians(k.psi) * 0.5);
    const double sy2 = std::sin(to_radians(k.psi) * 0.5);
    k.quat = Quatd(cr * cp * cy2 + sr * sp * sy2,
                   sr * cp * cy2 - cr * sp * sy2,
                   cr * sp * cy2 + sr * cp * sy2,
                   cr * cp * sy2 - sr * sp * cy2).normalized();

    // Recompute trig cache
    k.sinthe = std::sin(to_radians(k.theta));  k.costhe = std::cos(to_radians(k.theta));
    k.sinpsi = std::sin(to_radians(k.psi));    k.cospsi = std::cos(to_radians(k.psi));
    k.sinphi = 0.0;                k.cosphi = 1.0;
    k.sinalp = std::sin(to_radians(state_.aero.alpha));
    k.cosalp = std::cos(to_radians(state_.aero.alpha));
    k.sinbet = 0.0;                k.cosbet = 1.0;
    k.gmma = zero_angle();  k.singam = 0.0;  k.cosgam = 1.0;
    k.mu = zero_angle();    k.sinmu = 0.0;   k.cosmu = 1.0;
    k.sigma = k.psi;  k.sinsig = k.sinpsi;  k.cossig = k.cospsi;

    // Recompute aero forces at the trim alpha
    aeroIn.alpha = state_.aero.alpha;
    aeroIn.beta  = state_.aero.beta;
    aero_.update(aeroIn, state_.aero);

    // Compute load factors
    accelerometers();
}

// ---------------------------------------------------------------------------
// updateAtmosphere: compute atmosphere outputs from current state.
// ---------------------------------------------------------------------------
void FlightModel::updateAtmosphere() {
    const double alt_ft = -state_.kin.z;  // NED: altitude = -z
    const auto out = computeAtmosphere(alt_ft, state_.kin.vt,
                                       cfg_.geometry.area.value(),
                                       state_.fuel.mass_slugs);
    state_.rho   = out.rho;
    state_.pa    = out.pa;
    state_.mach  = out.mach;
    state_.qbar  = out.qbar;
    state_.qsom  = out.qsom;
    state_.qovt  = out.qovt;
    state_.vcas  = out.vcas;
    state_.sound = out.sound;
}

// ---------------------------------------------------------------------------
// updateGear: update gear state (once per major frame).
// ---------------------------------------------------------------------------
void FlightModel::updateGear(double dt) {
    AeroState& a = state_.aero;
    GearState& g = state_.gear;

    // Actuate gear toward handle position
    const double gearHandle = lastInput_.gearHandle;
    a.gearPos = gear_.updateGearPos(a.gearPos, gearHandle, dt);

    // Update strut compression
    gear_.updateStrutCompression(g, g.groundZ_ft, state_.kin.z, state_.kin.vt, dt);

    // Friction coefficient
    g.muFric = GearModel::calcMuFric(lastInput_.wheelBrakes, lastInput_.parkingBrake,
                                     g.onObject, g.overRunway);

    // Minimum body clearance
    g.minHeight_ft = gear_.computeMinHeight(g, a.gearPos);

    // Touchdown detection: transition from airborne to ground
    bool anyOnGround = false;
    for (const auto& w : g.wheels) {
        if (w.onGround) { anyOnGround = true; break; }
    }
    if (g.inAir && anyOnGround) {
        g.inAir = false;
        g.planted = false;
    }

    // Lift-off detection: transition from ground to airborne
    if (!g.inAir) {
        const double lift_lbs = a.lift * state_.fuel.mass_slugs;
        const double weight_lbs = state_.fuel.weight_lbs;
        if (lift_lbs > weight_lbs * LIFTOFF_LIFT_MARGIN && state_.kin.zdot < -LIFTOFF_ZDOT_THRESH) {
            g.inAir = true;
        }
    }
}

// ---------------------------------------------------------------------------
// accelerometers: compute load factors (G) from current force state.
//
// Gravity is NOT added — these represent aerodynamic + thrust accelerations
// only. Level flight produces nzcgs = 1.0 because lift balances gravity.
// ---------------------------------------------------------------------------
void FlightModel::accelerometers() {
    const AeroState& a = state_.aero;
    LoadFactorState& l = state_.loads;

    // Body axes: -zaero = lift (Z is down)
    l.nxcgb =  a.xaero / GRAVITY;
    l.nycgb =  a.yaero / GRAVITY;
    l.nzcgb = -a.zaero / GRAVITY;

    // Stability axes
    l.nxcgs =  a.xsaero / GRAVITY;
    l.nycgs =  a.ysaero / GRAVITY;
    l.nzcgs = -a.zsaero / GRAVITY;

    // Wind axes
    l.nxcgw =  a.xwaero / GRAVITY;
    l.nycgw =  a.ywaero / GRAVITY;
    l.nzcgw = -a.zwaero / GRAVITY;
}

void FlightModel::computeLoadFactors() { accelerometers(); }

// ---------------------------------------------------------------------------
// minorStep: run one physics sub-step.
// ---------------------------------------------------------------------------
void FlightModel::minorStep(double dt, const PilotInput& input) {
    KinematicState& k = state_.kin;
    AeroState& a = state_.aero;
    FcsState& f = state_.fcs;
    EngineState& e = state_.engine;

    // 1. Atmosphere
    updateAtmosphere();

    // 2. FCS (computes commanded alpha, beta, roll rate)
    FlightConditions fc;
    fc.qbar            = state_.qbar;
    fc.qsom            = state_.qsom;
    fc.mach            = state_.mach;
    fc.vt              = k.vt;
    fc.vcas            = state_.vcas;
    fc.alpha           = a.alpha;
    fc.beta            = a.beta;
    fc.sinalp          = k.sinalp;
    fc.cosalp          = k.cosalp;
    fc.sinbet          = k.sinbet;
    fc.cosbet          = k.cosbet;
    fc.cosmu           = k.cosmu;
    fc.cosgam          = k.cosgam;
    fc.singam          = k.singam;
    fc.costhe          = k.costhe;
    fc.cosphi          = k.cosphi;
    fc.phi             = k.phi;
    fc.loadingFraction = state_.fuel.loadingFraction;
    fc.inAir           = state_.gear.inAir;
    fc.nzcgs           = state_.loads.nzcgs;
    fc.nycgw           = state_.loads.nycgw;
    fcs_.update(input, fc, f, a, dt);

    // 3. Aerodynamics (recompute forces at the new alpha/beta)
    const double alt_ft = -k.z;
    AeroInputs aeroIn;
    aeroIn.alpha      = a.alpha;
    aeroIn.beta       = a.beta;
    aeroIn.mach       = state_.mach;
    aeroIn.vt_ftps    = k.vt;
    aeroIn.qbar       = state_.qbar;
    aeroIn.qsom       = state_.qsom;
    aeroIn.altitude_ft = alt_ft;
    aeroIn.groundZ_ft = state_.gear.groundZ_ft;
    aeroIn.z_ft       = k.z;
    aeroIn.vcas_kts   = state_.vcas;
    aeroIn.pstick     = input.pstick;
    aero_.update(aeroIn, a);

    // 4. Engine
    engine_.update(dt, alt_ft, state_.mach, k.vt,
                   state_.fuel.mass_slugs, input.throttle,
                   1.0,  // ethrst (no thrust reverse)
                   state_.simplified, e);

    // 5. Augment aero forces with thrust
    // IMPORTANT: thrust must be added to ALL 4 force sums (xaero, xsaero,
    // zsaero, xwaero), not just xwaero. The EOM reads xwaero for the Vt
    // integration, but accelerometers() reads xaero/xsaero/zsaero for the
    // G feedback. Missing any of these causes a G error.
    double xprop, yprop, zprop, xsprop, zsprop;
    EngineModel::bodyForces(e.thrust, k.sinalp, k.cosalp, e.nozzlePos,
                            xprop, yprop, zprop, xsprop, zsprop);
    a.xaero  += xprop;
    a.xsaero += xsprop;
    a.zsaero += zsprop;
    a.xwaero += xsprop * k.cosbet;  // wind axis X = stability X * cos(beta)

    // 6. Load factors (G)
    accelerometers();

    // 6.5 Stall state machine: poll flight state, emit events, update
    // AeroState.stallState for next frame's aero force modification.
    updateStallSM(dt, input);

    // 7. EOM (integrate body rates, quaternion, position)
    eom_.update(dt, input, state_);

    // 8. Burn fuel
    const double burnRate = e.fuelFlow / SEC_PER_HOUR;  // lb/hr -> lb/s
    state_.fuel.fuel_lbs -= burnRate * dt;
    state_.fuel.fuel_lbs = std::max(0.0, state_.fuel.fuel_lbs);
    state_.fuel.weight_lbs = state_.fuel.emptyWeight_lbs
                           + state_.fuel.fuel_lbs
                           + state_.fuel.externalFuel_lbs;
    state_.fuel.mass_slugs = state_.fuel.weight_lbs / GRAVITY;
    state_.fuel.loadingFraction = state_.fuel.weight_lbs
                                / std::max(1.0, state_.fuel.emptyWeight_lbs);
}

// ---------------------------------------------------------------------------
// update: run the major-frame update (sub-stepping).
// ---------------------------------------------------------------------------
void FlightModel::update(double dt, const PilotInput& input,
                          double groundZ_ft, const Vec3d& groundNormal) {
    // Validate pilot input — clamp to documented ranges, assert in debug.
    // This is the hardened boundary: AI/scripting/network input is untrusted.
    PilotInput safeInput = input;
    safeInput.validate();

    // Store ground state
    state_.gear.groundZ_ft = groundZ_ft;
    state_.gear.groundNormal = groundNormal;

    // Cache pilot input (needed by updateGear for brake state)
    lastInput_ = safeInput;

    // Map speed brake handle to dbrake position
    // speedBrake: -1 (retracted) .. +1 (extended)  ->  dbrake: 0..1
    state_.aero.dbrake = std::clamp((safeInput.speedBrake + 1.0) * 0.5, 0.0, 1.0);

    // Actuate flaps (TEF/LEF) toward commanded positions
    auto actuate = [](double& pos, double cmd, double rate, double step_dt) {
        const double diff = cmd - pos;
        const double step = rate * step_dt;
        if (std::fabs(diff) < step) {
            pos = cmd;
        } else {
            pos += (diff > 0.0 ? step : -step);
        }
    };
    actuate(state_.aero.tefPos, safeInput.tefCmd, TEF_RATE, dt);
    actuate(state_.aero.lefPos, safeInput.lefCmd, LEF_RATE, dt);

    // Update gear (once per major frame)
    updateGear(dt);

    // Sub-step
    const double minorDt = dt / minorPerMajor_;
    for (int i = 0; i < static_cast<int>(minorPerMajor_); ++i) {
        minorStep(minorDt, safeInput);
    }
}

// ---------------------------------------------------------------------------
// trim: iterative 1-G level flight trim.
//
// Iterates alpha and throttle to find a steady-state where:
//   - nzcgs = 1.0 (level flight)
//   - axial acceleration = 0 (constant speed)
// ---------------------------------------------------------------------------
bool FlightModel::trim() {
    // Ground trim guard: at 0 speed (qsom < QSOM_FLOOR), the aero model
    // can't produce lift regardless of alpha, so the 1-G iteration can't
    // converge and saturates alpha at aoamax. On the ground this is
    // harmless (alpha doesn't matter at 0 speed — the EOM ground clamp
    // controls attitude), but the saturated alpha (35°) produces a bad
    // first-frame transient when the FCS starts. Return early with alpha=0
    // and let the alpha_bias feedforward handle trim once the aircraft
    // starts moving and qsom becomes non-trivial.
    if (state_.qsom <= QSOM_FLOOR) {
        state_.aero.alpha = zero_angle();
        state_.fcs.pitchAlphaLag.reset(0.0);
        state_.fcs.pitchIntegral.reset(0.0);
        state_.fcs.aoacmd = zero_angle();
        state_.trimming = false;
        return true;  // "converged" trivially — 0 alpha at 0 speed
    }

    const int kMaxIter = 80;
    const double kTolNz = 0.05;    // G tolerance
    const double kTolAx = 0.5;     // ft/s^2 tolerance
    const double kAlphaK = 1.5;    // deg/G
    const double kThrotK = 0.01;   // throttle per (ft/s^2)

    double throttleCmd = 0.7;
    state_.trimming = true;

    for (int iter = 0; iter < kMaxIter; ++iter) {
        updateAtmosphere();

        AeroInputs aeroIn;
        aeroIn.alpha      = state_.aero.alpha;
        aeroIn.beta       = state_.aero.beta;
        aeroIn.mach       = state_.mach;
        aeroIn.vt_ftps    = state_.kin.vt;
        aeroIn.qbar       = state_.qbar;
        aeroIn.qsom       = state_.qsom;
        aeroIn.altitude_ft = -state_.kin.z;
        aeroIn.groundZ_ft = state_.gear.groundZ_ft;
        aeroIn.z_ft       = state_.kin.z;
        aeroIn.vcas_kts   = state_.vcas;
        aeroIn.pstick     = 0.0;
        aero_.update(aeroIn, state_.aero);

        // Large dt so the engine spool catches up quickly
        engine_.update(10.0, -state_.kin.z, state_.mach, state_.kin.vt,
                       state_.fuel.mass_slugs, throttleCmd,
                       1.0, state_.simplified, state_.engine);

        // Compute thrust forces
        double xprop, yprop, zprop, xsprop, zsprop;
        EngineModel::bodyForces(state_.engine.thrust, state_.kin.sinalp,
                                state_.kin.cosalp, state_.engine.nozzlePos,
                                xprop, yprop, zprop, xsprop, zsprop);

        const double nzcgs = -(state_.aero.zsaero + zsprop) / GRAVITY;
        const double ax_stab = state_.aero.xsaero + xsprop;

        if (std::fabs(1.0 - nzcgs) < kTolNz && std::fabs(ax_stab) < kTolAx) {
            // Converged — do a final pass with thrust augmentation
            state_.aero.xaero  += xprop;
            state_.aero.xsaero += xsprop;
            state_.aero.zsaero += zsprop;
            state_.aero.xwaero += xsprop * state_.kin.cosbet;
            accelerometers();
            // With the alpha_bias feedforward added AFTER the lead-lag
            // filter (fcs.cpp runPitch), the FCS holds trim by construction
            // when pstick=0: the bias provides the 1-G trim alpha, and the
            // PI loop (lead-lag + integrator) starts at 0. No seeding needed
            // — the bias handles trim, the PI loop handles corrections.
            state_.fcs.pitchAlphaLag.reset(0.0);
            state_.fcs.pitchIntegral.reset(0.0);
            state_.fcs.aoacmd = state_.aero.alpha;
            state_.trimming = false;
            return true;
        }

        // Adjust alpha toward 1 G
        double dalpha = (1.0 - nzcgs) * kAlphaK;
        state_.aero.alpha = angle_from_degrees(
            std::clamp(to_degrees(state_.aero.alpha) + dalpha,
                       cfg_.geometry.aoaMin.to<f4::Degrees>().value(),
                       cfg_.geometry.aoaMax.to<f4::Degrees>().value()));

        // Adjust throttle toward zero axial acceleration
        throttleCmd -= ax_stab * kThrotK;
        throttleCmd = std::clamp(throttleCmd, 0.0, 1.5);
    }

    state_.trimming = false;
    return false;  // did not converge
}

// ---------------------------------------------------------------------------
// updateStallSM: poll flight state, emit events to the stall SM, write the
// SM's current state back to AeroState for next frame's force modification.
//
// This is the bridge between FreeFalcon's polling-based stall logic and
// f4-state-machine's event-driven model. Called once per minor frame,
// after aero (which computed the `stalled` detection flag) and before EOM.
//
// Message bus integration (REFACTOR-2): when a MessageBus is attached,
// this function also publishes:
//   - StallWarningMessage on the rising edge of `aero.stalled` (the
//     earliest possible stall notification — fires BEFORE the SM processes
//     the event, so aural-cue consumers can react with minimum latency).
//   - StallStateChangeMessage when the SM transitions to a new state
//     (the authoritative "the SM has decided" notification).
// Both fire on the calling (sim) thread via publish() — see the threading
// note in flight_model.hpp.
// ---------------------------------------------------------------------------
void FlightModel::updateStallSM(double dt, const PilotInput& input) {
    AeroState& a = state_.aero;
    const StallState currentState = a.stallState;

    // Accumulate time in the current state
    stallTimer_ += dt;

    // Poll flight state and determine which event (if any) to emit
    StallDetection det;
    det.currentState   = currentState;
    det.aeroStalled    = a.stalled;
    det.alpha_deg      = to_degrees(a.alpha);
    det.vcas_kts       = state_.vcas;
    det.stallSpeed_kts = a.stallSpeed;
    det.qbar           = state_.qbar;
    det.timeInState_s  = stallTimer_;
    det.pstick         = input.pstick;

    const auto evtOpt = detectStallEvent(det, stallCfg_);

    // Rising-edge stall warning: publish BEFORE the SM processes the event
    // so consumers get the earliest possible notification. The warning
    // fires on the transition from "not stalled" to "stalled" — one
    // message per stall entry, not one per stalled frame.
    if (bus_ && a.stalled && !prevAeroStalled_) {
        StallWarningMessage msg;
        msg.aircraft_id     = aircraft_id_;
        msg.sim_time_s      = sim_time_s_;
        msg.alpha_deg       = to_degrees(a.alpha);
        msg.vcas_kts        = state_.vcas;
        msg.stall_speed_kts = a.stallSpeed;
        bus_->publish(msg);
    }
    prevAeroStalled_ = a.stalled;

    // Only process if there is an event AND the SM can actually fire it
    // from the current state (avoids no-op process() calls and spurious
    // trace entries).
    if (evtOpt.has_value()) {
        const StallEvent evt = evtOpt.value();
        if (stallSM_.can_fire(evt)) {
            const StallState prev = stallSM_.current();
            stallSM_.process(evt);
            const StallState next = stallSM_.current();
            if (next != prev) {
                // State changed — reset the dwell timer
                stallTimer_ = 0.0;

                // Publish the state-change event (authoritative SM notification).
                if (bus_) {
                    StallStateChangeMessage msg;
                    msg.aircraft_id = aircraft_id_;
                    msg.from_state  = prev;
                    msg.to_state    = next;
                    msg.sim_time_s  = sim_time_s_;
                    msg.alpha_deg   = to_degrees(a.alpha);
                    msg.vcas_kts    = state_.vcas;
                    msg.qbar_psf    = state_.qbar;
                    bus_->publish(msg);
                }
            }
        }
    }

    // Write the SM's current state back to AeroState for next frame's
    // aero force modification (FlatSpin -> lift=0, etc.)
    a.stallState = stallSM_.current();
}

}  // namespace f4::flight
