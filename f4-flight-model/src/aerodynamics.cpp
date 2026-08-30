// f4-flight-model/aerodynamics.cpp
//
// Aerodynamics force model implementation.
//
// Ported from F4Flight's aerodynamics.cpp, which is a port of FreeFalcon's
// aero.cpp. The force-build formulas and the stall model match FreeFalcon
// exactly; several bugs in earlier F4Flight versions were fixed by
// comparing against the FF source (documented inline).
//
// Key formulas:
//   - CL/CD/CY from bilinear interpolation of Mach x alpha tables
//   - Ground effect: CL *= 1.13 within 0.2*span of ground, fading to 1.0
//     by 1.0*span
//   - Flap factors: TEF increases CL and CD; LEF increases CD
//   - Drag additions: speed brake, gear, external stores
//   - Stall model: when criticalAOA > 0 and alpha > 10, compute stall speed
//     and reduce lift if vcas < stallSpeed or alpha > criticalAOA
//   - Force transformation: body = stability rotated by alpha; wind =
//     stability rotated by beta

#include "f4/flight/aerodynamics.hpp"
#include "f4/flight/stall_state.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace f4::flight {

using f4::data::AeroTable;
using f4::data::AircraftGeometry;
using f4::data::AuxAero;
using f4::data::makeClTable;
using f4::data::makeCdTable;
using f4::data::makeCyTable;

// ---------------------------------------------------------------------------
// Construction: build Table2D views from the config's raw vectors.
// ---------------------------------------------------------------------------
Aerodynamics::Aerodynamics(const AeroTable* table,
                           const AircraftGeometry* geom,
                           const AuxAero* aux)
    : table_(table), geom_(geom), aux_(aux) {
    assert(table_ != nullptr && "Aerodynamics: table must not be null");
    assert(geom_  != nullptr && "Aerodynamics: geom must not be null");
    assert(aux_   != nullptr && "Aerodynamics: aux must not be null");
    if (table_) {
        cl_ = makeClTable(*table_);
        cd_ = makeCdTable(*table_);
        cy_ = makeCyTable(*table_);
    }
}

