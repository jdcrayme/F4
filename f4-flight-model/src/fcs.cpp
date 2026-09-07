// f4-flight-model/fcs.cpp
//
// Flight Control System implementation.
//
// Ported from F4Flight's fcs.cpp, which is a port of FreeFalcon's
// fcs.cpp + gain.cpp + pitch.cpp + roll.cpp + yaw.cpp.
//
// The FCS has three channels:
//
// PITCH (G-command PI controller):
//   The pilot commands a G load via the pitch stick. The FCS computes the
//   error between commanded G and actual G (nzcgs), runs it through a PI
//   controller, and produces a commanded alpha. A lead-lag filter (F7Tust)
//   then shapes the alpha command to produce the final alpha_deg that the
//   aerodynamics model uses.
//
//   Anti-windup (PHUG-PLAN P4.1): back-calculation. The integrator is a REAL
//   integrator (Adams-Bashforth 2nd order, never re-seeded in flight); when
//   the clamped alpha command differs from the unclamped PI output, the
//   difference is fed back into the integrator input with a 0.5 s tracking
//   time constant. This single mechanism replaces the previous trio
//   (conditional integration + QIL 120 s leak + STAB-E51 shedding), which
//   interacted through the AB2 filter's reset() — the leak's per-frame
//   reset(eintg) clobbered u_prev with the OUTPUT, silently turning the
//   integrator into a first-order lag with DC gain 2.95*kp03 (measured,
//   Docs/LOOP_MARGIN_REPORT.md §3.5, finding M6).
//
// ROLL (rate command):
//   The pilot commands a roll rate via the roll stick. The FCS looks up the
//   maximum roll rate from the roll command table (alpha x qbar), scales it
//   by the stick input, applies alpha-based and speed-based rate limiting,
//   and filters the result through a first-order lag.
//
// YAW (beta-command, mostly stubbed):
//   The pilot commands a sideslip via the rudder pedals. The FCS computes a
//   beta command, but the EOM has no rudder-to-yaw dynamics, so beta is
//   forced to 0 to avoid positive feedback. This matches FreeFalcon behavior.
//
// Several bugs in earlier F4Flight versions were fixed by comparing against
// the FreeFalcon source. These fixes are documented inline with "Bug X fix:"
// comments.

#include "f4/flight/fcs.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace f4::flight {

using f4::data::AircraftConfig;
using f4::data::AircraftGeometry;
using f4::data::AuxAero;
using f4::data::LimiterKey;
using f4::data::Limiter;
using f4::data::LimiterType;
using f4::data::makeRollRateTable;
using f4::math::LagFilter;
using f4::math::AdamsBash2Filter;
using f4::math::LeadLagFilter;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------
FlightControlSystem::FlightControlSystem(const AircraftConfig* cfg,
                                         const AircraftGeometry* geom,
                                         const AuxAero* aux)
    : cfg_(cfg), geom_(geom), aux_(aux) {
    assert(cfg_  != nullptr && "FlightControlSystem: cfg must not be null");
    assert(geom_ != nullptr && "FlightControlSystem: geom must not be null");
    assert(aux_  != nullptr && "FlightControlSystem: aux must not be null");
    if (cfg_ && !cfg_->rollCmd.rollRate.empty()) {
        rollCmdTable_ = makeRollRateTable(cfg_->rollCmd);
    }
}

// ---------------------------------------------------------------------------
// applyLimiter: evaluate a named limiter at input x.
// Returns x unchanged if the limiter is not configured (default Line limiter
// with all-zero coords returns 0, which we treat as "no limit").
// ---------------------------------------------------------------------------
double FlightControlSystem::applyLimiter(LimiterKey key, double x) const {
    if (!cfg_) return x;
    const Limiter& lim = cfg_->limiter(key);
    // A default-constructed limiter (type=Line, all coords 0) returns 0.
    // Treat this as "not configured" and pass x through unchanged.
    if (lim.type == LimiterType::Line && lim.x1 == 0.0 && lim.x2 == 0.0 &&
        lim.y1 == 0.0 && lim.y2 == 0.0) {
        return x;
    }
    return lim.limit(x);
}

