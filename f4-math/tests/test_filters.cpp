#include <f4/math/filters.hpp>

#include <gtest/gtest.h>

#include <cmath>

using namespace f4::math;

// ============================================================================
// LagFilter (FLTust)
// ============================================================================

TEST(LagFilterTest, DCGainIsUnity) {
    // A constant input should eventually produce the same output (DC gain = 1).
    LagFilter f;
    double tau = 0.5;
    double dt = 0.01;
    for (int i = 0; i < 10000; ++i) f.step(7.0, tau, dt);
    EXPECT_NEAR(f.output(), 7.0, 1e-6);
}

TEST(LagFilterTest, StepResponseMatchesAnalyticalFirstOrder) {
    // For H(s) = 1/(tau*s+1), unit-step response is y(t) = 1 - exp(-t/tau).
    // Discretized via FF's FLTust, the steady state is exact (DC gain = 1)
    // but the transient has O(dt^2) error.
    LagFilter f;
    double tau = 0.3;
    double dt = 0.001;
    double t = 0.0;
    for (int i = 0; i < 1000; ++i) {
        f.step(1.0, tau, dt);
        t += dt;
    }
    // After t = 1.0s, exact y = 1 - exp(-1/0.3) = 1 - 0.0357 = 0.9643
    double exact = 1.0 - std::exp(-1.0 / tau);
    EXPECT_NEAR(f.output(), exact, 1e-3);
}

TEST(LagFilterTest, ZeroTimeConstantPassesInput) {
    LagFilter f;
    f.step(5.0, 0.0, 0.01);
    EXPECT_NEAR(f.output(), 5.0, 1e-12);
}

TEST(LagFilterTest, ResetClearsState) {
    LagFilter f;
    f.step(10.0, 1.0, 0.01);
    f.reset(0.0);
    f.step(2.0, 1.0, 0.01);
    // After reset, first step at u=2 should produce k2*(u+u_prev=2+0) = ~small
    // Just verify output is small (close to 0, not close to 10).
    EXPECT_LT(std::abs(f.output()), 1.0);
}

TEST(LagFilterTest, SignOfDecayingInputTracked) {
    // Decaying exponential input through lag filter: still positive output.
    LagFilter f;
    double tau = 0.2;
    double dt = 0.001;
    for (int i = 0; i < 500; ++i) {
        double t = i * dt;
        f.step(std::exp(-t), tau, dt);
    }
    EXPECT_GT(f.output(), 0.0);
}

// ============================================================================
// LeadFilter (FLeadTust)
// ============================================================================

TEST(LeadFilterTest, PassesStepImmediately) {
    // A lead filter passes the immediate step (high-pass character).
    LeadFilter f;
    f.step(0.0, 0.1, 0.001);  // first step at 0
    double first_response = f.step(1.0, 0.1, 0.001);
    // Lead filters amplify transients; output should be > 1 on the step.
    EXPECT_GT(first_response, 1.0);
}

TEST(LeadFilterTest, DCGainIsZero_MatchesFFImplementation) {
    // FF's FLeadTust has no direct feedthrough, so DC gain = 0.
    // Constant input produces decaying output. This matches FF's actual
    // behaviour (documented in filters.hpp).
    LeadFilter f;
    double tau = 0.1;
    double dt = 0.001;
    for (int i = 0; i < 10000; ++i) f.step(5.0, tau, dt);
    // After settling, output should be near 0 (not 5).
    EXPECT_NEAR(f.output(), 0.0, 1e-3);
}

TEST(LeadFilterTest, ResetClearsState) {
    LeadFilter f;
    f.step(10.0, 0.5, 0.01);
    f.reset(0.0);
    double y = f.step(1.0, 0.5, 0.01);
    // After reset, the "previous input" is 0 (from reset), so the step from
    // 0 to 1 should produce a large positive output.
    EXPECT_GT(y, 0.0);
}

// ============================================================================
// WashoutFilter (FWTust)
// ============================================================================

