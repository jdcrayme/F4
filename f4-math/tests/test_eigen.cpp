// f4-math/test_eigen.cpp
//
// Validation for the real nonsymmetric eigensolver (eigen_real.hpp).
//
// Strategy: every test constructs a matrix with a KNOWN spectrum (or a
// known invariant) and asserts
//   1. the computed eigenvalues match the known set, and
//   2. the eigenpair residual ||A v - lambda v||_2 / ||A||_F is tiny.
// Residual checking is the load-bearing assertion: it validates values and
// vectors jointly without depending on solver-internal ordering.

#include "f4/math/eigen_real.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <complex>
#include <vector>

namespace {

using cdouble = std::complex<double>;
using f4::math::EigenResult;
using f4::math::eig_real_general;
using f4::math::modal_info;

constexpr double kTolValue = 1e-8;   // eigenvalue match vs known spectrum
constexpr double kTolResid = 1e-6;   // eigenpair residual, relative to ||A||_F

double frobNorm(const std::vector<double>& A) {
    double s = 0.0;
    for (const double a : A) s += a * a;
    return std::sqrt(s);
}

// ||A v - lambda v||_2 / (||A||_F)  — v is unit-norm
double eigenpair_residual(const std::vector<double>& A, std::size_t n,
                          cdouble lambda, const std::vector<cdouble>& v) {
    double resid = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        cdouble s(0.0, 0.0);
        for (std::size_t j = 0; j < n; ++j) s += A[i * n + j] * v[j];
        s -= lambda * v[i];
        resid += std::norm(s);
    }
    return std::sqrt(resid) / std::max(1e-300, frobNorm(A));
}

// Match each expected eigenvalue to the nearest computed one.
void expect_spectrum(const std::vector<double>& A, std::size_t n,
                     const std::vector<cdouble>& expected, double tol = kTolValue) {
    const EigenResult r = eig_real_general(A, n);
    ASSERT_EQ(r.values.size(), expected.size());
    ASSERT_EQ(r.vectors.size(), expected.size());
    for (const cdouble& e : expected) {
        double best = std::numeric_limits<double>::infinity();
        for (const cdouble& g : r.values) {
            best = std::min(best, std::abs(g - e));
        }
        EXPECT_LT(best, tol) << "no computed eigenvalue near (" << e.real()
                             << ", " << e.imag() << "); best distance " << best;
    }
    // Residuals for all returned pairs
    const double normA = frobNorm(A);
    for (std::size_t i = 0; i < r.values.size(); ++i) {
        double vn = 0.0;
        for (const auto& c : r.vectors[i]) vn += std::norm(c);
        EXPECT_NEAR(vn, 1.0, 1e-9) << "eigenvector " << i << " not unit norm";
        const double resid = eigenpair_residual(A, n, r.values[i], r.vectors[i]);
        EXPECT_LT(resid, kTolResid)
            << "eigenpair " << i << " residual " << resid
            << " at lambda = (" << r.values[i].real() << ", "
            << r.values[i].imag() << ")";
    }
    (void)normA;
}

// ---------------------------------------------------------------------------
// 1. Diagonal matrix — eigenvalues are the diagonal entries.
// ---------------------------------------------------------------------------
TEST(EigenReal, DiagonalMatrix) {
    const std::size_t n = 4;
    std::vector<double> A(n * n, 0.0);
    A[0 * n + 0] = -0.05;   // slow divergent-ish
    A[1 * n + 1] = -1.0;
    A[2 * n + 2] = -2.0;
    A[3 * n + 3] = -3.0;
    expect_spectrum(A, n, {{-0.05, 0}, {-1.0, 0}, {-2.0, 0}, {-3.0, 0}});
}

// ---------------------------------------------------------------------------
// 2. 2x2 oscillatory block with known zeta / omega_n (the plan-appendix
//    speed-mode matrix shape): A = [[a, -g], [2g/V^2, 0]].
//    lambda^2 - a*lambda + 2g^2/V^2 = 0, zeta = -a*V/(2*sqrt(2)*g).
// ---------------------------------------------------------------------------
TEST(EigenReal, SpeedModeBlockKnownZeta) {
    const double g = 32.2, V = 400.0;
    const double a = 0.15;  // a_V > 0 = back side => unstable per plan §9.2
    std::vector<double> A = {a, -g, 2.0 * g / (V * V), 0.0};
    const double wn = g * std::sqrt(2.0) / V;
    const double zeta = -a * V / (2.0 * std::sqrt(2.0) * g);
    const double re = -zeta * wn;
    const double im = wn * std::sqrt(1.0 - zeta * zeta);
    expect_spectrum(A, 2, {{re, im}, {re, -im}});

    // modal_info sanity on a synthetic DISCRETE eigenvalue (this matrix is a
    // continuous-time model; the discrete->continuous conversion is exercised
    // separately in ModalInfoRoundTrip).
    const EigenResult r = eig_real_general(A, 2);
    const cdouble lam = r.values[0];
    EXPECT_NEAR(lam.real(), re, 1e-9);
    EXPECT_NEAR(std::abs(lam.imag()), im, 1e-9);
    EXPECT_GT(lam.real(), 0.0) << "back-side mode must be unstable";
}