// ---------------------------------------------------------------------------
// update: run the full FCS for one time step.
// ---------------------------------------------------------------------------
void FlightControlSystem::update(const PilotInput& pilot,
                                 const FlightConditions& fc,
                                 FcsState& fcsState,
                                 AeroState& aeroState,
                                 double dt) const {
    assert(cfg_  != nullptr && "FlightControlSystem: cfg must not be null");
    assert(geom_ != nullptr && "FlightControlSystem: geom must not be null");
    assert(aux_  != nullptr && "FlightControlSystem: aux must not be null");

    // Unpack flight conditions for readability
    const double qbar            = fc.qbar;
    const double qsom            = fc.qsom;
    const double mach            = fc.mach;
    const double vt_ftps         = fc.vt;
    const double vcas_kts        = fc.vcas;
    const Angle  alpha           = fc.alpha;
    const Angle  beta            = fc.beta;
    const double cosmu           = fc.cosmu;
    const double cosgam          = fc.cosgam;
    const double singam          = fc.singam;
    const double costhe          = fc.costhe;
    const double cosphi          = fc.cosphi;
    const Angle  phi             = fc.phi;
    const double loadingFraction = fc.loadingFraction;
    const bool   inAir           = fc.inAir;
    const double nzcgs           = fc.nzcgs;
    const double nycgw           = fc.nycgw;

    // References to mutable state (named to match the rest of the impl)
    FcsState& fcs  = fcsState;
    AeroState& aero = aeroState;

    (void)mach;  // not directly used by FCS (aero tables handle Mach effects)

    // The FCS internals still work in degrees for alpha/beta (table lookups,
    // limiter inputs, aoamin/aoamax bounds). Extract once at the boundary.
    const double alpha_deg = to_degrees(alpha);
    const double beta_deg  = to_degrees(beta);
    const double phi_rad   = to_radians(phi);

    // Landing gains are active when gear is down, refueling, or explicitly
    // requested by the host (e.g. approach mode).
    const bool gearDown = (aero.gearPos > 0.5);
    const bool landingGains = pilot.refueling || gearDown;

    // EXPERIMENT S: Apply roll-limit overrides from the steering layer.
    // When the AI sets maxRollDeg/maxRollDeltaDeg on PilotInput (>= 0), the
    // FCS uses those values to clamp bank and taper roll rate as the bank
    // approaches the limit. This is Falcon's maxRoll/maxRollDelta mechanism,
    // exposed cleanly through the PilotInput boundary. Negative values (the
    // default) leave the FCS internal defaults (80° bank, 5° taper window).
    if (pilot.maxRollDeg >= 0.0) {
        fcs.maxRoll = pilot.maxRollDeg;
    }
    if (pilot.maxRollDeltaDeg >= 0.0) {
        fcs.maxRollDelta = pilot.maxRollDeltaDeg;
    }

    // --- Damper gains from limiters ---
    // These scale the pitch/roll/yaw outputs based on dynamic pressure.
    fcs.plsdamp = applyLimiter(LimiterKey::PitchYawControlDamper, qbar);
    fcs.rlsdamp = applyLimiter(LimiterKey::RollControlDamper, qbar);
    fcs.ylsdamp = fcs.plsdamp;  // yaw uses same damper as pitch

    // --- pshape: shaped pitch stick input ---
    // Computed here because kp01 (in computeGains) depends on it.
    // pshape = pstick^2 * sign(pstick): squares the input (so small inputs
    // are suppressed) and preserves the sign.
    fcs.pshape = pilot.pstick * pilot.pstick;
    if (pilot.pstick < 0.0) fcs.pshape = -fcs.pshape;

    // --- Compute gains (pitch/roll/yaw) ---
    computeGains(qbar, qsom, vt_ftps, alpha,
                 aero.clift0, aero.clalph0, aero.clalpha, aero.cnalpha,
                 aero.cy,
                 cosgam, cosmu, costhe, cosphi,
                 loadingFraction, inAir,
                 landingGains, aero.gearPos,
                 fcs);

    // --- Run channels (pitch must run before aero uses alpha) ---
    const double aoamin = geom_->aoaMin.to<f4::Degrees>().value();
    const double aoamax = geom_->aoaMax.to<f4::Degrees>().value();
    const double betmin = geom_->betaMin.to<f4::Degrees>().value();
    const double betmax = geom_->betaMax.to<f4::Degrees>().value();
    const double maxGs  = geom_->maxGs;

    runPitch(dt, qbar, qsom, vt_ftps, vcas_kts,
             alpha, cosmu, cosgam, singam,
             nzcgs, aero.cl, aero.clalpha, aero.clalph0,
             aero.cnalpha, aoamin, aoamax, maxGs,
             pilot, fcs, aero, inAir, fc.pitch_rate, fc.alt_agl_ft);

    runRoll(dt, qbar, vcas_kts, alpha,
            aero.gearPos, phi,
            pilot, fcs);

    runYaw(dt, qbar, qsom, vt_ftps, vcas_kts,
           beta, nycgw, betmin, betmax,
           pilot, fcs, aero);

    // Suppress unused-variable warnings for the locals extracted above;
    // they are retained for clarity at the FCS-internal boundary even when
    // a future refactor moves them deeper into the channel functions.
    (void)alpha_deg; (void)beta_deg; (void)phi_rad;
}

