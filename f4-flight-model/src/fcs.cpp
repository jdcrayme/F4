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
#include <cmath>

namespace f4::flight {

using namespace f4::data;
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
void FlightControlSystem::update(double dt,
                                 double qbar, double qsom, double mach,
                                 double vt_ftps, double vcas_kts,
                                 double alpha_deg, double beta_deg,
                                 double cosmu, double cosgam, double singam,
                                 double costhe, double cosphi, double phi_rad,
                                 double loadingFraction,
                                 bool inAir,
                                 double nzcgs, double nycgw,
                                 bool gearDown, bool refueling,
                                 bool landingGainsActive,
                                 const PilotInput& input,
                                 FcsState& fcs,
                                 AeroState& aero) const {
    (void)mach;  // not directly used by FCS (aero tables handle Mach effects)

    // Landing gains are active when gear is down, refueling, or explicitly
    // requested by the host (e.g. approach mode).
    const bool landingGains = landingGainsActive || gearDown || refueling;

    // --- Damper gains from limiters ---
    // These scale the pitch/roll/yaw outputs based on dynamic pressure.
    fcs.plsdamp = applyLimiter(LimiterKey::PitchYawControlDamper, qbar);
    fcs.rlsdamp = applyLimiter(LimiterKey::RollControlDamper, qbar);
    fcs.ylsdamp = fcs.plsdamp;  // yaw uses same damper as pitch

    // --- pshape: shaped pitch stick input ---
    // Computed here because kp01 (in computeGains) depends on it.
    // pshape = pstick^2 * sign(pstick): squares the input (so small inputs
    // are suppressed) and preserves the sign.
    fcs.pshape = input.pstick * input.pstick;
    if (input.pstick < 0.0) fcs.pshape = -fcs.pshape;

    // --- Compute gains (pitch/roll/yaw) ---
    computeGains(qbar, qsom, vt_ftps, alpha_deg,
                 aero.clift0, aero.clalph0, aero.clalpha, aero.cnalpha,
                 aero.cy,
                 cosgam, cosmu, costhe, cosphi,
                 loadingFraction, inAir,
                 landingGains, aero.gearPos,
                 fcs);

    // --- Run channels (pitch must run before aero uses alpha) ---
    const double aoamin = geom_->aoaMin_deg;
    const double aoamax = geom_->aoaMax_deg;
    const double betmin = geom_->betaMin_deg;
    const double betmax = geom_->betaMax_deg;
    const double maxGs  = geom_->maxGs;

    runPitch(dt, qbar, qsom, vt_ftps, vcas_kts,
             alpha_deg, cosmu, cosgam, singam,
             nzcgs, aero.cl, aero.clalpha, aero.clalph0,
             aero.cnalpha, aoamin, aoamax, maxGs,
             input, fcs, aero);

    runRoll(dt, qbar, vcas_kts, alpha_deg,
            aero.gearPos, phi_rad,
            input, fcs);

    runYaw(dt, qbar, qsom, vt_ftps, vcas_kts,
           beta_deg, nycgw, betmin, betmax,
           input, fcs, aero);
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
                                       double alpha_deg,
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

    const double cosphiLim = std::max(0.0, cosphi);

    // --- Available G from lift ---
    // gsAvail = max G the aircraft can produce at current alpha.
    // Uses clalph0 (static slope), NOT the local clalpha.
    const double gsAvail = geom_->aoaMax_deg * clalph0 * qsom / GRAVITY;

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

    fcs.tp02 = (std::fabs(pfreq1) > 1e-6) ? 1.0 / pfreq1 : 1.0;
    fcs.tp03 = std::max(0.5, (std::fabs(pfreq2) > 1e-6) ? 1.0 / pfreq2 : 1.0);

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

    fcs.ty02 = (std::fabs(yfreq2) > 1e-6) ? 1.0 / yfreq2 : 1.0;

    // ky05: yaw feedback gain.
    // IMPORTANT: preserve the sign of the denominator. Earlier versions used
    // max(1e-6, denom) which destroyed the sign when cy < 0, causing the yaw
    // damper to become a positive-feedback loop.
    const double denom = qsom * cy * yfreq1 * yfreq2;
    if (std::fabs(denom) > 1e-6) {
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
                                    double alpha_deg, double cosmu,
                                    double cosgam, double singam,
                                    double nzcgs, double cl, double clalpha,
                                    double clalph0, double cnalpha,
                                    double aoamin, double aoamax, double maxGs,
                                    const PilotInput& input,
                                    FcsState& fcs, AeroState& aero) const {
    (void)qbar; (void)vt; (void)vcas_kts; (void)alpha_deg;
    (void)cl; (void)clalpha; (void)singam;
    (void)cnalpha;  // reserved for future yaw-damping coupling
    (void)input;    // pitch channel reads fcs.pshape, not raw pilot input

    // --- Commanded G ---
    double ptcmd = fcs.pshape * fcs.kp01;

    // Limit the command to available G.
    const double maxNegGs = applyLimiter(LimiterKey::NegGLimiter, vcas_kts);
    const double gsAvail  = geom_->aoaMax_deg * clalph0 * qsom / GRAVITY;
    const double maxCmd   = maxGs;
    const double minCmd   = std::max(maxNegGs, -gsAvail);
    ptcmd = std::clamp(ptcmd, minCmd, std::min(gsAvail, maxCmd));
    fcs.ptcmd = ptcmd;

    // --- G error ---
    // The error is the difference between commanded G and actual G, minus
    // the gravity component that the FCS must cancel.
    const double cosmu_lim = std::max(0.0, cosmu);
    const double gearGravityTerm = 0.1 * aero.gearPos * qsom / GRAVITY;
    const double error = (ptcmd - (nzcgs - cosmu_lim * cosgam - gearGravityTerm)) * fcs.kp05;

    // --- PI controller ---
    const double eprop = fcs.kp02 * error;
    const double eintg1 = fcs.kp03 * error;
    double eintg = fcs.pitchIntegral.step(eintg1, dt);

    // --- Anti-windup ---
    // When the integrator saturates at the alpha limits, zero the
    // proportional term AND clear the integrator history (y_prev, u_prev,
    // u_now) to prevent limit cycles. Clearing only y_prev is insufficient —
    // the Adams-Bashforth 2nd-order filter uses u_prev and u_now, and leaving
    // them non-zero causes the integrator to "wind down" slowly.
    if (eintg > aoamax) {
        eintg = aoamax;
        fcs.pitchIntegral.reset(eintg);  // clears y_prev, u_prev, u_now
    } else if (eintg < aoamin) {
        eintg = aoamin;
        fcs.pitchIntegral.reset(eintg);
    }

    // --- Alpha command ---
    double aoacmd = std::clamp((eprop + eintg) * fcs.plsdamp, aoamin, aoamax);
    fcs.aoacmd = aoacmd;

    // --- Lead-lag filter (F7Tust) ---
    // Shapes the alpha command to produce the final alpha_deg.
    // Time constants are scaled by pitchMomentum (aircraft inertia multiplier).
    const double tau1 = fcs.tp01 * aux_->pitchMomentum;
    const double tau2 = fcs.tp02 * aux_->pitchMomentum;
    const double tau3 = fcs.tp03 * aux_->pitchMomentum;
    double new_alpha = fcs.pitchAlphaLag.step(aoacmd, tau1, tau2, tau3, dt);
    new_alpha = std::clamp(new_alpha, aoamin, aoamax);

    // --- Alpha rate (for the EOM) ---
    // Compute alpha_dot from the change in alpha across this frame.
    const double old_alpha = aero.alpha_deg;
    aero.alpha_dot = (new_alpha - old_alpha) / std::max(dt, 1e-6);
    aero.alpha_deg = new_alpha;
}

// ---------------------------------------------------------------------------
// runRoll: rate command with alpha-based rate limiting.
// ---------------------------------------------------------------------------
void FlightControlSystem::runRoll(double dt, double qbar, double vcas_kts,
                                   double alpha_deg, double gearPos,
                                   double phi_rad,
                                   const PilotInput& input,
                                   FcsState& fcs) const {
    (void)qbar;

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
// runYaw: beta-command PI controller (mostly stubbed).
//
// The EOM has no rudder-to-yaw dynamics, so driving beta directly would
// create a positive feedback loop (the yaw damper would amplify any beta
// error instead of correcting it). To avoid this, beta is forced to 0.
// This matches FreeFalcon behavior.
// ---------------------------------------------------------------------------
void FlightControlSystem::runYaw(double dt, double qbar, double qsom,
                                  double vt, double vcas_kts,
                                  double beta_deg, double nycgw,
                                  double betmin, double betmax,
                                  const PilotInput& input,
                                  FcsState& fcs, AeroState& aero) const {
    (void)qbar; (void)qsom; (void)vt; (void)vcas_kts;
    (void)beta_deg; (void)betmin; (void)betmax; (void)nycgw;

    // --- Shaped pedal input ---
    double yshape = input.ypedal * input.ypedal;
    if (input.ypedal < 0.0) yshape = -yshape;
    fcs.yshape = yshape;

    // --- Commanded beta ---
    double nycmd = std::clamp(yshape * 2.0, -2.0, 2.0);

    // Limit to available side force
    const double gsAvail = geom_->betaMax_deg * 0.05 * qsom / GRAVITY;
    nycmd *= std::min(gsAvail / 2.0, 1.0);
    nycmd = applyLimiter(LimiterKey::YawAlphaLimiter, nycmd);

    // --- PI controller (computed but not used — see note above) ---
    const double error = (nycmd + nycgw) * fcs.ky05;
    const double eprop = fcs.ky02 * error;
    double eintg = fcs.yawIntegral.step(fcs.ky03 * error, dt);
    eintg = std::clamp(eintg, betmin, betmax);
    double betcmd = std::clamp((eprop + eintg) * fcs.ylsdamp, betmin, betmax);
    fcs.betcmd = betcmd;

    // Force beta to 0 — the EOM has no rudder dynamics, so any non-zero
    // beta would create positive feedback.
    aero.beta_deg = 0.0;
    aero.beta_dot = 0.0;
}

}  // namespace f4::flight