// ---------------------------------------------------------------------------
// update(): compute aerodynamic forces for one time step.
//
// alpha / beta are passed as the strong Angle type. Internally we extract
// the degree value once (the F-16 aero tables are degree-indexed and we
// do NOT convert them — converting the data would alter the flight feel).
// All inline arithmetic that needs radians uses to_radians(alpha) so the
// unit crossing is explicit at each call site.
// ---------------------------------------------------------------------------
void Aerodynamics::update(const AeroInputs& in, AeroState& aero) const {
    assert(table_ != nullptr && "Aerodynamics: table must not be null");
    assert(geom_  != nullptr && "Aerodynamics: geom must not be null");
    assert(aux_   != nullptr && "Aerodynamics: aux must not be null");

    // Unpack for readability — the struct groups these for type safety
    // at the call site, but internally the math is identical.
    const Angle  alpha      = in.alpha;
    const Angle  beta       = in.beta;
    const double mach       = in.mach;
    const double vt_ftps    = in.vt_ftps;
    const double qbar       = in.qbar;
    const double qsom       = in.qsom;
    const double groundZ_ft = in.groundZ_ft;
    const double z_ft       = in.z_ft;
    const double vcas_kts   = in.vcas_kts;
    const double pstick     = in.pstick;
    // altitude_ft is unused — ground effect uses AGL = |groundZ - z| instead

    if (!table_ || !geom_ || !aux_) {
        aero.lift = aero.drag = 0.0;
        aero.xaero = aero.yaero = aero.zaero = 0.0;
        return;
    }

    // --- Extract degree values for table lookups (the single place the
    // degree convention survives; the F-16 aero tables are physically
    // indexed in degrees). ---
    const double alpha_deg = to_degrees(alpha);
    const double beta_deg  = to_degrees(beta);

    // --- Trigonometry ---
    const double alp_rad = to_radians(alpha);
    const double bet_rad = to_radians(beta);
    const double cosalp = std::cos(alp_rad);
    const double sinalp = std::sin(alp_rad);
    const double cosbet = std::cos(bet_rad);
    const double sinbet = std::sin(bet_rad);

    // --- Flap factors ---
    // TEF/LEF positions are normalized 0..1 (stored in aero.tefPos/lefPos).
    const double tefFactor = aero.tefPos;
    const double lefFactor = aero.lefPos;

    // Effective alpha for CL lookup: alpha + tef - lef.
    // TEF increases effective alpha (more lift); LEF decreases it (delays stall).
    const double tempAlpha = alpha_deg + tefFactor - lefFactor;

    // --- Coefficient lookups (bilinear interpolation) ---
    double cl = cl_(mach, tempAlpha) * table_->clFactor;
    double cy = 0.0;
    if (cy_) {
        cy = (*cy_)(mach, alpha_deg) * table_->cyFactor;
    }

    // Drag uses a "reduced alpha" so corners bleed speed less aggressively.
    // dragAlpha = max(|beta|*0.6, tempAlpha)
    double dragAlpha = std::fabs(beta_deg) * 0.6;
    if (dragAlpha < tempAlpha) dragAlpha = tempAlpha;
    double cd = cd_(mach, dragAlpha) * table_->cdFactor;

    // --- Scale for flaps ---
    // TEF increases CL by CLtefFactor and CD by CDtefFactor.
    // LEF increases CD by CDlefFactor.
    cl *= (1.0 + tefFactor * aux_->CLtefFactor);
    cd *= (1.0 + tefFactor * aux_->CDtefFactor + lefFactor * aux_->CDlefFactor);

    // Drag chute (if deployed)
    if (aero.dragChutePos > 0.5) {
        cd += aux_->dragChuteCd * aero.dragChutePos;
    }

    // --- Local lift-curve slope (3-point finite difference) ---
    // Matches FreeFalcon aero.cpp:210-252 exactly.
    //
    // IMPORTANT: FreeFalcon uses RAW interpolated values for cl1/cl2/cd1/cd2
    // (no clFactor, no cdFactor, no TEF factor). The TEF factor is applied
    // ONCE in the clalpha/cnalpha formulas. Earlier F4Flight versions
    // applied clFactor AND TEF to cl1/cl2, then applied TEF AGAIN in
    // clalpha — double counting.
    //
    // Also: cnalpha must use CDtefFactor (not CLtefFactor) per FreeFalcon
    // line 231.
    const double cl1_raw = cl_(mach, tempAlpha - 2.0);
    const double cl2_raw = cl_(mach, tempAlpha + 2.0);
    const double cd1_raw = cd_(mach, dragAlpha - 2.0);
    const double cd2_raw = cd_(mach, dragAlpha + 2.0);

    // clalpha = dCL/dalpha (per radian), using 4-degree finite difference.
    // The *0.25 converts the 4-degree span to per-radian: (cl2-cl1)/(4*deg_per_rad).
    double clalpha = (cl2_raw - cl1_raw) * 0.25 * (1.0 + tefFactor * aux_->CLtefFactor);

    // cnalpha = dCN/dalpha (normal-force slope, used by FCS pitch gain).
    // CN = CL*cos(alpha) + CD*sin(alpha), so dCN/dalpha includes both CL and CD slopes.
    double cnalpha = ((cl2_raw - cl1_raw) * cosalp + (cd2_raw - cd1_raw) * sinalp) * 0.25 *
                     (1.0 + tefFactor * aux_->CDtefFactor);  // CDtefFactor, not CLtefFactor

    // --- Static lift-curve slope (alpha = 0..10 deg) ---
    // Used by the FCS to estimate available G. FreeFalcon: cl1/cl2 are RAW
    // (no clFactor). clift0 does NOT include clFactor (matches FreeFalcon line 252).
    const double cls0_raw = cl_(mach, 0.0);
    const double cls1_raw = cl_(mach, 10.0);
    double clalph0 = (cls1_raw - cls0_raw) * 0.1 * (1.0 + tefFactor * aux_->CLtefFactor);
    double clift0 = cls0_raw * (1.0 + tefFactor * aux_->CLtefFactor);

    // --- Ground effect ---
    // Within 0.2*span of the ground: CL *= 1.13 (ground cushion).
    // Between 0.2 and 1.0 span: fades linearly back to 1.0.
    // groundZ_ft is terrain altitude (MSL, positive up); z is NED.
    const double span = geom_->span.value();
    const double agl_ft = std::fabs(-z_ft - groundZ_ft);
    if (agl_ft < span * 0.2) {
        // Close to ground: full ground effect
        const double g = GROUND_EFFECT_CL_MULT;
        cl *= g; clalpha *= g; cnalpha *= g;
    } else if (agl_ft < span) {
        // Transition zone: linear fade from 1.13 to 1.0
        const double f = GROUND_EFFECT_CL_MULT - ((agl_ft - span * 0.2) / (span * 0.8)) * (GROUND_EFFECT_CL_MULT - 1.0);
        cl *= f; clalpha *= f; cnalpha *= f;
    }

    // --- Drag additions (speed brake, gear, stores) ---
    cd += aux_->CDSPDBFactor * aero.dbrake;
    cd += aux_->CDLDGFactor  * aero.gearPos;
    cd += aero.cdStores;

    // --- Stall model ---
    // Matches FreeFalcon aero.cpp:293-356.
    //
    // When criticalAOA > 0 and alpha > 10 deg, compute the stall speed:
    //   stallSpeed = 17.16 * sqrt((W/S) / |CL|)
    // If vcas < stallSpeed OR alpha > criticalAOA, the aircraft is stalled
    // and lift is reduced (goes to 0 or negative, scaled by speed ratio).
    bool stalled = false;
    double stallSpeed = 0.0;
    if (aux_->criticalAOA.value() > 0.0 && geom_->area.value() > 0.0 && alpha_deg > 10.0) {
        // Recover mass from qsom: qsom = q*S/m  =>  m = q*S/qsom
        const double q_val = qbar;
        const double S = geom_->area.value();
        const double mass_from_qsom = (qsom > QSOM_FLOOR) ? (q_val * S / qsom) : 1.0;
        const double weight_lbs = mass_from_qsom * GRAVITY;
        const double ws = weight_lbs / S;  // wing loading W/S
        if (std::fabs(cl) > 1e-3) {
            stallSpeed = K_STALL * std::sqrt(ws / std::fabs(cl));
        }
        // STAB-E2: hysteresis on the stall boundary. The previous hard
        // comparison (vcas < stallSpeed || alpha > criticalAOA) chattered
        // at the boundary: an alpha oscillation riding criticalAOA flipped
        // lift between full CL and <=0 EVERY FRAME (observed as nz snapping
        // 1.2 -> 0.01 with alpha 13.8 -> 0.0 in one tick in the
        // digi_full_mission trace — the "ballistic arc" that amplified the
        // altitude phugoid). Enter the stall with a margin above the
        // boundary; leave it only with a margin below. This matches the
        // physical reality that flow separation/reattachment lags the
        // boundary crossing.
        const double crit_deg = aux_->criticalAOA.to<f4::Degrees>().value();
        const double aoa_enter = crit_deg + 1.0;
        const double aoa_exit  = crit_deg - 3.0;
        const double spd_enter = stallSpeed;               // enter below stall speed
        const double spd_exit  = stallSpeed + 5.0;         // exit only with margin
        if (aero.stalled) {
            // Latched: stay stalled until BOTH conditions clear with margin.
            stalled = !(alpha_deg < aoa_exit && vcas_kts > spd_exit);
        } else {
            stalled = (vcas_kts < spd_enter || alpha_deg > aoa_enter);
        }
    }

    // --- Force summation ---
    // Forces are ACCELERATIONS (ft/s^2 = force/mass).
    // lift = CL * qsom, drag = CD * qsom.
    //
    // Stall force model (matches FreeFalcon aero.cpp:319-327):
    //   FlatSpin  -> lift = 0 (terminal, no lift whatsoever)
    //   Stalled   -> lift = min(0, cl*0.5) * (vcas/stallSpeed) * qsom
    //                (drives lift toward 0 or negative, aircraft falls)
    //   Normal    -> lift = cl * qsom
    //
    // The stallState is read from AeroState (set by the FlightModel's stall
    // SM at the end of the previous frame). This introduces a 1-frame latency
    // (the SM state lags the aero detection by one minor frame) which is
    // negligible at 240 Hz.
    double lift;
    // Use the typed StallState enum instead of raw int comparison.
    // Previously this was `const int stallState = aero.stallState; if (stallState == 4)`
    // which was a magic-number comparison — fragile and unclear.
    const StallState stallState = aero.stallState;
    if (stallState == StallState::FlatSpin) {  // FlatSpin: lift = 0
        lift = 0.0;  // FreeFalcon aero.cpp:319: lift = 0.0f
    } else if (stalled && stallSpeed > MIN_VT) {
        const double cl_stalled = std::min(0.0, cl * 0.5);
        lift = cl_stalled * (vcas_kts / stallSpeed) * qsom;
    } else if (vt_ftps < MIN_VT) {
        lift = 0.0;  // avoid NaN at zero airspeed
    } else {
        lift = cl * qsom;
    }
    const double drag = cd * qsom;

    // --- Transform forces to body, stability, and wind axes ---
    //
    // Body axes (ft/s^2):
    //   xaero = -D*cos(a) + L*sin(a)
    //   zaero = -L*cos(a) - D*sin(a)
    //   yaero =  Cy * qsom * (beta - |beta|*yshape*0.5)
    //
    // The yshape term couples pitch stick into side force (for trim drag
    // at high G). Matches FreeFalcon aero.cpp.
    const double yshape = pstick * pstick * (pstick >= 0 ? 1.0 : -1.0);
    const double yaero = cy * qsom * (beta_deg - std::fabs(beta_deg) * yshape * 0.5);

    const double xaero = -drag * cosalp + lift * sinalp;
    const double zaero = -lift * cosalp - drag * sinalp;

    // Stability axes (ft/s^2):
    //   xsaero = -drag
    //   ysaero =  yaero
    //   zsaero = -lift
    const double xsaero = -drag;
    const double ysaero = yaero;
    const double zsaero = -lift;

    // Wind axes (ft/s^2): stability axes rotated by beta
    //   xwaero =  xsaero * cos(beta) + ysaero * sin(beta)
    //   ywaero = -xsaero * sin(beta) + ysaero * cos(beta)
    //   zwaero =  zsaero
    const double xwaero =  xsaero * cosbet + ysaero * sinbet;
    const double ywaero = -xsaero * sinbet + ysaero * cosbet;
    const double zwaero =  zsaero;

    // --- Store results ---
    aero.cl = cl;
    aero.cd = cd;
    aero.cy = cy;
    aero.clalpha = clalpha;
    aero.clalph0 = clalph0;
    aero.clift0 = clift0;
    aero.cnalpha = cnalpha;
    aero.lift = lift;
    aero.drag = drag;
    aero.xaero = xaero;
    aero.yaero = yaero;
    aero.zaero = zaero;
    aero.xsaero = xsaero;
    aero.ysaero = ysaero;
    aero.zsaero = zsaero;
    aero.xwaero = xwaero;
    aero.ywaero = ywaero;
    aero.zwaero = zwaero;
    aero.stalled = stalled;
    aero.stallSpeed = stallSpeed;
}

}  // namespace f4::flight