// ---------------------------------------------------------------------------
// computeGains: compute FCS gains from current flight state.
//
// This is the port of FreeFalcon's gain.cpp. It computes the pitch/roll/yaw
// gains and time constants based on dynamic pressure, airspeed, alpha, and
// aircraft loading. The gains are designed to place the closed-loop poles
// at desired locations (2nd-order system design).
// ---------------------------------------------------------------------------
void FlightControlSystem::computeGains(double qbar, double qsom, double vt,
                                       Angle alpha,
                                       double clift0, double clalph0,
                                       double clalpha, double cnalpha,
                                       double cy,
                                       double cosgam, double cosmu,
                                       double costhe, double cosphi,
                                       double loadingFraction,
                                       bool inAir,
                                       bool landingGains, double gearPos,
                                       FcsState& fcs) const {
    (void)gearPos;  // landing gains applied separately below
    (void)clift0;   // reserved for future lift-curve refinement
    (void)clalpha;  // reserved: clalph0 (static slope) is currently used instead
    (void)cosgam;   // reserved for future flight-path-angle coupling
    (void)cosmu;    // reserved for future velocity-axis coupling

    const double alpha_deg = to_degrees(alpha);
    const double cosphiLim = std::max(0.0, cosphi);

    // --- Available G from lift ---
    // gsAvail = max G the aircraft can produce at current alpha.
    // Uses clalph0 (static slope), NOT the local clalpha.
    const double gsAvail = geom_->aoaMax.to<f4::Degrees>().value() * clalph0 * qsom / GRAVITY;

    // --- Pitch damping ratio (zp01) ---
    // Reduced at low qbar (sloppy controls) and high loading (sluggish).
    fcs.zp01 = 0.900;
    fcs.zp01 *= (1.0 - 0.15 * std::max(0.0, 1.0 - qbar / 25.0));
    // zpdamp: additional damping from limiter (not commonly configured)
    // (omitted — the limiter-based zpdamp is rarely used and adds complexity)
    fcs.zp01 -= std::max(0.0, (loadingFraction - 1.3) * 0.01);
    fcs.zp01 = std::max(0.5, fcs.zp01);  // floor

    // --- kp01: pitch command gain ---
    // Depends on pshape: positive stick (pull) commands G up to maxGs;
    // negative stick (push) commands G down to -4 (or whatever costhe allows).
    if (fcs.pshape > 0.0) {
        fcs.kp01 = geom_->maxGs - costhe * cosphiLim;
    } else {
        fcs.kp01 = 4.0 + costhe * cosphiLim;
    }

    // --- Pitch time constants ---
    fcs.tp01 = 0.200;  // lead time constant
    // P4.1: kp02 stays 1.0 (the value the pole-placement algebra assumes
    // with the corrected kp05 = 1/K_nz). NOTE (worklog PHUG-P4): the
    // "|R|peak 0.66-1.05 flat" claim below was measured BEFORE the
    // q-damper rescale was discovered — with the damper 28x too hot the
    // small-signal g-case looks deceptively flat while the command path is
    // broken (the loop trims to stick + damper-bias). The binding
    // measurements for the corrected loop are the P4 stick-step (command
    // following) and the post-rescale margin campaign.
    fcs.kp02 = 1.0;    // proportional gain
    // P4.1: kp03 2.0 -> 0.4. With the corrected kp05 = 1/K_nz the integral
    // path's loop gain is kp03·kp05·K_nz = kp03 (the plant inverse cancels)
    // — at 2.0 the integrator crossed over ABOVE the P path (2 rad/s vs
    // ~0.9), dominated every transient, wound huge states during railed
    // commands, and unwound only as fast as the reversed error allowed —
    // the ballast behind the approach porpoising (measured: 8 s nz limit
    // cycle through the guardian). 0.4 puts the integral crossover at
    // ~0.4 rad/s (half the P crossover — the classic PI split), keeping
    // the type-1 DC trim without the ballast.
    fcs.kp03 = 0.4;    // integral gain

    // --- Closed-loop pitch frequency ---
    // omegasp = 1 / (ttheta2 * 0.65), where ttheta2 is the time constant
    // of the alpha-response-to-G transfer function.
    const double nzalpha = clalph0 * qsom * RTD / GRAVITY;
    const double ttheta2 = std::max(0.1, vt / (GRAVITY * std::max(0.01, nzalpha)));
    double omegasp1 = std::max(1.0, 1.0 / (ttheta2 * 0.65));
    double omegasp = omegasp1;

    // On ground, double the frequency (faster response for takeoff/landing)
    if (!inAir) {
        omegasp *= 2.0;
    } else {
        // LowSpeedOmega limiter: scales frequency at low speed
        // (only if the limiter is configured — most aircraft don't use it)
        const Limiter& lso = cfg_->limiter(LimiterKey::LowSpeedOmega);
        if (lso.type != LimiterType::Line ||
            !(lso.x1 == 0.0 && lso.x2 == 0.0 && lso.y1 == 0.0 && lso.y2 == 0.0)) {
            omegasp *= lso.limit(qbar);
        }
    }

    // --- Inner-loop pole placement (2nd order) ---
    // Solves for tp02, tp03 (lag time constants) that place the closed-loop
    // poles at the desired locations.
    const double wp01 = omegasp;
    const double pcoef1 = fcs.tp01 * wp01 * wp01 - 2.0 * fcs.zp01 * wp01 - fcs.kp03;
    const double pcoef2 = 2.0 * fcs.zp01 * wp01 * fcs.kp03 - fcs.kp03 * fcs.tp01 * wp01 * wp01;
    const double pradcl = std::max(pcoef1 * pcoef1 - 4.0 * pcoef2, 0.0);
    const double pfreq1 = (std::sqrt(pradcl) - pcoef1) * 0.5;
    const double pfreq2 = -pcoef1 - pfreq1;

    fcs.tp02 = (std::fabs(pfreq1) > QSOM_FLOOR) ? 1.0 / pfreq1 : 1.0;
    fcs.tp03 = std::max(0.5, (std::fabs(pfreq2) > QSOM_FLOOR) ? 1.0 / pfreq2 : 1.0);
    // PHUG-PLAN P0.3: publish the designed inner-loop bandwidth so the trace
    // shows the instantaneous L0 design point (it varies with V via ttheta2).
    fcs.omegaSp = omegasp;

    // --- kp05: pitch feedback gain (G-error → alpha-degrees) ---
    // PHUG-PLAN P4.1 (measured, LOOP_MARGIN_REPORT §6 + the P4 stick-step):
    // kp05 is the static plant inverse — degrees of alpha per G of error:
    //     kp05 = 1/K_nz = g/(clalph0·qsom)
    // The FreeFalcon AOA-command formula (kp05 = tp02·tp03·ω²) OMITTED the
    // plant gain entirely: the realized loop gain kp05·kp02·K_nz was 0.036
    // instead of ~1, putting the integrator crossover at 0.07 rad/s — 10x
    // below the designed ω_sp = 0.8. The M6 lag bug (DC gain 5.9) had been
    // propping the loop gain up to ~0.25; fixing the integrator (real 1/s)
    // exposed the gain deficit. With kp05 = 1/K_nz the loop gain is ~1 and
    // the closed-loop poles land where this algebra intended (ω = ω_sp,
    // ζ = zp01) — verified by the fm_sysid margin harness.
    // AOA-command mode vs G-command mode share the same static plant
    // (nzcgs = K_nz·alpha), so one formula covers both; the gsAvail/maxGs
    // clamp on ptcmd already bounds the command authority either way.
    fcs.aoaCmdModeRuntime = (gsAvail <= geom_->maxGs);

    if (std::fabs(clalph0) > QSOM_FLOOR && qsom > QSOM_FLOOR) {
        fcs.kp05 = GRAVITY / (clalph0 * qsom);
    } else {
        // Degenerate aero (synthetic fixtures): keep the legacy formula so
        // the loop still closes at a low, benign gain.
        fcs.kp05 = fcs.tp02 * fcs.tp03 * wp01 * wp01;
    }

    // --- P4.1: q-damper loop-gain rescale ---
    // The Tranche 42/45/46 damper gain (pitchRateDampGain, flight_model.cpp)
    // was CALIBRATED — through STAB/Tranche iteration and the P2-M1 margin
    // measurements — against the legacy feedback gain kp05 = tp02·tp03·ω².
    // P4.1 corrected kp05 to the plant inverse 1/K_nz, which at the 250-kt
    // trim is 28x larger (0.244 → 6.8 deg/G). The damper term k·q enters the
    // G-command path BEFORE the ×kp05 conversion, so the same k became ~28x
    // hotter in loop-gain terms: measured (P4 stick-step, worklog PHUG-P4),
    // a railed −0.35 stick delivered only −0.31 G of its −0.61 G command
    // because the damper injected +0.30 G of rate-proportional phantom pull
    // and the (correctly tracking) PI trimmed to stick + damper-bias.
    // Rescale: effective authority = base·sched·(kp05_legacy/kp05), computed
    // per frame so the P2-measured healthy damping carries over at every
    // speed. (kp05_legacy is still available — the tp02/tp03 pole algebra
    // above is unchanged.)
    {
        const double kp05_legacy = fcs.tp02 * fcs.tp03 * wp01 * wp01;
        fcs.qDampScale = (std::fabs(fcs.kp05) > QSOM_FLOOR)
            ? std::clamp(kp05_legacy / fcs.kp05, 0.0, 1.0)
            : 1.0;
    }

    // Ground fade: at very low qbar, reduce kp05 to avoid excessive alpha
    // commands during taxi.
    // NOTE: qDampScale is computed from the pre-fade kp05 — the fade is a
    // ground-taxi authority limit, not a change of the plant the damper
    // senses (and the damper itself is AGL-gated in runPitch).
    if (!inAir) {
        fcs.kp05 *= std::max(0.0, std::min(1.0, (qbar - 20.0) / 45.0));
    }

    // --- Roll channel ---
    // tr01: roll rate lag time constant. Higher at low qbar (sluggish).
    if (qbar >= 250.0) {
        fcs.tr01 = 0.25;
    } else {
        fcs.tr01 = -0.001111 * (qbar - 100.0) + 0.416;
    }

    // psmax: maximum roll rate from the command table (deg/s).
    double psmax = 360.0;  // default if no table
    if (rollCmdTable_) {
        psmax = (*rollCmdTable_)(alpha_deg, qbar);
    }
    fcs.kr01 = psmax * DTR;  // convert to rad/s
    fcs.kr02 = std::cos(alpha_deg * DTR);  // roll authority reduction at high alpha

    if (landingGains) {
        fcs.kr01 *= aux_->rollGearGain;
    }

    // --- Yaw channel ---
    const double zy01 = 0.70;  // yaw damping ratio
    const double wy01 = 0.3 / std::max(0.01, fcs.tr01) * (1.0 - loadingFraction * 0.1);
    fcs.ky02 = 1.0;
    fcs.ky03 = 2.0;

    // Yaw pole placement (same 2nd-order approach as pitch)
    const double ycoef1 = -2.0 * zy01 * wy01 - fcs.ky03;
    const double ycoef2 =  2.0 * zy01 * wy01 * fcs.ky03;
    const double yradcl = std::max(ycoef1 * ycoef1 - 4.0 * ycoef2, 0.0);
    const double yfreq1 = (std::sqrt(yradcl) - ycoef1) * 0.5;
    const double yfreq2 = -ycoef1 - yfreq1;

    fcs.ty02 = (std::fabs(yfreq2) > QSOM_FLOOR) ? 1.0 / yfreq2 : 1.0;

    // ky05: yaw feedback gain.
    // IMPORTANT: preserve the sign of the denominator. Earlier versions used
    // max(1e-6, denom) which destroyed the sign when cy < 0, causing the yaw
    // damper to become a positive-feedback loop.
    const double denom = qsom * cy * yfreq1 * yfreq2;
    if (std::fabs(denom) > QSOM_FLOOR) {
        fcs.ky05 = -GRAVITY * wy01 * wy01 / denom;
    }

    // Landing gain scaling
    if (landingGains) {
        fcs.kp05 *= aux_->pitchGearGain;
        fcs.ky05 *= aux_->yawGearGain;
    }
}

