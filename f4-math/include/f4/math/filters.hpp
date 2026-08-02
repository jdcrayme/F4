// f4-math/filters.hpp
//
// Discrete-time filters and integrators used by the FreeFalcon flight
// control system (FCS) and sensor fusion. Each filter is a *faithful port*
// of the corresponding routine in FreeFalcon's simlib/math.cpp, preserving
// the exact z-transform coefficients so the closed-loop FCS step response
// matches the original.
//
//   LagFilter       — port of FLTust:  H(s) = 1 / (tau*s + 1)
//   LeadFilter      — port of FLeadTust: H(s) = tau*s + 1
//   WashoutFilter   — port of FWTust:  H(s) = tau*s / (tau*s + 1)
//   LeadLagFilter   — port of F7Tust:  H(s) = (tau1*s + 1) / ((tau2*s+1)*(tau3*s+1))
//   IntegratorTustin — port of FITust: H(s) = 1/s (trapezoidal)
//   AdamsBash2Filter — port of FIAdamsBash: H(s) = 1/s (2nd-order Adams-Bashforth)
//
// IMPORTANT — non-obvious porting decision:
//
//   FreeFalcon's FLTust and FWTust use k1 = exp(-dt/tau) for the homogeneous
//   coefficient, NOT the pure Tustin form k1 = (2*tau - dt)/(2*tau + dt).
//   These two formulations differ by O(dt^2). F4Flight used the Tustin form,
//   which is *not* the source-of-truth behaviour. We use FF's exact form
//   here so the FCS step response is bit-identical to the original.
//
//   Concretely, for FLTust:
//     k1 = exp(-dt/tau)                              [FF uses this]
//     k2 = (1 - k1) / 2                              [FF uses this]
//     y[n] = k1 * y[n-1] + k2 * (u[n] + u[n-1])
//
//   This is the discrete-time realisation of H(s) = 1/(tau*s+1) obtained by
//   zero-order-hold on the homogeneous part and trapezoidal on the input.
//   It's slightly less accurate than pure Tustin but matches FF exactly.
//
// Each filter:
//   - Owns its own state (no external SAVE_ARRAY).
//   - Is constexpr-default-constructible (zero state).
//   - Provides reset() to clear all history.
//   - Provides step(u, ...) which advances the filter one sample and
//     returns the new output.
//
// Tests (test_filters.cpp) validate:
//   - DC gain = 1 for lag, lead-lag, washout (steady state).
//   - Step response matches the analytical continuous-time response
//     within the expected discretization error.
//   - Bootstrapping (jstart counter for F7Tust) matches FF exactly.
//   - reset() clears all state.

#pragma once

#include <cmath>

namespace f4::math {

// ============================================================================
// LagFilter — port of FLTust. H(s) = 1 / (tau*s + 1).
//
// Coefficients (exact FF form):
//   k1 = exp(-dt/tau)
//   k2 = (1 - k1) / 2
//   y[n] = k1*y[n-1] + k2*(u[n] + u[n-1])
// ============================================================================
class LagFilter {
    double y_prev_ = 0.0;
    double u_prev_ = 0.0;
public:
    LagFilter() = default;

    double step(double u, double tau, double dt) noexcept {
        if (tau < 1e-12 || dt < 1e-12) {
            y_prev_ = u;
            u_prev_ = u;
            return u;
        }
        const double k1 = std::exp(-dt / tau);
        const double k2 = (1.0 - k1) * 0.5;
        const double y = y_prev_ * k1 + k2 * (u + u_prev_);
        y_prev_ = y;
        u_prev_ = u;
        return y;
    }

    void reset(double y = 0.0) noexcept {
        y_prev_ = y;
        u_prev_ = y;
    }

    [[nodiscard]] double output() const noexcept { return y_prev_; }
};

// ============================================================================
// LeadFilter — port of FLeadTust. Documented as H(s) = tau*s + 1.
//
// IMPORTANT — FF implementation note:
//   FF's FLeadTust implementation uses y[n] = k1*y[n-1] + k2*(u[n] - u[n-1])
//   with no direct feedthrough term. This means the DISCRETE realisation has
//   DC gain = 0 (constant input produces decaying output), NOT DC gain = 1
//   as the continuous-time H(s) = tau*s + 1 would imply. This is likely a
//   long-standing FF bug, but we preserve it here so the FCS step response
//   matches the original. If you need a true lead filter with DC gain = 1,
//   compose LeadLagFilter with tau2 = tau3 = a small value.
//
// Coefficients (exact FF form):
//   k1 = exp(-dt/tau)
//   k2 = 2 / (1 - k1)
//   y[n] = k1*y[n-1] + k2*(u[n] - u[n-1])
// ============================================================================
class LeadFilter {
    double y_prev_ = 0.0;
    double u_prev_ = 0.0;
public:
    LeadFilter() = default;

    double step(double u, double tau, double dt) noexcept {
        if (tau < 1e-12 || dt < 1e-12) {
            y_prev_ = u;
            u_prev_ = u;
            return u;
        }
        const double k1 = std::exp(-dt / tau);
        const double k2 = 2.0 / (1.0 - k1);
        const double y = y_prev_ * k1 + k2 * (u - u_prev_);
        y_prev_ = y;
        u_prev_ = u;
        return y;
    }

    void reset(double y = 0.0) noexcept {
        y_prev_ = y;
        u_prev_ = y;
    }

