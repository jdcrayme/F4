// f4-math/eigen_real.hpp
//
// Dense real nonsymmetric eigenvalue solver (values + eigenvectors).
//
// Purpose: pole-based flight-control diagnosis. The flight model is a
// deterministic fixed-step discrete map x[k+1] = F(x[k], u[k]); its local
// stability is the eigenspectrum of the Jacobian dF/dx at trim. The tooling
// (f4-flight-model/tests/diag_poles.cpp) builds that Jacobian by finite
// differences and calls this solver. No third-party dependency (Eigen etc.)
// is pulled in — f4-math stays self-contained.
//
// Algorithm (classic, textbook):
//   1. Balance (Parlett-Reinsch diagonal scaling) — improves accuracy.
//   2. Householder reduction to upper Hessenberg form.
//   3. Shifted QR iteration in COMPLEX arithmetic (Wilkinson shift,
//      exceptional shifts every 10 stalled iterations, standard deflation)
//      to read the eigenvalues off the triangular diagonal. Complex
//      arithmetic is used instead of the real Francis double-shift bulge
//      chase: it is materially simpler to implement correctly, and the
//      matrices here are small (n < ~64) so the ~4x operation cost is
//      irrelevant.
//   4. Eigenvectors by inverse iteration ((A - lambda I) w = v, complex LU
//      with partial pivoting, 2 refinement passes) on the ORIGINAL matrix.
//
// Limitations (documented, acceptable for diagnosis):
//   - Repeated / clustered eigenvalues: inverse iteration may return nearly
//     identical eigenvectors for a defective or tight cluster. The pole
//     tool reports residuals so degenerate cases are visible.
//   - Eigenvectors are not orthogonalized.
//
// Tests: f4-math/tests/test_eigen.cpp validates against known spectra and
// checks the residual ||A v - lambda v|| for every returned pair.

#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif // !M_PI