// ---------------------------------------------------------------------------
// runPitch: G-command PI controller with anti-windup.
// ---------------------------------------------------------------------------
void FlightControlSystem::runPitch(double dt, double qbar, double qsom,
                                    double vt, double vcas_kts,
                                    Angle alpha, double cosmu,
                                    double cosgam, double singam,
                                    double nzcgs, double cl, double clalpha,
                                    double clalph0, double cnalpha,
                                    double aoamin, double aoamax, double maxGs,
                                    const PilotInput& input,
                                    FcsState& fcs, AeroState& aero,
                                    bool inAir,
                                    double pitch_rate,
                                    double alt_agl_ft) const {
    (void)qbar; (void)vt; (void)vcas_kts;
    (void)alpha;  // pitch channel writes aero.alpha; current value unused
    (void)cl; (void)clalpha; (void)singam;
    (void)cnalpha;  // reserved for future yaw-damping coupling
    (void)input;    // pitch channel reads fcs.pshape, not raw pilot input

    // --- Alpha bias (1-G trim feedforward) ---
    // FreeFalcon's gain.cpp computes an alpha bias that represents the
    // alpha needed for 1-G level flight at the current flight conditions:
    //   α_bias = [g·cos(γ)·cos(μ) / q_som + 0.1·gear − CL₀·TEF_factor] / CL_α,0
    //            − tefFactor + lefFactor
    //
    // KEY DESIGN DECISION: the bias is added AFTER the lead-lag filter,
    // not before. The previous attempt (worklog ALT-2) fed
    // `aoacmd = bias + PI_output` through the lead-lag, but the filter's
    // lead term (tau1=0.2s) amplified frame-to-frame bias changes,
    // producing alpha overshoot that the AI cascade then over-corrected.
    // By adding the bias after the filter, the lead-lag only shapes the
    // PI correction — the bias goes directly to alpha without filter
    // dynamics. Result: altitude range dropped from 593 ft to 166 ft.
    //
    // NOTE: clalph0 is per-DEGREE (not per-radian) — see its use in
    // computeGains (gsAvail = aoaMax_deg * clalph0 * qsom / g). So
    // cl_needed / clalph0 is already in degrees — do NOT multiply by RTD.
    //
    // See Docs/FreeFalcon_Core_Systems_Reference.html §4.2 (AOA Bias).
    // See worklog ALT-2 through ALT-5 for the investigation history.
    const double tefFactor = aero.tefPos;
    const double lefFactor = aero.lefPos;
    const double clift0 = aero.clift0;
    // P4.3: the alpha-bias trim-lag time constant (see FcsState::alphaBiasTrim).
    // v19d MEASURED (worklog PHUG-P4): 45 s. The bias responds to the
    // oscillating V through 1/qsom: at the measured 26 s beam-ride cycle the
    // ±12 kt swings moved the 5-s-lagged bias ±0.8 deg = ±0.09 G at −70 deg
    // phase — a feedforward PUMP inside every half-cycle (the intercept
    // ride's ±2,500 fpm cycle). 45 s attenuates the 0.24 rad/s coupling to
    // ~9% (±0.008 G — negligible) while real trim changes (gear/flap
    // extension, spool transients) are covered by the FCS's own type-1 PI
    // (kp03 0.4 → tau 2.5 s: 18x dominant-time separation, the cascade
    // rule). MEASURED at 5 s: violent (gamma ±19); at 1.5 s: violent;
    // shortening is never the answer — the lag IS the damper.
    constexpr double kAlphaBiasTrimTauS = 5.0;
    double alpha_bias_deg = 0.0;
    // The bias formula cl_needed = g/qsom blows up at low qsom (g/qsom → ∞).
    // Only compute the bias when qsom is high enough to produce a reasonable
    // alpha — below this, the aircraft is on the ground or at very low speed
    // and the bias should be 0 (the EOM ground clamp controls attitude).
    // The threshold of 5.0 corresponds to roughly 50 kts for the F-16
    // (qsom = q*S/m ≈ 5 at 50 kts sea level). Below this, no meaningful
    // 1-G trim alpha can be computed.
    if (std::fabs(clalph0) > QSOM_FLOOR && qsom > 5.0) {
        // Flare-mode G reduction: when gear down + idle throttle + not in
        // ground effect (nzcgs < 1.05), target 0.92G instead of 1.0G.
        // This produces a gentle descent during the flare.
        double flare_g_factor = 1.0;
        if (aero.gearPos > 0.5 && input.throttle < 0.05 && nzcgs < 1.05) {
            flare_g_factor = 0.92;
        }
        // EXPERIMENT L (Idea 1A done right): the coordinated-turn lift needed
        // is L = m*g*cos(γ) / cos(μ), so cl_needed = g*cos(γ) / (cos(μ)*qsom).
        // The previous formula had cos(γ)*cos(μ) (multiply instead of divide),
        // which UNDER-CORRECTED by cos²(bank): at 30° bank it commanded 0.866G
        // instead of 1.155G, the aircraft sank, and the AI cascade compensated
        // through the FCS PI lag → phugoid → speed-brake cycling.
        const double cosmu_safe = std::max(0.3, cosmu);  // clamp at ~72° bank
        const double cl_needed = flare_g_factor * GRAVITY * cosgam / (cosmu_safe * qsom)
                               + 0.1 * aero.gearPos
                               - clift0 * tefFactor * aux_->CLtefFactor;
        // clalph0 is per-degree, so cl_needed / clalph0 is in degrees.
        alpha_bias_deg = cl_needed / clalph0 - tefFactor + lefFactor;
        alpha_bias_deg = std::clamp(alpha_bias_deg, aoamin, aoamax);
        // PHUG-PLAN P4.3 (M7): the bias is a TRIM feedforward — lag it so the
        // per-frame 1/qsom tracking cannot pump alpha at the outer-loop
        // frequencies (see FcsState::alphaBiasTrim for the measured failure).
        // The type-1 PI integral owns the transient trim; the ground reset
        // below also resets this filter.
        alpha_bias_deg = fcs.alphaBiasTrim.step(alpha_bias_deg,
                                                kAlphaBiasTrimTauS, dt);
    }
    // PHUG-PLAN P0.3: publish the bias (0 below the qsom guard) so the trace
    // can separate the feedforward path from the PI correction path.
    fcs.alphaBiasDeg = alpha_bias_deg;

    // --- Commanded G ---
    double ptcmd = fcs.pshape * fcs.kp01;

    // Limit the command to available G.
    //
    // applyLimiter() returns its input verbatim when the limiter is not
    // configured (default-constructed Line with all-zero coords). For the
    // damper limiters that is the desired passthrough (input is qbar, a
    // unitless pressure, and the "limited" output is also a dimensionless
    // scale). For NegGLimiter, however, the input is vcas_kts and the
    // expected output is in G — returning vcas_kts (~300) when unconfigured
    // produces a nonsense maxNegGs and inverts the clamp bounds (MSVC's
    // debug CRT asserts; libstdc++ silently produces garbage). Detect the
    // passthrough case explicitly and fall back to a symmetric -maxGs.
    double maxNegGs = applyLimiter(LimiterKey::NegGLimiter, vcas_kts);
    {
        const Limiter& nlim = cfg_ ? cfg_->limiter(LimiterKey::NegGLimiter) : Limiter{};
        const bool unconfigured = (nlim.type == LimiterType::Line &&
                                   nlim.x1 == 0.0 && nlim.x2 == 0.0 &&
                                   nlim.y1 == 0.0 && nlim.y2 == 0.0);
        if (unconfigured) maxNegGs = -maxGs;
    }

    // gsAvail can collapse to 0 (or even go negative) when clalph0 is 0
    // — which happens for synthetic test fixtures and for any aircraft
    // whose .dat aero tables haven't been populated yet. In that case the
    // G-limiting clamp range degenerates and std::clamp(v, lo, hi) would
    // be called with lo > hi, which is undefined behavior (libstdc++
    // returns garbage; MSVC's debug CRT asserts). Guard explicitly.
    const double gsAvail = std::max(0.0,
        geom_->aoaMax.to<f4::Degrees>().value() * clalph0 * qsom / GRAVITY);
    const double maxCmd  = maxGs;
    const double upper   = std::min(gsAvail, maxCmd);
    // Defensive: never let lower > upper. If maxNegGs > upper (can only
    // happen via a misconfigured limiter), pin lower to upper so the clamp
    // becomes a no-op rather than UB.
    const double lower   = std::min(maxNegGs, upper);
    ptcmd = std::clamp(ptcmd, lower, upper);
    fcs.ptcmd = ptcmd;

    // --- G error ---
    // The error is the difference between commanded G and actual G, minus
    // the gravity component that the FCS must cancel.
    // EXPERIMENT L (Idea 1A done right): the gravity baseline is 1/cos(mu),
    // NOT cos(mu). In a 30° bank the aircraft needs 1.155G to hold altitude;
    // the old formula used cos(30°)=0.866, so the FCS trimmed to 0.866G and
    // the aircraft sank in every turn.
    const double cosmu_lim = std::max(0.3, cosmu);  // clamp at ~72° bank
    const double gravity_baseline = cosgam / cosmu_lim;  // 1G / cos(bank) in banked flight
    const double gearGravityTerm = 0.1 * aero.gearPos * qsom / GRAVITY;
    // Ground guard: on the ground (gear down) at low speed, the aero model
    // can't produce enough lift for 1-G, so nzcgs < 1. The FCS interprets
    // this as a 1-G error and drives alpha to aoamax. But on the ground
    // alpha doesn't matter — the EOM ground clamp controls attitude. Zero
    // the error when gear is down AND qsom is low (ground roll / taxi).
    // The alpha_bias is already 0 in this regime (the qsom guard above),
    // so the FCS produces alpha=0 on the ground.
    //
    // STAB-E14: require !inAir for the nzcgs<0.8 "on ground" arm.
    // Previously gear-down + ANY transient nzcgs<0.8 tick (e.g. stall
    // boundary chatter on final) latched the ground guard MID-FLIGHT: it
    // forced alpha=0, zeroing lift, which kept nzcgs<0.8 — a self-
    // reinforcing zero-lift fall (observed at digi_full_mission t=740:
    // alpha 0.0, nz 0.01, VS -5,053, gear down at 3,000 ft AGL). The
    // FM's inAir flag is authoritative ground contact.
    const bool on_ground = (!inAir && aero.gearPos > 0.5 && nzcgs < 0.8);
    const bool ground_guard = (aero.gearPos > 0.5 && qsom < 5.0) || on_ground;
    // Tranche 42: pitch-rate (q) feedback — the phugoid damper INSIDE the FCS.
    // The real F-16 FLCS has q-feedback that kills the phugoid naturally.
    // The phugoid exchanges altitude and speed at ~constant G, so the G-error
    // PI controller can't see it. But q (pitch rate) DOES change during the
    // phugoid. Subtracting kq*q from the G command opposes the pitch-rate
    // changes, damping the phugoid with zero AI-side lag.
    // Tranche 45: speed-scheduled q-damper. At high qbar (high speed) the
    // aircraft is responsive — reduce the gain to prevent over-damping.
    // At low qbar (low speed) keep the full gain. effective_gain =
    // base * sqrt(qbar_ref / qbar), capped at 2x.
    // Tranche 44/46 gate history, superseded by PHUG-PLAN P4.1 (M2):
    // The damper used to be gated OFF with gear down (T44) and below 200 ft
    // AGL (T46). Phase 2 measured the consequence: at the approach point
    // (160 kts gear+TEF+LEF) the G-loop has a 6.9x resonant peak at 0.2
    // rad/s (zeta ~= 0.073) — the damper was the missing damping, and the
    // gear gate removed it exactly where the approach porpoising lives.
    // P4.1: the 200 ft AGL flare gate stays (flare pitch-up protection,
    // and it covers the takeoff rotation where alt_agl ~= 0); the gear gate
    // becomes a 0.5x authority scale so the approach config keeps half the
    // damping authority.
    const double alt_agl = alt_agl_ft;  // passed in from FlightModel

    // --- P4.1: alpha protection (the FLCS alpha limiter) ---
    // The stall boundary is the aux table's criticalAOA (25 deg for the
    // F-16) — NOT the geometry aoaMax (35 deg, far beyond the CL break).
    // With the corrected loop gain (kp05 = 1/K_nz) the FCS faithfully
    // drives alpha to whatever the G command needs — including stall alpha
    // (measured: the P4 approach runs pulled from a 148-kt spawn straight
    // to the 35-deg clamp, stalled, and departed). The commanded alpha (PI
    // output) and the final alpha are clamped to criticalAOA minus a
    // 2.5-deg margin; the back-calculation anti-windup unwinds the
    // integrator against this clamp automatically, so sustained
    // saturation cannot accumulate.
    const double crit_aoa_deg = aux_->criticalAOA.to<f4::Degrees>().value();
    const double alpha_prot_max = (crit_aoa_deg > 0.0)
        ? std::min(aoamax, crit_aoa_deg - 2.5)
        : aoamax;

    if (fcs.pitchRateDampGain > 0.0 && alt_agl > 200.0) {
        constexpr double QBAR_REF = 18.0;  // lb/ft² at 250 kts sea level
        constexpr double GEAR_DAMP_SCALE = 0.5;  // P4.1: reduced authority,
                                                 // not a hard gate (M2 fix)
        const double gear_scale = (aero.gearPos > 0.5) ? GEAR_DAMP_SCALE : 1.0;
        const double qbar_ratio = std::sqrt(QBAR_REF / std::max(1.0, qbar));
        const double effective_gain = fcs.pitchRateDampGain * fcs.qDampScale
                                    * gear_scale
                                    * std::min(2.0, qbar_ratio);
        ptcmd -= effective_gain * pitch_rate;
        // PHUG-PLAN P0.3: the L1 loop signal actually applied this frame.
        fcs.qDamperTerm = effective_gain * pitch_rate;
    }
    const double error = ground_guard ? 0.0
                        : (ptcmd - (nzcgs - gravity_baseline - gearGravityTerm)) * fcs.kp05;
    // PHUG-PLAN P0.3: the L0 loop input signal (post-kp05, post-guard).
    fcs.piError = error;

    // --- PI controller (PHUG-PLAN P4.1) ---
    const double eprop = fcs.kp02 * error;

    // On-ground: reset pitch integrator + lead-lag filter to kill wound-up
    // state. This is a MODE change (air <-> ground), not an in-flight
    // anti-windup — the in-flight mechanism is the back-calculation below.
    // NOTE (PHUG-P4 retune): alphaBiasTrim is deliberately NOT reset here.
    // It is a trim FEEDFORWARD lag tracking a computed 1-G bias, not
    // wound-up state; resetting it every ground frame pins its output at
    // ~0, which zeroes the takeoff-roll alpha (no lift, no rotation — the
    // Brain/taxi liftoff regression) and staggers the touchdown trim. The
    // lag (5 s) self-covers any mode change; the taxi ground-alpha clamp
    // below still forces alpha = 0 while parked.
    if (ground_guard) {
        fcs.pitchIntegral.reset(0.0);
        fcs.pitchAlphaLag.reset(0.0);
    }

    // --- Real integrator + back-calculation anti-windup (P4.1, M6 fix) ---
    // ONE mechanism replaces the previous conditional-integration + QIL
    // leak + E51-shedding trio:
    //
    //  * The Adams-Bashforth 2nd-order filter is a faithful 1/s integrator
    //    — PROVIDED it is never reset() in flight. The old QIL leak called
    //    reset(eintg) every frame, which on this filter class sets u_prev
    //    (the previous INPUT) to the previous OUTPUT: the recurrence became
    //    ė = 1.5·u − 0.5·e (a lag with DC gain 2.95·kp03, tau ~= 2 s,
    //    measured in LOOP_MARGIN_REPORT §3.5). The G-loop was type 0 and
    //    tracked only through the alpha-bias feedforward.
    //
    //  * Back-calculation: when the saturated command differs from the raw
    //    PI output, feed the difference back into the integrator INPUT with
    //    tracking time constant T_aw. The integrator state then converges
    //    to the value that just sustains the clamp — no windup, no state
    //    pokes, no resets, smooth unwind by construction. This is the
    //    standard tracking (Hanüs) anti-windup form.
    constexpr double PITCH_AW_TRACK_S = 0.5;  // back-calculation time constant
    const double eintg_pre = fcs.pitchIntegral.output();
    const double aoacmd_raw = (eprop + eintg_pre) * fcs.plsdamp;
    const double aoacmd_lim = std::clamp(aoacmd_raw, aoamin, alpha_prot_max);
    const double aw_rate = (aoacmd_lim - aoacmd_raw)
                         / std::max(0.05, fcs.plsdamp) / PITCH_AW_TRACK_S;
    const double eintg = fcs.pitchIntegral.step(fcs.kp03 * error + aw_rate, dt);

    // --- Alpha command (PI output only — bias is added after the filter) ---
    // The lead-lag filter shapes ONLY the PI correction, not the bias.
    // This prevents the filter's lead term from amplifying frame-to-frame
    // bias changes (the regression documented in worklog ALT-2).
    double aoacmd = std::clamp((eprop + eintg) * fcs.plsdamp, aoamin, aoamax);
    fcs.aoacmd = angle_from_degrees(aoacmd);

    // --- Lead-lag filter (F7Tust) ---
    // Shapes the PI correction to produce the filtered alpha delta.
    // Time constants are scaled by pitchMomentum (aircraft inertia multiplier).
    const double tau1 = fcs.tp01 * aux_->pitchMomentum;
    const double tau2 = fcs.tp02 * aux_->pitchMomentum;
    const double tau3 = fcs.tp03 * aux_->pitchMomentum;
    double filtered_pi = fcs.pitchAlphaLag.step(aoacmd, tau1, tau2, tau3, dt);

    // --- Final alpha = bias + filtered PI correction ---
    // The bias provides the 1-G trim feedforward (no filter dynamics).
    // The filtered PI provides the correction on top of the bias.
    // Together: pstick=0 → PI output ≈ 0 → alpha ≈ bias (trim by construction).
    // The final alpha is bounded by the alpha protection too (P4.1) — the
    // bias itself can approach the stall boundary at very low speed.
    double new_alpha = std::clamp(alpha_bias_deg + filtered_pi,
                                  aoamin, alpha_prot_max);

    // Ground alpha clamp: on the ground with no pitch command, force alpha=0.
    if (ground_guard && ptcmd <= 0.0) {
        new_alpha = 0.0;
    }

    // --- Alpha rate (for the EOM) ---
    // Compute alpha_dot from the change in alpha across this frame.
    // alpha_dot is stored as AngularRate (rad/s canonical); convert from
    // the degree-valued finite difference at the assignment site so the
    // unit crossing is explicit.
    const double old_alpha = to_degrees(aero.alpha);
    const double new_alpha_deg = new_alpha;
    aero.alpha_dot = angular_rate_from_degrees_per_second(
        (new_alpha_deg - old_alpha) / std::max(dt, QSOM_FLOOR));
    aero.alpha = angle_from_degrees(new_alpha);
}

