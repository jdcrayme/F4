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
//   Anti-windup: when the integrator saturates at the alpha limits, the
//   proportional term is zeroed and the integrator history is cleared to
//   prevent limit cycles.
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
    fcs.kp02 = 1.0;    // proportional gain
    fcs.kp03 = 2.0;    // integral gain

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

    // --- kp05: pitch feedback gain ---
    // Differs between AOA-command mode and G-command mode.
    // AOA-command mode is used when the aircraft can't produce enough G
    // to hit maxGs (i.e., gsAvail <= maxGs).
    fcs.aoaCmdModeRuntime = (gsAvail <= geom_->maxGs);

    if (fcs.aoaCmdModeRuntime || qsom * cnalpha == 0.0) {
        // AOA-command mode: kp05 places the closed-loop pole
        fcs.kp05 = fcs.tp02 * fcs.tp03 * wp01 * wp01;
    } else {
        // G-command mode: kp05 includes the normal-force slope
        fcs.kp05 = GRAVITY * fcs.tp02 * fcs.tp03 * wp01 * wp01 / (qsom * cnalpha);
    }

    // Ground fade: at very low qbar, reduce kp05 to avoid excessive alpha
    // commands during taxi.
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
    }

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
    // Tranche 44: gate off during takeoff/landing (gear down).
    // Tranche 46: gate off below 200 ft AGL (flare zone) — the high gain
    // fights the flare pitch-up. Also gate off when gear down (takeoff/landing).
    const double alt_agl = alt_agl_ft;  // passed in from FlightModel
    if (fcs.pitchRateDampGain > 0.0 && aero.gearPos < 0.5 && alt_agl > 200.0) {
        constexpr double QBAR_REF = 18.0;  // lb/ft² at 250 kts sea level
        const double qbar_ratio = std::sqrt(QBAR_REF / std::max(1.0, qbar));
        const double effective_gain = fcs.pitchRateDampGain * std::min(2.0, qbar_ratio);
        ptcmd -= effective_gain * pitch_rate;
    }
    const double error = ground_guard ? 0.0
                        : (ptcmd - (nzcgs - gravity_baseline - gearGravityTerm)) * fcs.kp05;

    // --- PI controller ---
    const double eprop = fcs.kp02 * error;
    const double eintg1 = fcs.kp03 * error;

    // On-ground: reset pitch integrator + lead-lag filter to kill wound-up state.
    if (ground_guard) {
        fcs.pitchIntegral.reset(0.0);
        fcs.pitchAlphaLag.reset(0.0);
    }

    // --- Conditional integration anti-windup ---
    // Standard anti-windup: STOP integrating when the integrator is at a
    // limit AND the new error would push it further into saturation. This
    // is in contrast to the earlier "reset the integrator to the limit
    // value" approach, which produced step changes in aoacmd every time
    // the saturation released — those steps propagated through the
    // lead-lag and the EOM into a low-frequency pitch oscillation (the
    // "altitude phugoid" symptom in FLIGHT_CONTROL_STABILITY_PLAN.md
    // §4.2 RC-1). Conditional integration preserves the integrator's
    // value across saturation and lets it unwind smoothly when the error
    // reverses.
    double eintg = fcs.pitchIntegral.output();  // last integrated value
    const bool at_upper = (eintg >= aoamax && eintg1 > 0.0);
    const bool at_lower = (eintg <= aoamin && eintg1 < 0.0);
    if (!at_upper && !at_lower) {
        eintg = fcs.pitchIntegral.step(eintg1, dt);
    }
    // Clamp to the limit range (the step may overshoot by one frame's worth).
    if (eintg > aoamax) eintg = aoamax;
    else if (eintg < aoamin) eintg = aoamin;

    // EXPERIMENT QIL: Slow integrator leak. The pitch integrator has
    // anti-windup (stops at limits) and shedding (drains under strong
    // opposition), but no slow leak. In sustained flight it slowly winds
    // up to compensate for the FCS's own 1-G bias inaccuracy (the alpha_bias
    // formula uses approximations), then slowly unwinds through the shedding
    // mechanism when the error reverses — creating a 20s phugoid. A slow
    // leak (120s time constant) lets the integrator hold trim in the short
    // term while preventing the slow windup. The leak factor is small enough
    // that the integrator still tracks real trim changes (the P term
    // dominates during transients, the I term only settles the residual).
    constexpr double INTEGRATOR_LEAK_PER_S = 1.0 / 120.0;  // 120s time constant
    eintg *= std::max(0.0, 1.0 - INTEGRATOR_LEAK_PER_S * dt);
    fcs.pitchIntegral.reset(eintg);

    // --- STAB-E51: integrator shedding under strong opposition ---
    // The conditional-integration anti-windup above stops further winding
    // but does not speed the UNWIND: with the integrator holding alpha at
    // its clamp, a sustained opposing stick command (the AI's beam-ride
    // balloon: +1,100 fpm climb against ptcmd -0.35 for 14 s, digi trace
    // fix26 t=1245-1258) is consumed by the integrator's own time
    // constant first. Shed a fraction of the integrator toward zero each
    // frame — but ONLY when a strong stick deflection is failing to move
    // the achieved G in its direction (the observed stuck-trim states).
    // A broad "integrator opposes error" rule also drains the integrator
    // during LEGITIMATE trim-building and pins the aircraft at the 1-G
    // bias trim (fix27: a steady -900 fpm enroute descent that ignored a
    // +1,700 ft altitude error for 90 s).
    const bool strong_push_fails = (ptcmd < -0.15 && nzcgs > 1.05);
    const bool strong_pull_fails = (ptcmd >  0.15 && nzcgs < 0.95);
    if ((strong_push_fails && eintg > 0.0) ||
        (strong_pull_fails && eintg < 0.0)) {
        eintg *= std::max(0.0, 1.0 - 1.5 * dt);
        fcs.pitchIntegral.reset(eintg);
    }

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
    double new_alpha = std::clamp(alpha_bias_deg + filtered_pi, aoamin, aoamax);

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
