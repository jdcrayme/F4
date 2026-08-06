// f4-flight-model/atmosphere.hpp
//
// 3-layer standard atmosphere model.
//
// Computes air density, pressure, temperature, speed of sound, Mach number,
// dynamic pressure, and calibrated airspeed from altitude and true airspeed.
//
// Ported from F4Flight's atmosphere.h, which is a port of FreeFalcon's
// atmos.cpp. The 3-layer model (troposphere / lower stratosphere / upper
// stratosphere) matches the 1962 US Standard Atmosphere.
//
// Units: Imperial (ft, slugs/ft^3, lb/ft^2, ft/s, Rankine).
// Input:  altitude in feet (positive upward), true airspeed in ft/s.
// Output: density (rho), pressure (pa), temperature ratio (ttheta),
//         density ratio (rsigma), pressure ratio (pdelta), speed of sound,
//         Mach number, dynamic pressure (qbar), calibrated airspeed (vcas).

#pragma once

#include "f4/flight/constants.hpp"

#include <algorithm>
#include <cmath>

namespace f4::flight {

/// Output of the atmosphere computation.
struct AtmosphereOutput {
    double rho{0.0};       // air density (slugs/ft^3)
    double pa{0.0};        // ambient pressure (lb/ft^2)
    double ttheta{1.0};    // temperature ratio T/T0
    double rsigma{1.0};    // density ratio rho/rho0
    double pdelta{1.0};    // pressure ratio P/P0
    double sound{AASL};    // speed of sound (ft/s)
    double mach{0.0};      // Mach number
    double qbar{0.0};      // dynamic pressure (lb/ft^2)
    double qovt{0.0};      // qbar / vt
    double qsom{0.0};      // normalized dynamic pressure = q*S/m (ft/s^2 per unit CL)
    double vcas{0.0};      // calibrated airspeed (knots)
};

/// Compute the pressure ratio (P/P0) and temperature/density ratios
/// (T/T0, rho/rho0) using the 3-layer standard atmosphere model.
///
/// alt_ft is in FEET, POSITIVE UPWARD.
///
/// Layer 1 (troposphere, 0 to 36089 ft):
///   Temperature decreases linearly with altitude.
///   ttheta = 1 - lapse_rate * alt
///   rsigma = ttheta ^ 4.255876
///
/// Layer 2 (lower stratosphere, 36089 to 65617 ft):
///   Isothermal (constant temperature).
///   ttheta = 0.751865 (constant)
///   rsigma = 0.297076 * exp(4.806e-5 * (36089 - alt))
///
/// Layer 3 (upper stratosphere, 65617+ ft):
///   Temperature increases with altitude.
///   ttheta = 0.682457 + alt / 945374
///   rsigma = (0.978261 + alt / 659515) ^ -35.16319
///
/// Returns pdelta = ttheta * rsigma (the pressure ratio P/P0).
inline double calcPressureRatio(double alt_ft,
                                double& ttheta,
                                double& rsigma) noexcept {
    if (alt_ft <= TROPO_ALT_FT) {
        // Troposphere: temperature decreases linearly
        ttheta = 1.0 - TROPO_LAPSE * alt_ft;
        rsigma = std::pow(ttheta, TROPO_RHO_EXP);
    } else if (alt_ft < TROPO_ALT2_FT) {
        // Lower stratosphere: isothermal
        ttheta = STRATO_TTHETA;
        rsigma = STRATO_RHO_BASE * std::exp(STRATO_RHO_K * (TROPO_ALT_FT - alt_ft));
    } else {
        // Upper stratosphere: temperature increases
        ttheta = 0.682457 + alt_ft / 945374.0;
        rsigma = std::pow(0.978261 + alt_ft / 659515.0, -35.16319);
    }
    return ttheta * rsigma;  // P/P0
}

/// Compute Mach number from KCAS (calibrated airspeed in knots).
///
/// This is the inverse of calcKcasFromMach. For subsonic Mach, the inverse
/// is closed-form. For supersonic Mach, a Newton-Raphson iteration on the
/// Rayleigh pitot formula is used (32 iterations max, converges in ~5).
///
///   kcas : calibrated airspeed in knots
///   pa   : ambient pressure in lb/ft^2
/// Returns Mach number (dimensionless). Returns 0 for kcas <= 0 or pa <= 0.
inline double calcMachFromKcas(double kcas, double pa) noexcept {
    if (kcas <= 0.0 || pa <= 0.0) return 0.0;

    // Step 1: compute the impact pressure ratio qc/pa from kcas.
    // qc/P0 = (1 + 0.2 * M_kcas^2)^3.5 - 1, where M_kcas = kcas / a_sl
    const double M_kcas = kcas / AASLK;
    const double qc = PASL * (std::pow(1.0 + 0.2 * M_kcas * M_kcas, 3.5) - 1.0);
    const double qcpa_val = qc / pa;

    // Step 2: try the subsonic inverse.
    // qcpa = (1 + 0.2*M^2)^3.5 - 1  =>  M^2 = 5 * ((1+qcpa)^(1/3.5) - 1)
    const double M2_sub = 5.0 * (std::pow(1.0 + qcpa_val, 1.0 / 3.5) - 1.0);
    if (M2_sub <= 1.0) {
        // Subsonic solution is valid
        return std::sqrt(std::max(0.0, M2_sub));
    }

    // Step 3: supersonic — iterate the Rayleigh formula.
    //   f(u)  = 166.921 * u^7 / (7*u^2 - 1)^2.5 - (1 + qc/pa)
    //   f'(u) = 7 * 166.921 * u^6 * (2*u^2 - 1) / (7*u^2 - 1)^3.5
    double u = std::sqrt(M2_sub);  // initial guess from subsonic formula (>1)
    if (u < 1.01) u = 1.01;
    for (int iter = 0; iter < 32; ++iter) {
        const double u2 = u * u;
        const double denom = 7.0 * u2 - 1.0;
        if (denom <= 0.0) { u = 1.05; continue; }
        const double fu  = 166.921 * std::pow(u, 7.0) / std::pow(denom, 2.5) - (1.0 + qcpa_val);
        const double fpu = 7.0 * 166.921 * std::pow(u, 6.0) * (2.0 * u2 - 1.0) / std::pow(denom, 3.5);
        if (std::fabs(fpu) < 1e-12) break;
        const double delta = fu / fpu;
        u -= delta;
        if (std::fabs(fu) < 0.001) break;
        if (u < 1.01) u = 1.01;
        if (u > 5.0)  u = 5.0;
    }
    return u;
}

/// Compute KCAS (calibrated airspeed in knots) from Mach number and
/// ambient pressure.
///
/// For subsonic Mach: qc = ((1 + 0.2*M^2)^3.5 - 1) * pa
/// For supersonic Mach: qc = (166.9 * M^7 / (7*M^2 - 1)^2.5 - 1) * pa
/// Then vcas = 1479.12 * sqrt((qc/P0 + 1)^0.285714 - 1), with a supersonic
/// correction when qc > 1889.64.
inline double calcKcasFromMach(double mach, double pa) noexcept {
    if (mach < 0.0) mach = 0.0;
    double qc;
    if (mach <= 1.0) {
        // Subsonic: impact pressure from Bernoulli
        qc = (std::pow(1.0 + 0.2 * mach * mach, 3.5) - 1.0) * pa;
    } else {
        // Supersonic: Rayleigh pitot formula
        qc = (166.9 * std::pow(mach, 7.0) / std::pow(7.0 * mach * mach - 1.0, 2.5) - 1.0) * pa;
    }
    const double qpasl1 = qc / PASL + 1.0;
    double vcas = 1479.12 * std::sqrt(std::pow(qpasl1, 0.285714) - 1.0);
    if (qc > 1889.64) {
        // Supersonic CAS correction
        const double oper = qpasl1 * std::pow(7.0 - (AASLK * AASLK) / (vcas * vcas), 2.5);
        vcas = 51.1987 * std::sqrt(oper);
    }
    return vcas;
}

/// Top-level atmosphere update.
///
/// Computes all atmosphere outputs from altitude, true airspeed, wing area,
/// and aircraft mass.
///
///   alt_ft     : altitude above sea level (feet, positive upward)
///   vt_ftps    : true airspeed (ft/s)
///   area_ft2   : wing reference area (ft^2)
///   mass_slugs : aircraft mass (slugs)
inline AtmosphereOutput computeAtmosphere(double alt_ft,
                                          double vt_ftps,
                                          double area_ft2,
                                          double mass_slugs) noexcept {
    AtmosphereOutput out;

    // Pressure, temperature, density ratios
    const double pdelta = calcPressureRatio(alt_ft, out.ttheta, out.rsigma);
    out.pdelta = pdelta;
    out.sound  = std::sqrt(out.ttheta) * AASL;   // a = sqrt(theta) * a0
    out.rho    = out.rsigma * RHOASL;             // rho = sigma * rho0
    out.pa     = pdelta * PASL;                    // P = delta * P0

    // Mach number and dynamic pressure
    //
    // The "safe_vt" floor (1.0 ft/s) protects the qovt = qbar/vt division
    // from divide-by-zero at zero airspeed. Mach and qbar themselves are
    // well-defined at vt=0 (Mach=0, qbar=0) and must use the ACTUAL vt,
    // not the floor — otherwise a stationary aircraft reports Mach ~= 1/sound
    // and qbar ~= 0.5*rho (a 1 ft/s breeze), which propagates into the FCS
    // as a phantom airload.
    const double safe_vt = (vt_ftps > 1.0) ? vt_ftps : 1.0;  // for qovt only
    out.mach = (vt_ftps > 0.0) ? (vt_ftps / out.sound) : 0.0;
    out.qbar = 0.5 * out.rho * vt_ftps * vt_ftps;             // q = 0.5 * rho * V^2
    out.qovt = out.qbar / safe_vt;

    // Normalized dynamic pressure: qsom = q * S / m
    // This is the acceleration per unit CL (CL * qsom has units of ft/s^2).
    const double safe_mass = (mass_slugs > 1e-6) ? mass_slugs : 1e-6;
    out.qsom = out.qbar * area_ft2 / safe_mass;

    // Calibrated airspeed
    out.vcas = calcKcasFromMach(out.mach, out.pa);

    return out;
}

}  // namespace f4::flight