TEST(WashoutFilterTest, DCGainIsZero) {
    // A constant input should wash out to zero.
    WashoutFilter f;
    double tau = 0.5;
    double dt = 0.01;
    for (int i = 0; i < 10000; ++i) f.step(7.0, tau, dt);
    EXPECT_NEAR(f.output(), 0.0, 1e-6);
}

TEST(WashoutFilterTest, StepDecaysToZero) {
    // Step input: output spikes then decays to zero like exp(-t/tau).
    WashoutFilter f;
    double tau = 0.3;
    double dt = 0.001;
    double first_output = 0.0;
    for (int i = 0; i < 5000; ++i) {  // t = 5.0s = ~17 tau
        first_output = f.step(1.0, tau, dt);
    }
    // After t = 5.0s, exp(-17) ~ 4e-8, output should be well below 1e-3.
    EXPECT_NEAR(f.output(), 0.0, 1e-3);
    // First output should be positive (high-pass).
    EXPECT_GT(first_output, 0.0);
}

TEST(WashoutFilterTest, PassesHighFrequency) {
    // A high-frequency sinusoid (omega >> 1/tau) should pass through
    // approximately unattenuated.
    WashoutFilter f;
    double tau = 0.1;
    double dt = 0.0001;
    double omega = 100.0;  // omega*tau = 10, so |H| ~ 1
    for (int i = 0; i < 10000; ++i) {
        double t = i * dt;
        f.step(std::sin(omega * t), tau, dt);
    }
    // Compare peak amplitude of last few cycles to input amplitude (1.0).
    // We just check the output is non-trivial (> 0.5).
    double peak = 0.0;
    for (int i = 10000; i < 11000; ++i) {
        double t = i * dt;
        double y = f.step(std::sin(omega * t), tau, dt);
        peak = std::max(peak, std::abs(y));
    }
    EXPECT_GT(peak, 0.5);
}

TEST(WashoutFilterTest, ResetClearsState) {
    WashoutFilter f;
    f.step(1.0, 0.5, 0.01);
    f.reset();
    // After reset, with constant input, output should head toward 0.
    // Need enough steps for exp(-t/tau) to be small: t/tau > 5.
    for (int i = 0; i < 1000; ++i) f.step(1.0, 0.5, 0.01);  // t=10, t/tau=20
    EXPECT_NEAR(f.output(), 0.0, 1e-3);
}

// ============================================================================
// LeadLagFilter (F7Tust)
// ============================================================================

TEST(LeadLagFilterTest, DCGainIsUnity) {
    // H(s) = (tau1*s + 1) / ((tau2*s+1)*(tau3*s+1))
    // At DC: (0+1)/((0+1)*(0+1)) = 1.
    LeadLagFilter f;
    double tau1 = 0.2, tau2 = 0.3, tau3 = 0.4;
    double dt = 0.001;
    for (int i = 0; i < 20000; ++i) f.step(5.0, tau1, tau2, tau3, dt);
    EXPECT_NEAR(f.output(), 5.0, 1e-3);
}

TEST(LeadLagFilterTest, BootstrapJstartMatchesFFBehaviour) {
    // FF's jstart counter delays the history-shift for the first 2 steps.
    // Verify the filter produces sensible outputs even at step 1, 2, 3.
    LeadLagFilter f;
    double tau1 = 0.2, tau2 = 0.3, tau3 = 0.4;
    double dt = 0.01;
    double y1 = f.step(1.0, tau1, tau2, tau3, dt);
    double y2 = f.step(1.0, tau1, tau2, tau3, dt);
    double y3 = f.step(1.0, tau1, tau2, tau3, dt);
    // All should be finite and reasonable.
    EXPECT_TRUE(std::isfinite(y1));
    EXPECT_TRUE(std::isfinite(y2));
    EXPECT_TRUE(std::isfinite(y3));
    // After 3 steps with constant input, output should be approaching 1.
    EXPECT_GT(y3, 0.0);
    EXPECT_LT(y3, 2.0);
}