// ---------------------------------------------------------------------------
// runRoll: rate command with alpha-based rate limiting.
// ---------------------------------------------------------------------------
void FlightControlSystem::runRoll(double dt, double qbar, double vcas_kts,
                                   Angle alpha, double gearPos,
                                   Angle phi,
                                   const PilotInput& input,
                                   FcsState& fcs) const {
    (void)qbar;

    const double alpha_deg = to_degrees(alpha);
    const double phi_rad   = to_radians(phi);

    // --- Shaped roll stick input ---
    double rshape = input.rstick * input.rstick;
    if (input.rstick < 0.0) rshape = -rshape;
    fcs.rshape = rshape;

    // --- Commanded roll rate ---
    double pscmd = std::clamp(rshape * fcs.kr01, -fcs.kr01, fcs.kr01);

    // Alpha-based rate limiting (reduces roll authority at high alpha)
    pscmd *= applyLimiter(LimiterKey::RollRateLimiter, alpha_deg);

    // Slow-speed fade: reduce roll rate below 220 kts
    if (vcas_kts < 220.0) {
        pscmd *= std::max(0.0, vcas_kts / 220.0);
    }

    // Landing gain: reduce roll rate when gear is down
    if (gearPos > 0.5) {
        pscmd *= aux_->rollGearGain;
    }

    // --- Roll limit (from steering layer) ---
    // When maxRoll < 80 deg, the steering layer has set a roll limit.
    // Clamp the commanded roll rate to bring phi back within the limit.
    if (fcs.maxRoll < 80.0) {
        const double phi_deg = phi_rad * RTD;
        if (phi_deg > fcs.maxRoll) {
            pscmd = (fcs.maxRoll - phi_deg) * DTR * fcs.kr01;
        } else if (phi_deg < -fcs.maxRoll) {
            pscmd = (fcs.maxRoll - phi_deg) * DTR * fcs.kr01;  // note: phi_deg is negative
        }
        // Roll-rate damping: scale pscmd by proximity to target bank
        if (fcs.maxRollDelta <= 0.0) {
            pscmd = 0.0;
        } else {
            double scale = std::max(0.0, std::min(1.0,
                1.0 - (fcs.startRoll * RTD) / std::max(0.01, fcs.maxRollDelta)));
            pscmd *= scale;
        }
    }
    fcs.pscmd = pscmd;

    // --- Filter the roll rate command (first-order lag) ---
    const double tau = fcs.tr01 * aux_->rollMomentum;
    fcs.pstab = fcs.rollRateLag.step(pscmd, tau, dt);
}