namespace f4::math {

// ============================================================================
// Result: eigenvalues and (parallel-indexed) eigenvectors.
//   values[i]                : i-th eigenvalue.
//   vectors[i][j]            : j-th component of the i-th eigenvector
//                              (unit 2-norm).
// For a real matrix, complex eigenvalues come in conjugate pairs and the
// vectors follow: vector[i] == conj(vector[j]) when values[i] == conj(values[j]).
// ============================================================================
struct EigenResult {
    std::vector<std::complex<double>> values;
    std::vector<std::vector<std::complex<double>>> vectors;
};

namespace detail_eigen {

using cdouble = std::complex<double>;

// ---------------------------------------------------------------------------
// Balance: Parlett-Reinsch diagonal similarity scaling. Reduces the norm
// imbalance between rows/columns which otherwise costs QR accuracy.
// RADIX 2; the iteration is the classic algorithm (Parlett & Reinsch 1969).
// ---------------------------------------------------------------------------
inline void balance(std::vector<double>& A, std::size_t n) noexcept {
    const double RADIX = 2.0;
    const double sqrdx = RADIX * RADIX;
    bool done = false;
    while (!done) {
        done = true;
        for (std::size_t i = 0; i < n; ++i) {
            double r = 0.0, c = 0.0;
            for (std::size_t j = 0; j < n; ++j) {
                if (j != i) {
                    r += std::fabs(A[i * n + j]);
                    c += std::fabs(A[j * n + i]);
                }
            }
            if (c == 0.0 || r == 0.0) continue;
            double g = r / RADIX;
            double f = 1.0;
            double s = c + r;
            while (c < g) { f *= RADIX; c *= sqrdx; }
            g = r * RADIX;
            while (c > g) { f /= RADIX; c /= sqrdx; }
            if ((c + r) / f < 0.95 * s) {
                done = false;
                g = 1.0 / f;
                for (std::size_t j = 0; j < n; ++j) A[i * n + j] *= g;
                for (std::size_t j = 0; j < n; ++j) A[j * n + i] *= f;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Householder reduction to upper Hessenberg form (in place).
// Golub & Van Loan alg 7.4.2-style reflectors, with pivoting for stability.
// No accumulation needed: eigenvectors are later recovered by inverse
// iteration on the ORIGINAL matrix.
// ---------------------------------------------------------------------------
inline void hessenberg(std::vector<double>& A, std::size_t n) noexcept {
    for (std::size_t k = 0; k + 2 < n; ++k) {
        // Column k, rows k+1..n-1
        double normx = 0.0;
        for (std::size_t i = k + 1; i < n; ++i) normx += A[i * n + k] * A[i * n + k];
        normx = std::sqrt(normx);
        if (normx == 0.0) continue;
        // Pivot the largest-magnitude row to k+1 for a stable reflector
        std::size_t imax = k + 1;
        double amax = std::fabs(A[(k + 1) * n + k]);
        for (std::size_t i = k + 2; i < n; ++i) {
            const double a = std::fabs(A[i * n + k]);
            if (a > amax) { amax = a; imax = i; }
        }
        if (imax != k + 1) {
            for (std::size_t j = 0; j < n; ++j)
                std::swap(A[(k + 1) * n + j], A[imax * n + j]);
            for (std::size_t j = 0; j < n; ++j)
                std::swap(A[j * n + (k + 1)], A[j * n + imax]);
        }
        const double x0 = A[(k + 1) * n + k];
        const double alpha = x0 + ((x0 >= 0.0) ? 1.0 : -1.0) * normx;
        std::vector<double> v(n, 0.0);
        v[k + 1] = alpha;
        for (std::size_t i = k + 2; i < n; ++i) v[i] = A[i * n + k];
        double vnorm2 = 0.0;
        for (std::size_t i = k + 1; i < n; ++i) vnorm2 += v[i] * v[i];
        if (vnorm2 == 0.0) continue;
        // P = I - 2 v v^T / vnorm2, applied left and right
        for (std::size_t j = 0; j < n; ++j) {           // left: A := P A
            double dot = 0.0;
            for (std::size_t i = k + 1; i < n; ++i) dot += v[i] * A[i * n + j];
            const double f = 2.0 * dot / vnorm2;
            for (std::size_t i = k + 1; i < n; ++i) A[i * n + j] -= f * v[i];
        }
        for (std::size_t i = 0; i < n; ++i) {           // right: A := A P
            double dot = 0.0;
            for (std::size_t j = k + 1; j < n; ++j) dot += A[i * n + j] * v[j];
            const double f = 2.0 * dot / vnorm2;
            for (std::size_t j = k + 1; j < n; ++j) A[i * n + j] -= f * v[j];
        }
        // Clean below-subdiagonal entries in column k
        for (std::size_t i = k + 2; i < n; ++i) A[i * n + k] = 0.0;
    }
}

// ---------------------------------------------------------------------------
// Complex LU with partial pivoting. Returns false on exact singularity
// (pivot below tiny) — caller perturbs the shift and retries.
// ---------------------------------------------------------------------------
inline bool lu_factor_complex(std::vector<cdouble>& M, std::size_t n,
                              std::vector<std::size_t>& piv) noexcept {
    piv.resize(n);
    for (std::size_t i = 0; i < n; ++i) piv[i] = i;
    for (std::size_t k = 0; k < n; ++k) {
        std::size_t p = k;
        double best = std::abs(M[k * n + k]);
        for (std::size_t i = k + 1; i < n; ++i) {
            const double m = std::abs(M[i * n + k]);
            if (m > best) { best = m; p = i; }
        }
        if (best < 1e-300) return false;
        if (p != k) {
            for (std::size_t j = 0; j < n; ++j) std::swap(M[k * n + j], M[p * n + j]);
            std::swap(piv[k], piv[p]);
        }
        const cdouble inv = 1.0 / M[k * n + k];
        for (std::size_t i = k + 1; i < n; ++i) {
            const cdouble f = M[i * n + k] * inv;
            M[i * n + k] = f;
            if (f != cdouble(0.0, 0.0)) {
                for (std::size_t j = k + 1; j < n; ++j) M[i * n + j] -= f * M[k * n + j];
            }
        }
    }
    return true;
}

inline void lu_solve_complex(const std::vector<cdouble>& LU, std::size_t n,
                             const std::vector<std::size_t>& piv,
                             std::vector<cdouble>& b) noexcept {
    std::vector<cdouble> x(n);
    for (std::size_t i = 0; i < n; ++i) x[i] = b[piv[i]];
    for (std::size_t i = 1; i < n; ++i) {              // forward (L)
        cdouble s = x[i];
        for (std::size_t j = 0; j < i; ++j) s -= LU[i * n + j] * x[j];
        x[i] = s;
    }
    for (std::size_t i = n; i-- > 0;) {                // back (U)
        cdouble s = x[i];
        for (std::size_t j = i + 1; j < n; ++j) s -= LU[i * n + j] * x[j];
        x[i] = s / LU[i * n + i];
    }
    b = x;
}

// ---------------------------------------------------------------------------
// Inverse iteration on the ORIGINAL (row-major real) matrix for eigenvalue
// lam. Returns a unit eigenvector (complex). Deterministic start vector.
// ---------------------------------------------------------------------------
inline std::vector<cdouble> inverse_iteration(const std::vector<double>& A,
                                              std::size_t n, cdouble lam) {
    for (int attempt = 0; attempt < 4; ++attempt) {
        // M = A - lam I, nudged by a tiny imaginary spiral if near-singular
        std::vector<cdouble> M(n * n);
        for (std::size_t i = 0; i < n; ++i)
            for (std::size_t j = 0; j < n; ++j)
                M[i * n + j] = cdouble(A[i * n + j], 0.0);
        const double nudge = 1e-13 * std::max(1.0, std::abs(lam)) * std::pow(10.0, attempt);
        for (std::size_t i = 0; i < n; ++i)
            M[i * n + i] -= lam + cdouble(0.0, nudge);

        std::vector<std::size_t> piv;
        std::vector<cdouble> LU = M;
        if (!lu_factor_complex(LU, n, piv)) continue;

        // Deterministic start: quasi-random unit vector
        std::vector<cdouble> v(n);
        double s = 0.6180339887498949 + 0.13 * attempt;
        double nrm = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            v[i] = cdouble(s, 0.0037 * double((i * 13) % 17));
            s = s * s + 0.17;
            nrm += std::norm(v[i]);
        }
        for (auto& c : v) c /= std::sqrt(nrm);

        // Two inverse-iteration passes
        for (int pass = 0; pass < 2; ++pass) {
            lu_solve_complex(LU, n, piv, v);
            double w = 0.0;
            for (const auto& c : v) w += std::norm(c);
            if (w == 0.0 || !std::isfinite(w)) break;
            const double inv = 1.0 / std::sqrt(w);
            for (auto& c : v) c *= inv;
        }
        bool ok = true;
        for (const auto& c : v) {
            if (!std::isfinite(c.real()) || !std::isfinite(c.imag())) { ok = false; break; }
        }
        if (ok) return v;
    }
    // Fallback: unit basis vector (the tool's residual report will flag it)
    std::vector<cdouble> v(n, cdouble(0.0, 0.0));
    v[0] = cdouble(1.0, 0.0);
    return v;
}

} // namespace detail_eigen

// ============================================================================
// eig_real_general — full eigendecomposition of a real n x n matrix.
//
//   A : row-major n*n doubles. All entries must be finite.
// Throws std::invalid_argument on non-finite input; std::runtime_error if
// the QR iteration fails to converge (effectively never for balanced
// Hessenberg matrices).
//
// Ordering: eigenvalues sorted by real part ASCENDING (most unstable first),
// tie-break |imag| descending — the diagnosis-relevant modes surface first.
// Complex conjugate pairs remain adjacent (equal real part, equal |imag|).
// ============================================================================
inline EigenResult eig_real_general(const std::vector<double>& A_in, std::size_t n) {
    if (n == 0) return {};
    if (A_in.size() != n * n) throw std::invalid_argument("eig_real_general: A must be n*n");
    for (const double a : A_in) {
        if (!std::isfinite(a)) throw std::invalid_argument("eig_real_general: non-finite entry");
    }

    using cdouble = std::complex<double>;
    std::vector<double> A = A_in;
    detail_eigen::balance(A, n);
    detail_eigen::hessenberg(A, n);

    // --- Shifted complex QR on the Hessenberg matrix ---
    std::vector<cdouble> H(n * n);
    for (std::size_t i = 0; i < n * n; ++i) H[i] = cdouble(A[i], 0.0);

    std::vector<cdouble> evals;
    evals.reserve(n);
    const double eps = 1e-14;

    std::size_t top = 0;          // active block is [top .. bot] (inclusive)
    std::size_t bot = n - 1;
    std::size_t stall = 0;
    const std::size_t kMaxTotal = 200 * n + 400;
    std::size_t total_iters = 0;

    while (bot != (std::size_t)-1 && bot >= top) {
        if (++total_iters > kMaxTotal)
            throw std::runtime_error("eig_real_general: QR failed to converge");

        // Deflation scan: smallest l in (top, bot] with negligible subdiagonal
        std::size_t l = top;
        for (std::size_t i = bot; i > top; --i) {
            const double s = std::abs(H[(i - 1) * n + (i - 1)]) + std::abs(H[i * n + i]);
            const double thresh = (s == 0.0) ? eps : eps * s;
            if (std::abs(H[i * n + (i - 1)]) <= thresh) {
                H[i * n + (i - 1)] = cdouble(0.0, 0.0);
                l = i;
                break;
            }
        }

        if (l == bot) {                                   // 1x1 deflated
            evals.push_back(H[bot * n + bot]);
            if (bot == top) break;
            --bot;
            stall = 0;
            continue;
        }
        if (l == bot - 1) {                               // 2x2 block deflated
            const cdouble a = H[(bot - 1) * n + (bot - 1)], b = H[(bot - 1) * n + bot];
            const cdouble c = H[bot * n + (bot - 1)],     d = H[bot * n + bot];
            const cdouble tr = a + d;
            const cdouble disc = std::sqrt(tr * tr - 4.0 * (a * d - b * c));
            evals.push_back((tr + disc) * 0.5);
            evals.push_back((tr - disc) * 0.5);
            if (bot - 1 == top) break;
            bot -= 2;
            stall = 0;
            continue;
        }

        // --- Wilkinson shift from the trailing 2x2 of the active block ---
        cdouble mu;
        if (stall > 0 && stall % 10 == 0) {
            const double kick = std::abs(H[bot * n + (bot - 1)])
                              + std::abs(H[(bot - 1) * n + (bot - 2)]);
            mu = H[bot * n + bot] + cdouble(kick, 0.0);
        } else {
            const cdouble a = H[(bot - 1) * n + (bot - 1)], b = H[(bot - 1) * n + bot];
            const cdouble c = H[bot * n + (bot - 1)],     d = H[bot * n + bot];
            const cdouble tr = a + d;
            const cdouble disc = std::sqrt(tr * tr - 4.0 * (a * d - b * c));
            const cdouble m1 = (tr + disc) * 0.5, m2 = (tr - disc) * 0.5;
            mu = (std::abs(m1 - d) < std::abs(m2 - d)) ? m1 : m2;
        }

        // --- Explicit single-shift QR step on H[top..bot] ---
        // M = H - mu*I;  M = Q R (complex Givens);  H := R Q + mu*I.
        // (Explicit form of the standard iteration — simple and correct;
        // the bulge-chase optimization is unnecessary at these sizes.)
        for (std::size_t i = top; i <= bot; ++i) H[i * n + i] -= mu;

        std::vector<cdouble> cs(bot - top, cdouble(0.0)), sn(bot - top, cdouble(0.0));
        for (std::size_t i = top; i < bot; ++i) {
            // Zero H[(i+1)*n + i] against H[i*n + i]
            const cdouble x = H[i * n + i];
            const cdouble y = H[(i + 1) * n + i];
            const double r = std::sqrt(std::norm(x) + std::norm(y));
            cdouble c(1.0, 0.0), s(0.0, 0.0);
            if (r != 0.0) {
                c = x / r;
                s = y / r;
                // G = [[conj(c), conj(s)], [-s, c]] applied to rows i, i+1.
                // Column range [i .. n-1]: rows i/i+1 have nonzeros in the
                // upper-triangular part RIGHT of the active block (columns
                // > bot) whenever the block has already been shrunk by
                // deflation — skipping them breaks the similarity transform.
                // (Column i-1 of rows i/i+1 is exactly zero: zeroed by the
                // previous rotation, or by the deflation that created `top`.)
                for (std::size_t j = i; j < n; ++j) {
                    const cdouble h1 = H[i * n + j], h2 = H[(i + 1) * n + j];
                    H[i * n + j]       = std::conj(c) * h1 + std::conj(s) * h2;
                    H[(i + 1) * n + j] = -s * h1 + c * h2;
                }
                H[(i + 1) * n + i] = cdouble(0.0, 0.0);  // exact zero by construction
            }
            cs[i - top] = c;
            sn[i - top] = s;
        }
        // H := R Q : right-multiply by G^* for each stored rotation (columns i, i+1).
        // Row range [0 .. min(n-1, i+2)]: rows ABOVE the active block carry
        // upper-triangular entries in these columns and must be rotated too;
        // rows below i+2 are exactly zero in columns i, i+1 (Hessenberg + the
        // (i+2, i+1) fill the rotation itself creates).
        for (std::size_t i = top; i < bot; ++i) {
            const cdouble c = cs[i - top], s = sn[i - top];
            const std::size_t rmax = std::min(n - 1, i + 2);
            for (std::size_t r2 = 0; r2 <= rmax; ++r2) {
                const cdouble h1 = H[r2 * n + i], h2 = H[r2 * n + (i + 1)];
                H[r2 * n + i]       = c * h1 + s * h2;
                H[r2 * n + (i + 1)] = -std::conj(s) * h1 + std::conj(c) * h2;
            }
        }
        for (std::size_t i = top; i <= bot; ++i) H[i * n + i] += mu;

        ++stall;
    }

    // --- Sort: most unstable first, oscillatory tie-break ---
    EigenResult out;
    out.values = std::move(evals);
    for (auto& v : out.values) {
        if (std::abs(v.imag()) < 1e-13 * std::max(1.0, std::abs(v.real()))) v.imag(0.0);
    }
    std::vector<std::size_t> order(out.values.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        const cdouble& va = out.values[a];
        const cdouble& vb = out.values[b];
        if (va.real() != vb.real()) return va.real() < vb.real();
        return std::abs(va.imag()) > std::abs(vb.imag());
    });
    std::vector<cdouble> sorted_vals(out.values.size());
    for (std::size_t i = 0; i < order.size(); ++i) sorted_vals[i] = out.values[order[i]];
    out.values = std::move(sorted_vals);

    // --- Eigenvectors: inverse iteration on the ORIGINAL matrix ---
    out.vectors.assign(out.values.size(), {});
    std::vector<char> done(out.values.size(), 0);
    for (std::size_t i = 0; i < out.values.size(); ++i) {
        if (done[i]) continue;
        const cdouble lam = out.values[i];
        auto vec = detail_eigen::inverse_iteration(A_in, n, lam);
        out.vectors[i] = vec;
        done[i] = 1;
        // Conjugate partner shares the conjugated vector (exact for real A)
        for (std::size_t j = i + 1; j < out.values.size(); ++j) {
            if (!done[j] && out.values[j] == std::conj(lam)) {
                std::vector<cdouble> cv(vec.size());
                for (std::size_t k = 0; k < vec.size(); ++k) cv[k] = std::conj(vec[k]);
                out.vectors[j] = std::move(cv);
                done[j] = 1;
                break;
            }
        }
    }
    return out;
}

// ============================================================================
// Discrete-to-continuous eigenvalue mapping and modal metrics.
//
// The flight model is the discrete map x[k+1] = F(x[k]) at major-frame dt.
// The equivalent continuous-time pole is lambda_c = ln(lambda_d) / dt
// (principal branch — valid while the oscillation period > 2*dt, always
// true for the modes of interest here).
// ============================================================================
struct ModalInfo {
    std::complex<double> lambda_c;   // continuous-time pole [1/s]
    double zeta;                     // damping ratio = -Re/|lambda|
    double omega_n;                  // natural frequency [rad/s]
    double period_s;                 // oscillation period (inf if non-oscillatory)
    double time_to_double_s;         // ln(2)/Re: positive = doubling (unstable),
                                     // negative = halving (stable), inf if Re==0
};

inline ModalInfo modal_info(std::complex<double> lambda_d, double dt) {
    ModalInfo mi;
    const std::complex<double> lnc = std::log(lambda_d);
    mi.lambda_c = lnc / dt;
    const double re = mi.lambda_c.real();
    const double im = mi.lambda_c.imag();
    mi.omega_n = std::sqrt(re * re + im * im);
    mi.zeta = (mi.omega_n > 0.0) ? -re / mi.omega_n : 0.0;
    mi.period_s = (im != 0.0) ? (2.0 * M_PI / std::fabs(im))
                              : std::numeric_limits<double>::infinity();
    mi.time_to_double_s = (re != 0.0) ? std::log(2.0) / re
                                      : std::numeric_limits<double>::infinity();
    return mi;
}

} // namespace f4::math