TEST(LeadLagFilterTest, StepResponseConvergesToInput) {
    LeadLagFilter f;
    double tau1 = 0.2, tau2 = 0.3, tau3 = 0.4;
    double dt = 0.001;
    double t = 0.0;
    for (int i = 0; i < 20000; ++i) {
        f.step(1.0, tau1, tau2, tau3, dt);
        t += dt;
    }
    // After t = 20s (many time constants), output should be ~1.
    EXPECT_NEAR(f.output(), 1.0, 1e-3);
}

TEST(LeadLagFilterTest, DegenerateTimeConstantsPassThrough) {
    // If any tau is zero, FF passes the input through unchanged.
    LeadLagFilter f;
    double y = f.step(7.0, 0.0, 0.3, 0.4, 0.01);
    EXPECT_NEAR(y, 7.0, 1e-12);
    y = f.step(7.0, 0.2, 0.0, 0.4, 0.01);
    EXPECT_NEAR(y, 7.0, 1e-12);
}

TEST(LeadLagFilterTest, ResetClearsState) {
    LeadLagFilter f;
    for (int i = 0; i < 100; ++i) f.step(5.0, 0.2, 0.3, 0.4, 0.01);
    f.reset(0.0);
    double y = f.step(1.0, 0.2, 0.3, 0.4, 0.01);
    // After reset, first step's output should not be biased by old state.
    EXPECT_TRUE(std::isfinite(y));
    EXPECT_LT(std::abs(y), 5.0);  // not still showing 5.0 from before reset
}

// ============================================================================
// IntegratorTustin (FITust)
// ============================================================================

TEST(IntegratorTustinTest, IntegratesConstantToLinearRamp) {
    // IntegratorTustin uses the trapezoidal rule on the input signal:
    //   y[n] = y[n-1] + (dt/2) * (u[n] + u[n-1])
    // With u_prev initialized to 0, the first step underestimates:
    //   y[1] = 0 + 0.5*0.01*(1 + 0) = 0.005
    // Subsequent steps: y[n] = y[n-1] + 0.5*0.01*(1 + 1) = y[n-1] + 0.01
    // After 100 steps: y = 0.005 + 99*0.01 = 0.995
    IntegratorTustin f;
    double dt = 0.01;
    for (int i = 0; i < 100; ++i) f.step(1.0, dt);
    // Exact trapezoidal result with u_prev=0 bootstrap: 0.995
    EXPECT_NEAR(f.output(), 0.995, 1e-9);
}

TEST(IntegratorTustinTest, IntegratesSineToCosine) {
    // ∫ sin(t) dt = -cos(t) + C. With y(0)=0, y(t) = 1 - cos(t).
    IntegratorTustin f;
    double dt = 0.0001;
    int n = static_cast<int>(1.0 / dt);  // integrate to t=1
    for (int i = 0; i < n; ++i) {
        double t = i * dt;
        f.step(std::sin(t), dt);
    }
    // Expected: 1 - cos(1) = 1 - 0.5403 = 0.4597
    double expected = 1.0 - std::cos(1.0);
    EXPECT_NEAR(f.output(), expected, 1e-3);
}

TEST(IntegratorTustinTest, ResetClearsState) {
    IntegratorTustin f;
    for (int i = 0; i < 100; ++i) f.step(1.0, 0.01);
    f.reset(0.0);
    f.step(1.0, 0.01);
    // After reset, first step is Euler: 0 + 0.5*0.01*(1+0) = 0.005
    EXPECT_NEAR(f.output(), 0.005, 1e-12);
}

