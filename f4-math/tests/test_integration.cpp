#include <f4/math/integration.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <functional>

using namespace f4::math;

// Small 2-D state type for the harmonic oscillator test. Defined at file
// scope because C++ doesn't allow friend function definitions inside local
// classes (and we need operator*(double, S) for the IntegrableState concept).
struct HarmonicState {
    double y, v;
    HarmonicState operator+(HarmonicState o) const { return {y + o.y, v + o.v}; }
    HarmonicState operator-(HarmonicState o) const { return {y - o.y, v - o.v}; }
    HarmonicState operator*(double s) const { return {y * s, v * s}; }
    friend HarmonicState operator*(double s, HarmonicState a) { return a * s; }
};

// ============================================================================
// Analytical reference problems used throughout this file:
//
//   1. Exponential decay:   dy/dt = -k*y,   y(0) = y0   ->  y(t) = y0 * exp(-k*t)
//   2. Harmonic oscillator: dy/dt = v; dv/dt = -omega^2 * y
//                            ->  y(t) = y0 * cos(omega*t);  v(t) = -y0*omega*sin(omega*t)
//
// These have exact solutions, so we can compute the integrator's actual
// error and verify it scales with the expected order of accuracy.
// ============================================================================

// ============================================================================
// Euler
// ============================================================================

TEST(EulerTest, ExponentialDecayConvergesFirstOrder) {
    const double k = 0.5;
    const double y0 = 1.0;
    auto f = [k](double /*y*/) { return -k * 1.0; };  // placeholder, replaced below

    // Use a struct-free version: state is just double, derivative is -k*state.
    auto deriv = [k](double s) { return -k * s; };

    // Integrate to t=2.0 with two different dt values; error ratio should
    // be ~2 (first-order).
    auto integrate_to = [&](double dt) {
        EulerIntegrator<double> e(y0);
        int n = static_cast<int>(2.0 / dt);
        for (int i = 0; i < n; ++i) e.step(deriv, dt);
        return e.state();
    };

    double err_coarse = std::abs(integrate_to(0.01) - y0 * std::exp(-k * 2.0));
    double err_fine   = std::abs(integrate_to(0.001) - y0 * std::exp(-k * 2.0));
    double ratio = err_coarse / err_fine;
    // First-order: halving dt should roughly halve the error -> ratio ~ 10.
    EXPECT_GT(ratio, 5.0);
    EXPECT_LT(ratio, 15.0);
}

TEST(EulerTest, ZeroDerivativeHoldsState) {
    EulerIntegrator<double> e(42.0);
    e.step([](double) { return 0.0; }, 0.1);
    EXPECT_NEAR(e.state(), 42.0, 1e-12);
}

TEST(EulerTest, ConstantDerivativeAdvancesLinearly) {
    EulerIntegrator<double> e(0.0);
    e.step([](double) { return 1.0; }, 0.1);
    e.step([](double) { return 1.0; }, 0.1);
    e.step([](double) { return 1.0; }, 0.1);
    EXPECT_NEAR(e.state(), 0.3, 1e-12);
}

TEST(EulerTest, ResetRestoresInitialState) {
    EulerIntegrator<double> e(0.0);
    e.step([](double) { return 1.0; }, 1.0);
    e.reset(5.0);
    EXPECT_NEAR(e.state(), 5.0, 1e-12);
}

// ============================================================================
// TrapezoidalIntegrator (FF FITust)
// ============================================================================

TEST(TrapezoidalTest, FirstStepUsesHeunMethod) {
    // Heun's method: k1 = f(y_0) = 1; k2 = f(y_0 + dt*k1) = f(0 + 1*1) = 1
    // y_1 = 0 + (1+1)/2 * 1 = 1
    TrapezoidalIntegrator<double> t(0.0);
    t.step([](double) { return 1.0; }, 1.0);
    EXPECT_NEAR(t.state(), 1.0, 1e-12);
}

TEST(TrapezoidalTest, SecondStepIsAlsoHeun) {
    // For constant f=1, Heun gives y_n = n * dt (same as Euler).
    TrapezoidalIntegrator<double> t(0.0);
    t.step([](double) { return 1.0; }, 1.0);
    t.step([](double) { return 1.0; }, 1.0);
    EXPECT_NEAR(t.state(), 2.0, 1e-12);
}

TEST(TrapezoidalTest, ExponentialDecayConvergesSecondOrder) {
    const double k = 0.5;
    const double y0 = 1.0;
    auto deriv = [k](double s) { return -k * s; };
    auto integrate_to = [&](double dt) {
        TrapezoidalIntegrator<double> t(y0);
        int n = static_cast<int>(2.0 / dt);
        for (int i = 0; i < n; ++i) t.step(deriv, dt);
        return t.state();
    };
    double err_coarse = std::abs(integrate_to(0.01) - y0 * std::exp(-k * 2.0));
    double err_fine   = std::abs(integrate_to(0.001) - y0 * std::exp(-k * 2.0));
    double ratio = err_coarse / err_fine;
    // Second-order: 10x finer dt -> 100x smaller error -> ratio ~ 100.
    EXPECT_GT(ratio, 50.0);
    EXPECT_LT(ratio, 200.0);
}

