// f4-math/solver.hpp
//
// 1-D root finder: Newton-Raphson with bracketing fallback.
//
// The flight model uses this for the trim solver (find alpha and throttle
// such that Nz = 1g and q = 0 at a given altitude/speed). Newton-Raphson
// converges fast when the derivative is good, but it can diverge if the
// initial guess is poor or the function has near-zero derivative. We add
// a bracket check to bail out gracefully when Newton steps outside the
// bracket, falling back to bisection.

#pragma once

#include <cmath>
#include <concepts>
#include <functional>
#include <optional>
#include <stdexcept>
#include <utility>

namespace f4::math {

// ============================================================================
// Result of a root-finding attempt.
// ============================================================================
struct SolverResult {
    double root;            // The computed root (only valid if converged).
    bool    converged;      // True if |f(root)| < tol within max_iter.
    int     iterations;     // Number of iterations actually performed.
    double  final_value;    // f(root) at the end (small if converged).
};

// ============================================================================
// Newton-Raphson with bisection fallback.
//
// Parameters:
//   f         : function whose root we seek.
//   df        : derivative of f.
//   x0        : initial guess.
//   tol       : stop when |f(x)| < tol.
//   max_iter  : max Newton steps before giving up.
//
// If the Newton step would land outside [lo, hi] (when provided), or if
// the derivative is near zero, fall back to a bisection step using the
// same bracket. The bracket also bounds the final answer.
//
// Returns SolverResult with converged=true on success, false on failure
// (caller decides whether to throw or accept the partial result).
// ============================================================================
inline SolverResult newton_raphson(
    const std::function<double(double)>& f,
    const std::function<double(double)>& df,
    double x0,
    double tol = 1e-9,
    int max_iter = 50,
    std::optional<std::pair<double, double>> bracket = std::nullopt
) {
    double x = x0;
    // If bracket provided, ensure f changes sign across it; if not, the
    // caller has given us a bad bracket and we cannot bisect.
    double lo = 0.0, hi = 0.0;
    bool have_bracket = false;
    if (bracket) {
        lo = bracket->first;
        hi = bracket->second;
        if (lo > hi) std::swap(lo, hi);
        have_bracket = true;
        // Clamp x0 into bracket.
        if (x < lo) x = lo;
        if (x > hi) x = hi;
    }

    for (int i = 0; i < max_iter; ++i) {
        double fx = f(x);
        if (std::abs(fx) < tol) {
            return {x, true, i, fx};
        }

        double dfx = df(x);

        double x_next;
        if (std::abs(dfx) < 1e-14) {
            // Derivative too small — fall back to bisection if possible.
            if (!have_bracket) {
                return {x, false, i, fx};
            }
            x_next = 0.5 * (lo + hi);
        } else {
            x_next = x - fx / dfx;
            // If bracketed and the Newton step went outside, bisect instead.
            if (have_bracket && (x_next < lo || x_next > hi)) {
                x_next = 0.5 * (lo + hi);
            }
        }

        // Update bracket if we have one (maintain sign change across [lo,hi]).
        if (have_bracket) {
            double f_next = f(x_next);
            double f_lo = f(lo);
            if (f_lo * f_next < 0.0) {
                hi = x_next;
            } else {
                lo = x_next;
            }
        }

        x = x_next;
    }

    return {x, false, max_iter, f(x)};
}

// ============================================================================
// Pure bisection — useful when the derivative is unreliable or expensive.
// Requires a sign-changing bracket [lo, hi].
// ============================================================================
inline SolverResult bisection(
    const std::function<double(double)>& f,
    double lo, double hi,
    double tol = 1e-9,
    int max_iter = 100
) {
    double f_lo = f(lo);
    double f_hi = f(hi);
    if (f_lo * f_hi > 0.0) {
        throw std::invalid_argument("bisection: f(lo) and f(hi) must have opposite signs");
    }

    for (int i = 0; i < max_iter; ++i) {
        double mid = 0.5 * (lo + hi);
        double f_mid = f(mid);
        if (std::abs(f_mid) < tol || (hi - lo) < tol) {
            return {mid, true, i + 1, f_mid};
        }
        if (f_lo * f_mid < 0.0) {
            hi = mid;
            f_hi = f_mid;
        } else {
            lo = mid;
            f_lo = f_mid;
        }
    }
    return {0.5 * (lo + hi), false, max_iter, f(0.5 * (lo + hi))};
}

} // namespace f4::math