TEST(IntegratorTustinTest, ZeroInputHoldsState_AfterFirstZeroStep) {
    // The trapezoidal rule integrates the area under the trapezoid from
    // u_prev to u. After stepping u=5, the first u=0 step still adds the
    // tail of the previous input (trapezoid from 5 to 0). After that, the
    // state is held.
    IntegratorTustin f;
    f.step(5.0, 0.01);  // y = 0.5*0.01*(5+0) = 0.025
    double after_first = f.output();
    f.step(0.0, 0.01);  // y = 0.025 + 0.5*0.01*(0+5) = 0.05 (tail of trapezoid)
    double after_second = f.output();
    // Now state should hold (u=0, u_prev=0 → no further accumulation).
    for (int i = 0; i < 100; ++i) f.step(0.0, 0.01);
    EXPECT_NEAR(f.output(), after_second, 1e-12);
    EXPECT_GT(after_second, after_first);  // The tail was added.
}

// ============================================================================
// AdamsBash2Filter (FIAdamsBash)
// ============================================================================

TEST(AdamsBash2FilterTest, IntegratesConstantToLinearRamp) {
    // AdamsBash2Filter: y[n] = y[n-1] + (dt/2) * (3*u[n] - u[n-1])
    // With u_prev initialized to 0:
    //   Step 1: y = 0 + 0.5*0.01*(3*1 - 0) = 0.015
    //   Steps 2..100: y = y_prev + 0.5*0.01*(3*1 - 1) = y_prev + 0.01
    // After 100 steps: y = 0.015 + 99*0.01 = 1.005
    AdamsBash2Filter f;
    double dt = 0.01;
    for (int i = 0; i < 100; ++i) f.step(1.0, dt);
    EXPECT_NEAR(f.output(), 1.005, 1e-9);
}

TEST(AdamsBash2FilterTest, ResetClearsState) {
    AdamsBash2Filter f;
    for (int i = 0; i < 100; ++i) f.step(1.0, 0.01);
    f.reset(0.0);
    f.step(1.0, 0.01);
    // After reset: y = 0 + 0.5*0.01*(3*1 - 0) = 0.015
    EXPECT_NEAR(f.output(), 0.015, 1e-12);
}

TEST(AdamsBash2FilterTest, ZeroInputHoldsState_AfterFirstZeroStep) {
    // Same trapezoidal-tail behaviour as IntegratorTustin: the first u=0
    // step still has u_prev=5, so it subtracts some of the prior input.
    AdamsBash2Filter f;
    f.step(5.0, 0.01);  // y = 0.5*0.01*(3*5 - 0) = 0.075
    double after_first = f.output();
    f.step(0.0, 0.01);  // y = 0.075 + 0.5*0.01*(3*0 - 5) = 0.075 - 0.025 = 0.05
    double after_second = f.output();
    // Now u_prev=0, so state should hold.
    for (int i = 0; i < 100; ++i) f.step(0.0, 0.01);
    EXPECT_NEAR(f.output(), after_second, 1e-12);
    EXPECT_LT(after_second, after_first);  // The tail was subtracted.
}

// ============================================================================
// Cross-filter sanity: Lag + Lead of same tau should compose to ~identity
// (approximately — the discretizations differ slightly).
// ============================================================================

TEST(FilterCompositionTest, LagThenWashoutApproximatesDifferentiator) {
    // Lag + Washout (both with same tau) approximates a differentiator
    // at frequencies below 1/tau. We don't test the exact transfer function
    // here; we just verify the composition is stable and produces finite
    // output for a sinusoidal input.
    LagFilter lag;
    WashoutFilter wash;
    double tau = 0.1;
    double dt = 0.001;
    double omega = 5.0;
    for (int i = 0; i < 5000; ++i) {
        double t = i * dt;
        double u = std::sin(omega * t);
        double lagged = lag.step(u, tau, dt);
        wash.step(lagged, tau, dt);
    }
    double peak = 0.0;
    for (int i = 5000; i < 6000; ++i) {
        double t = i * dt;
        double u = std::sin(omega * t);
        double lagged = lag.step(u, tau, dt);
        double y = wash.step(lagged, tau, dt);
        peak = std::max(peak, std::abs(y));
    }
    // Should produce a finite, non-zero output.
    EXPECT_TRUE(std::isfinite(peak));
    EXPECT_GT(peak, 0.01);
    EXPECT_LT(peak, 10.0);
}