TEST(TrapezoidalTest, LinearDerivativeExact) {
    // For f(y) = y (linear), Heun's method is exact for the ODE y' = y
    // at each step in the sense that it matches the exact solution of
    // the discretized problem. The result should converge to exp(t)
    // as dt -> 0.
    TrapezoidalIntegrator<double> t(1.0);
    double dt = 0.0001;
    int n = static_cast<int>(1.0 / dt);  // integrate to t=1
    for (int i = 0; i < n; ++i) t.step([](double s) { return s; }, dt);
    // Exact: exp(1) = 2.71828...
    EXPECT_NEAR(t.state(), std::exp(1.0), 1e-4);
}

TEST(TrapezoidalTest, ResetRestoresInitialState) {
    TrapezoidalIntegrator<double> t(0.0);
    t.step([](double) { return 1.0; }, 1.0);
    t.reset(0.0);
    t.step([](double) { return 1.0; }, 1.0);
    EXPECT_NEAR(t.state(), 1.0, 1e-12);
}

// ============================================================================
// AdamsBashforth2Integrator (FF FIAdamsBash)
// ============================================================================

TEST(AdamsBashforth2Test, FirstStepFallsBackToEuler) {
    AdamsBashforth2Integrator<double> t(0.0);
    t.step([](double) { return 1.0; }, 1.0);
    EXPECT_NEAR(t.state(), 1.0, 1e-12);
}

TEST(AdamsBashforth2Test, SecondStepUsesAB2) {
    // Step 1 (Euler): y=1, deriv=1
    // Step 2: deriv at y=1 is 1; prev_deriv is 1
    // AB2: y_2 = 1 + (1/2)*(3*1 - 1)*1 = 1 + 1 = 2
    AdamsBashforth2Integrator<double> t(0.0);
    t.step([](double) { return 1.0; }, 1.0);
    t.step([](double) { return 1.0; }, 1.0);
    EXPECT_NEAR(t.state(), 2.0, 1e-12);
}

TEST(AdamsBashforth2Test, ExponentialDecayConvergesSecondOrder) {
    const double k = 0.5;
    const double y0 = 1.0;
    auto deriv = [k](double s) { return -k * s; };
    auto integrate_to = [&](double dt) {
        AdamsBashforth2Integrator<double> t(y0);
        int n = static_cast<int>(2.0 / dt);
        for (int i = 0; i < n; ++i) t.step(deriv, dt);
        return t.state();
    };
    double err_coarse = std::abs(integrate_to(0.01) - y0 * std::exp(-k * 2.0));
    double err_fine   = std::abs(integrate_to(0.001) - y0 * std::exp(-k * 2.0));
    double ratio = err_coarse / err_fine;
    EXPECT_GT(ratio, 50.0);
    EXPECT_LT(ratio, 200.0);
}

TEST(AdamsBashforth2Test, ResetClearsHistory) {
    AdamsBashforth2Integrator<double> t(0.0);
    t.step([](double) { return 1.0; }, 1.0);
    t.step([](double) { return 1.0; }, 1.0);
    t.reset(0.0);
    t.step([](double) { return 1.0; }, 1.0);  // Euler again
    EXPECT_NEAR(t.state(), 1.0, 1e-12);
}

// ============================================================================
// AdamsBashforth4Integrator
// ============================================================================

TEST(AdamsBashforth4Test, FirstStepIsEuler) {
    AdamsBashforth4Integrator<double> t(0.0);
    t.step([](double) { return 1.0; }, 1.0);
    EXPECT_NEAR(t.state(), 1.0, 1e-12);
}

TEST(AdamsBashforth4Test, FourthOrderConvergenceOnExpDecay) {
    const double k = 0.5;
    const double y0 = 1.0;
    auto deriv = [k](double s) { return -k * s; };
    // Use a longer integration time (t=10) so the AB4 steps dominate over
    // the 3 bootstrap steps (Euler, AB2, AB3).
    auto integrate_to = [&](double dt) {
        AdamsBashforth4Integrator<double> t(y0);
        int n = static_cast<int>(10.0 / dt);
        for (int i = 0; i < n; ++i) t.step(deriv, dt);
        return t.state();
    };
    double err_coarse = std::abs(integrate_to(0.01) - y0 * std::exp(-k * 10.0));
    double err_fine   = std::abs(integrate_to(0.005) - y0 * std::exp(-k * 10.0));
    double ratio = err_coarse / err_fine;
    // Fourth-order: 2x finer dt -> 16x smaller error -> ratio ~ 16.
    EXPECT_GT(ratio, 8.0);
    EXPECT_LT(ratio, 32.0);
}