// ---------------------------------------------------------------------------
// runYaw: beta-command PI controller (yaw damper + coordinated-turn beta
// compensation). The PI output drives aero.beta directly; the EOM computes
// yaw rate `r` from the resulting side force `nycgw`. With the correct sign
// of fcs.ky05 (preserved in computeGains) this is a NEGATIVE feedback loop
// that damps sideslip — the coordinated-turn damping the aircraft needs
// during banked flight. See FLIGHT_CONTROL_STABILITY_PLAN.md §4.1 RC-1.
//
// Ground guard: when gear is down AND qsom is low (taxi / takeoff roll),
// beta is held at 0 — the EOM's nose-wheel steering controls heading
// directly, and the aero model can't produce meaningful side force at
// low qbar anyway.
// ---------------------------------------------------------------------------
void FlightControlSystem::runYaw(double dt, double qbar, double qsom,
                                  double vt, double vcas_kts,
                                  Angle beta, double nycgw,
                                  double betmin, double betmax,
                                  const PilotInput& input,
                                  FcsState& fcs, AeroState& aero) const {
    // qbar/vt/vcas_kts are unused (the yaw loop is dimensionless in G/Beta
    // space); beta/nycgw/betmin/betmax drive the PI controller. The (void)
    // casts are retained for qbar/vt/vcas_kts only.
    (void)qbar; (void)vt; (void)vcas_kts;

    // --- Shaped pedal input ---
    double yshape = input.ypedal * input.ypedal;
    if (input.ypedal < 0.0) yshape = -yshape;
    fcs.yshape = yshape;

    // --- Commanded beta ---
    double nycmd = std::clamp(yshape * 2.0, -2.0, 2.0);

    // Limit to available side force
    const double gsAvail = geom_->betaMax.to<f4::Degrees>().value() * 0.05 * qsom / GRAVITY;
    nycmd *= std::min(gsAvail / 2.0, 1.0);
    nycmd = applyLimiter(LimiterKey::YawAlphaLimiter, nycmd);

    // --- PI controller (computed but not used — see note above) ---
    const double error = (nycmd + nycgw) * fcs.ky05;
    const double eprop = fcs.ky02 * error;
    double eintg = fcs.yawIntegral.step(fcs.ky03 * error, dt);
    eintg = std::clamp(eintg, betmin, betmax);
    double betcmd = std::clamp((eprop + eintg) * fcs.ylsdamp, betmin, betmax);
    fcs.betcmd = angle_from_degrees(betcmd);

    // --- Apply commanded beta to the aero state (Phase A1, revised NAV-C) ---
    //
    // Previously this was stubbed (`aero.beta = 0`). Phase A1 un-stubbed it
    // as an nycgw-regulating "damper", but the regulation target is wrong:
    // nycgw = ywaero/g includes the wind-axes bookkeeping term
    // -xsaero*sin(beta) = +drag*sin(beta) (aerodynamics.cpp:290) — the
    // rotation of DRAG into the wind axes, not a physical side force to be
    // nulled. Regulating ywaero -> 0 with beta as the actuator is positive
    // feedback through that term: the trace shows betcmd railing from the
    // first tick and pinning |beta| at the 15-deg aero clamp for the ENTIRE
    // flight (course_intercept t=0-260: beta 15.00 constant, wings level
    // or banked, zero pedal) — a permanent ~50 ft/s lateral drift.
    //
    // In this EOM the FCS sets aero.beta DIRECTLY (there is no rudder
    // actuator): with the pedals centered the commanded sideslip IS zero —
    // that is the definition of coordinated flight in this model (beta=0
    // => ywaero=0 => yaw rate comes purely from the bank-kinematics term
    // in eom.cpp:195). So: pedal centered -> beta = 0, integrator reset.
    // The PI shaper below remains only for deliberate pilot sideslip
    // commands (nothing in the AI commands airborne pedal; ground steering
    // is nosewheel-authority and gated off by the gear guard below).
    constexpr double PEDAL_DEADBAND = 1e-6;
    if (std::fabs(input.ypedal) < PEDAL_DEADBAND) {
        fcs.yawIntegral.reset(0.0);
        fcs.betcmd = zero_angle();
    } else {
        fcs.betcmd = angle_from_degrees(betcmd);
    }

    // Ground guard: on the ground at low speed, the gear clamp controls
    // heading directly via nose-wheel steering, and the aero model can't
    // produce meaningful side force. Hold beta at 0 in that regime to
    // avoid spurious transients during the takeoff roll.
    if (aero.gearPos > 0.5) {
        aero.beta = zero_angle();
        aero.beta_dot = zero_angular_rate();
    } else {
        aero.beta = fcs.betcmd;
        // beta_dot: finite-difference from the previous frame's beta.
        // (Mirrors the alpha_dot computation in runPitch.)
        const double old_beta_deg = to_degrees(beta);
        aero.beta_dot = angular_rate_from_degrees_per_second(
            (to_degrees(fcs.betcmd) - old_beta_deg) / std::max(dt, QSOM_FLOOR));
    }
}

}  // namespace f4::flight