// ---------------------------------------------------------------------------
// 3. Companion matrix with known roots: (lambda+1)(lambda-2)(lambda+3-4i)
//    (lambda+3+4i) = lambda^4 + 5*lambda^3 + 17*lambda^2 - 37*lambda - 50.
//    Companion (row-major, top row holds -a_k):
//      [[-5, -17, 37, 50], [1,0,0,0], [0,1,0,0], [0,0,1,0]]
// ---------------------------------------------------------------------------
TEST(EigenReal, CompanionKnownRoots) {
    const std::size_t n = 4;
    std::vector<double> A(n * n, 0.0);
    A[0 * n + 0] = -5.0; A[0 * n + 1] = -17.0; A[0 * n + 2] = 37.0; A[0 * n + 3] = 50.0;
    A[1 * n + 0] = 1.0;
    A[2 * n + 1] = 1.0;
    A[3 * n + 2] = 1.0;
    expect_spectrum(A, n, {{-1.0, 0}, {2.0, 0}, {-3.0, 4.0}, {-3.0, -4.0}});
}

// ---------------------------------------------------------------------------
// 4. Rotation-scaling 2x2 (defective-free complex pair, real matrix):
//    [[s, -w], [w, s]] -> lambda = s +/- i w.
// ---------------------------------------------------------------------------
TEST(EigenReal, RotationScalingBlock) {
    const double s = 0.3, w = 2.5;
    std::vector<double> A = {s, -w, w, s};
    expect_spectrum(A, 2, {{s, w}, {s, -w}});
}

// ---------------------------------------------------------------------------
// 5. Nearly-defective cluster — solver must at least return values close to
//    the true repeated root and not produce garbage vectors (residual check).
//    Jordan-ish block perturbed by 1e-9.
// ---------------------------------------------------------------------------
TEST(EigenReal, NearRepeatedRoot) {
    const std::size_t n = 3;
    std::vector<double> A(n * n, 0.0);
    A[0 * n + 0] = -0.5;
    A[0 * n + 1] = 1.0;
    A[1 * n + 0] = -1e-9;
    A[1 * n + 1] = -0.5;
    A[2 * n + 2] = -7.0;
    // True eigenvalues: -0.5 (+- sqrt(-1e-9)/... ~ -0.5 +/- 3.16e-5 i), -7
    const EigenResult r = eig_real_general(A, n);
    ASSERT_EQ(r.values.size(), n);
    int near_half = 0;
    for (const auto& v : r.values) {
        if (std::abs(v - cdouble(-0.5, 0.0)) < 1e-3) ++near_half;
    }
    EXPECT_EQ(near_half, 2) << "expected a tight pair near -0.5";
    for (std::size_t i = 0; i < n; ++i) {
        const double resid = eigenpair_residual(A, n, r.values[i], r.vectors[i]);
        EXPECT_LT(resid, 1e-4) << "cluster eigenpair " << i << " residual " << resid;
    }
}

// ---------------------------------------------------------------------------
// 6. Badly-scaled UPPER TRIANGULAR matrix — eigenvalues are exactly the
//    diagonal entries regardless of the 1e3-spread in the upper part, and
//    the row/col norm imbalance stresses the balancer.
// ---------------------------------------------------------------------------
TEST(EigenReal, BadlyScaledMatrix) {
    const std::size_t n = 4;
    std::vector<double> A(n * n, 0.0);
    A[0 * n + 0] = -1000.0;
    A[0 * n + 1] = 500.0;
    A[1 * n + 1] = -1.0;
    A[1 * n + 2] = 0.5;
    A[2 * n + 2] = -0.1;
    A[2 * n + 3] = 0.05;
    A[3 * n + 3] = -0.01;
    expect_spectrum(A, n,
                    {{-1000.0, 0}, {-1.0, 0}, {-0.1, 0}, {-0.01, 0}}, 1e-6);
}

// ---------------------------------------------------------------------------
// 7. modal_info: discrete->continuous conversion round trip.
//    lambda_d = exp(lambda_c * dt) must invert exactly.
// ---------------------------------------------------------------------------
TEST(EigenReal, ModalInfoRoundTrip) {
    const double dt = 1.0 / 60.0;
    const cdouble lam_c(-0.15, 0.35);
    const cdouble lam_d = std::exp(lam_c * dt);
    const auto mi = modal_info(lam_d, dt);
    EXPECT_NEAR(mi.lambda_c.real(), lam_c.real(), 1e-12);
    EXPECT_NEAR(mi.lambda_c.imag(), lam_c.imag(), 1e-12);
    EXPECT_NEAR(mi.period_s, 2.0 * M_PI / 0.35, 1e-9);
    EXPECT_NEAR(mi.time_to_double_s, std::log(2.0) / -0.15, 1e-9);
}

}  // namespace