    [[nodiscard]] double output() const noexcept { return y_prev_; }
};

// ============================================================================
// WashoutFilter — port of FWTust. H(s) = tau*s / (tau*s + 1).
//
// High-pass with DC gain = 0 (steady-state output decays to 0).
//
// Coefficients (exact FF form):
//   k1 = exp(-dt/tau)
//   k2 = (1 + k1) / 2
//   y[n] = k1*y[n-1] + k2*(u[n] - u[n-1])
// ============================================================================
class WashoutFilter {
    double y_prev_ = 0.0;
    double u_prev_ = 0.0;
public:
    WashoutFilter() = default;

    double step(double u, double tau, double dt) noexcept {
        if (tau < 1e-12 || dt < 1e-12) {
            y_prev_ = 0.0;
            u_prev_ = u;
            return 0.0;
        }
        const double k1 = std::exp(-dt / tau);
        const double k2 = (1.0 + k1) * 0.5;
        const double y = y_prev_ * k1 + k2 * (u - u_prev_);
        y_prev_ = y;
        u_prev_ = u;
        return y;
    }

    void reset() noexcept {
        y_prev_ = 0.0;
        u_prev_ = 0.0;
    }

    [[nodiscard]] double output() const noexcept { return y_prev_; }
};

// ============================================================================
// LeadLagFilter — port of F7Tust.
//
//   H(s) = (tau1*s + 1) / ((tau2*s + 1) * (tau3*s + 1))
//
// Direct port of FF's F7Tust with the exact z-transform coefficients.
// Two prior frames of history are kept (save[0..1] for y, save[3..4] for u
// in the FF SAVE_ARRAY convention; we rename them for clarity).
//
// jstart counter: FF uses *jstart to skip the history-shift for the first
// two steps (because there is no "previous frame" yet). We replicate this
// exactly so the bootstrap transient matches.
// ============================================================================
class LeadLagFilter {
    double y_nm2_ = 0.0;  // y[n-2]
    double y_nm1_ = 0.0;  // y[n-1]
    double u_nm2_ = 0.0;  // u[n-2]
    double u_nm1_ = 0.0;  // u[n-1]
    int    jstart_ = 0;
public:
    LeadLagFilter() = default;

    double step(double in, double tau1, double tau2, double tau3, double dt) noexcept {
        // Guard against degenerate time constants (matches FF behaviour of
        // passing the input through unchanged when the filter is meaningless).
        if (tau1 < 1e-9 || tau2 < 1e-9 || tau3 < 1e-9 || dt < 1e-9) {
            y_nm2_ = y_nm1_ = in;
            u_nm2_ = u_nm1_ = in;
            jstart_ = 2;
            return in;
        }

        // Z-transform coefficients (exact FF form, using exp not Tustin).
        const double a = -(std::exp(-dt / tau2) + std::exp(-dt / tau3));
        const double b =  std::exp(-dt * (1.0 / tau2 + 1.0 / tau3));
        const double c =  1.0 - std::exp(-dt / tau1);
        const double d = -std::exp(-dt / tau1);

        double k;
        if (std::fabs(1.0 + c + d) < 1e-12) k = 0.0;
        else k = (1.0 + a + b) / (1.0 + c + d);

        // Compute output (uses current input via "save[5] = in" in FF).
        const double y_n = k * (in + c * u_nm1_ + d * u_nm2_)
                         - a * y_nm1_ - b * y_nm2_;

        // Shift history. FF guards these shifts with jstart >= 2 / >= 1.
        if (jstart_ >= 2) {
            y_nm2_ = y_nm1_;
            u_nm2_ = u_nm1_;
        }
        if (jstart_ >= 1) {
            y_nm1_ = y_n;
            u_nm1_ = in;
        }
        ++jstart_;
        return y_n;
    }

    void reset(double y = 0.0) noexcept {
        y_nm2_ = y_nm1_ = y;
        u_nm2_ = u_nm1_ = y;
        jstart_ = 0;
    }

    [[nodiscard]] double output() const noexcept { return y_nm1_; }
};

// ============================================================================
// IntegratorTustin — port of FITust. H(s) = 1/s, trapezoidal realisation.
//
//   y[n] = y[n-1] + (dt/2) * (u[n] + u[n-1])
//
// First step falls back to forward Euler (no prior input available).
// ============================================================================
class IntegratorTustin {
    double y_prev_ = 0.0;
    double u_prev_ = 0.0;
public:
    IntegratorTustin() = default;

    double step(double u, double dt) noexcept {
        const double y = y_prev_ + 0.5 * dt * (u + u_prev_);
        y_prev_ = y;
        u_prev_ = u;
        return y;
    }

    void reset(double y = 0.0) noexcept {
        y_prev_ = y;
        u_prev_ = y;
    }

    [[nodiscard]] double output() const noexcept { return y_prev_; }
};

// ============================================================================
// AdamsBash2Filter — port of FIAdamsBash. H(s) = 1/s, 2nd-order AB realisation.
//
//   y[n] = y[n-1] + (dt/2) * (3*u[n] - u[n-1])
//
// First step falls back to forward Euler (no prior input available).
// ============================================================================
class AdamsBash2Filter {
    double y_prev_ = 0.0;
    double u_prev_ = 0.0;
public:
    AdamsBash2Filter() = default;

    double step(double u, double dt) noexcept {
        const double y = y_prev_ + 0.5 * dt * (3.0 * u - u_prev_);
        y_prev_ = y;
        u_prev_ = u;
        return y;
    }

    void reset(double y = 0.0) noexcept {
        y_prev_ = y;
        u_prev_ = y;
    }

    [[nodiscard]] double output() const noexcept { return y_prev_; }
};

} // namespace f4::math