TEST(AdamsBashforth4Test, ResetClearsHistory) {
    AdamsBashforth4Integrator<double> t(0.0);
    for (int i = 0; i < 5; ++i) t.step([](double) { return 1.0; }, 0.1);
    t.reset(0.0);
    t.step([](double) { return 1.0; }, 0.1);
    EXPECT_NEAR(t.state(), 0.1, 1e-12);  // Euler bootstrap
}

// ============================================================================
// RK4Integrator
// ============================================================================

TEST(RK4Test, ConstantDerivativeExact) {
    RK4Integrator<double> t(0.0);
    t.step([](double) { return 1.0; }, 0.5);
    EXPECT_NEAR(t.state(), 0.5, 1e-12);
}

TEST(RK4Test, ExponentialDecayHighAccuracy) {
    const double k = 0.5;
    const double y0 = 1.0;
    auto deriv = [k](double s) { return -k * s; };
    RK4Integrator<double> t(y0);
    double dt = 0.01;
    int n = static_cast<int>(2.0 / dt);
    for (int i = 0; i < n; ++i) t.step(deriv, dt);
    double exact = y0 * std::exp(-k * 2.0);
    EXPECT_NEAR(t.state(), exact, 1e-7);
}

TEST(RK4Test, FourthOrderConvergence) {
    const double k = 0.5;
    const double y0 = 1.0;
    auto deriv = [k](double s) { return -k * s; };
    auto integrate_to = [&](double dt) {
        RK4Integrator<double> t(y0);
        int n = static_cast<int>(2.0 / dt);
        for (int i = 0; i < n; ++i) t.step(deriv, dt);
        return t.state();
    };
    double err_coarse = std::abs(integrate_to(0.05) - y0 * std::exp(-k * 2.0));
    double err_fine   = std::abs(integrate_to(0.025) - y0 * std::exp(-k * 2.0));
    double ratio = err_coarse / err_fine;
    // Fourth-order: 2x finer dt -> 16x smaller error.
    EXPECT_GT(ratio, 10.0);
    EXPECT_LT(ratio, 25.0);
}

TEST(RK4Test, HarmonicOscillatorPreservesAmplitudeApproximately) {
    // y'' = -omega^2 * y  ->  dy/dt = v, dv/dt = -omega^2 * y
    // Uses HarmonicState defined at file scope.
    const double omega = 1.0;
    auto deriv = [omega](HarmonicState s) -> HarmonicState {
        return HarmonicState{s.v, -omega * omega * s.y};
    };

    RK4Integrator<HarmonicState> integ(HarmonicState{1.0, 0.0});  // y(0)=1, v(0)=0
    double dt = 0.01;
    int n = static_cast<int>(10.0 / dt);  // integrate to t=10
    for (int i = 0; i < n; ++i) integ.step(deriv, dt);

    // Exact: y(10) = cos(10) = -0.8391...
    double exact_y = std::cos(10.0);
    EXPECT_NEAR(integ.state().y, exact_y, 1e-5);

    // Energy (amplitude) should be preserved well: y^2 + v^2 ~ 1
    double energy = integ.state().y * integ.state().y + integ.state().v * integ.state().v;
    EXPECT_NEAR(energy, 1.0, 1e-4);
}

TEST(RK4Test, ResetRestoresInitialState) {
    RK4Integrator<double> t(0.0);
    t.step([](double) { return 1.0; }, 1.0);
    t.reset(7.0);
    EXPECT_NEAR(t.state(), 7.0, 1e-12);
}

// ============================================================================
// Cross-integrator sanity: all should agree on a trivial problem
// ============================================================================

TEST(IntegratorComparisonTest, AllAgreeOnConstantDerivative) {
    auto deriv = [](double) { return 1.0; };
    double dt = 0.1;
    int n = 100;

    EulerIntegrator<double> e(0.0);
    TrapezoidalIntegrator<double> t(0.0);
    AdamsBashforth2Integrator<double> ab2(0.0);
    AdamsBashforth4Integrator<double> ab4(0.0);
    RK4Integrator<double> rk(0.0);

    for (int i = 0; i < n; ++i) {
        e.step(deriv, dt);
        t.step(deriv, dt);
        ab2.step(deriv, dt);
        ab4.step(deriv, dt);
        rk.step(deriv, dt);
    }

    double expected = n * dt;  // 10.0
    EXPECT_NEAR(e.state(),   expected, 1e-12);
    EXPECT_NEAR(t.state(),   expected, 1e-12);
    EXPECT_NEAR(ab2.state(), expected, 1e-12);
    EXPECT_NEAR(ab4.state(), expected, 1e-12);
    EXPECT_NEAR(rk.state(),  expected, 1e-12);
}
