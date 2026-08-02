#include <f4/math/solver.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>

using namespace f4::math;

// ============================================================================
// Newton-Raphson — basic convergence
// ============================================================================

TEST(NewtonRaphsonTest, FindsRootOfLinearFunction) {
    // f(x) = 2x - 4, root at x = 2
    auto f  = [](double x) { return 2.0 * x - 4.0; };
    auto df = [](double) { return 2.0; };
    auto r = newton_raphson(f, df, 0.0);
    EXPECT_TRUE(r.converged);
    EXPECT_NEAR(r.root, 2.0, 1e-9);
    EXPECT_NEAR(r.final_value, 0.0, 1e-9);
}

TEST(NewtonRaphsonTest, FindsRootOfQuadratic) {
    // f(x) = x^2 - 4, roots at +2 and -2. Starting near +1 should find +2.
    auto f  = [](double x) { return x * x - 4.0; };
    auto df = [](double x) { return 2.0 * x; };
    auto r = newton_raphson(f, df, 1.0);
    EXPECT_TRUE(r.converged);
    EXPECT_NEAR(r.root, 2.0, 1e-9);
}

TEST(NewtonRaphsonTest, FindsNegativeRoot) {
    auto f  = [](double x) { return x * x - 4.0; };
    auto df = [](double x) { return 2.0 * x; };
    auto r = newton_raphson(f, df, -1.0);
    EXPECT_TRUE(r.converged);
    EXPECT_NEAR(r.root, -2.0, 1e-9);
}

TEST(NewtonRaphsonTest, FindsRootOfCubic) {
    // f(x) = x^3 - 2x - 5, root near 2.0945...
    auto f  = [](double x) { return x * x * x - 2.0 * x - 5.0; };
    auto df = [](double x) { return 3.0 * x * x - 2.0; };
    auto r = newton_raphson(f, df, 2.0);
    EXPECT_TRUE(r.converged);
    EXPECT_NEAR(r.root, 2.0945514815, 1e-6);
}

TEST(NewtonRaphsonTest, ConvergesInOneStepForLinear) {
    auto f  = [](double x) { return 3.0 * x - 6.0; };
    auto df = [](double) { return 3.0; };
    auto r = newton_raphson(f, df, 0.0);
    EXPECT_TRUE(r.converged);
    EXPECT_EQ(r.iterations, 1);
}

// ============================================================================
// Newton-Raphson — bracketing fallback
// ============================================================================

TEST(NewtonRaphsonTest, BracketClampsNewtonStep) {
    // f(x) = x^2 - 4. If we start at x0 = 100 without a bracket, Newton
    // jumps to x = 50, then 25, etc. — slow. With a bracket [0, 10], the
    // step is bounded.
    auto f  = [](double x) { return x * x - 4.0; };
    auto df = [](double x) { return 2.0 * x; };
    auto r = newton_raphson(f, df, 100.0, 1e-9, 50, std::make_pair(0.0, 10.0));
    EXPECT_TRUE(r.converged);
    EXPECT_NEAR(r.root, 2.0, 1e-9);
    EXPECT_GE(r.root, 0.0);
    EXPECT_LE(r.root, 10.0);
}

TEST(NewtonRaphsonTest, ZeroDerivativeTriggersBisection) {
    // f(x) = constant — derivative is zero everywhere. With a bracket,
    // the solver should fall back to bisection and converge to the
    // midpoint (which is wrong for a constant, but at least it doesn't
    // divide by zero). Without a bracket, it should fail gracefully.
    auto f  = [](double) { return 5.0; };
    auto df = [](double) { return 0.0; };

    // Without bracket: should not crash, should report not converged.
    auto r1 = newton_raphson(f, df, 0.0);
    EXPECT_FALSE(r1.converged);

    // With bracket: bisection can't help here because f doesn't change sign.
    // The result should still be "not converged".
    auto r2 = newton_raphson(f, df, 0.0, 1e-9, 50, std::make_pair(-10.0, 10.0));
    EXPECT_FALSE(r2.converged);
}

TEST(NewtonRaphsonTest, BracketFallbackWhenDerivativeVanishesMidway) {
    // f(x) = x^3 - 3x + 1, has a local max at x=-1 (f=3) and min at x=1 (f=-1).
    // Starting at x=1 with derivative 0, we need a bracket to recover.
    auto f  = [](double x) { return x * x * x - 3.0 * x + 1.0; };
    auto df = [](double x) { return 3.0 * x * x - 3.0; };
    // Start at the local minimum x=1 where df=0
    auto r = newton_raphson(f, df, 1.0, 1e-9, 50, std::make_pair(-2.0, 2.0));
    // Should converge to one of the three real roots (approximately
    // -1.879, 0.347, 1.532) that lies within the bracket.
    EXPECT_TRUE(r.converged);
    EXPECT_NEAR(f(r.root), 0.0, 1e-6);
}

TEST(NewtonRaphsonTest, DoesNotExceedMaxIter) {
    auto f  = [](double x) { return std::exp(x) - 1e30; };  // root at x = 30*log(10) ~ 69.07
    auto df = [](double x) { return std::exp(x); };
    auto r = newton_raphson(f, df, 0.0, 1e-9, 5);
    EXPECT_FALSE(r.converged);
    EXPECT_EQ(r.iterations, 5);
}

// ============================================================================
// Bisection
// ============================================================================

TEST(BisectionTest, FindsRootOfMonotonicFunction) {
    auto f = [](double x) { return x * x - 4.0; };
    auto r = bisection(f, 0.0, 10.0);
    EXPECT_TRUE(r.converged);
    EXPECT_NEAR(r.root, 2.0, 1e-6);
}

TEST(BisectionTest, RejectsSameSignBracket) {
    auto f = [](double x) { return x * x + 1.0; };  // always positive
    EXPECT_THROW(bisection(f, -1.0, 1.0), std::invalid_argument);
}

TEST(BisectionTest, FindsNegativeRoot) {
    auto f = [](double x) { return x * x - 4.0; };
    auto r = bisection(f, -10.0, 0.0);
    EXPECT_TRUE(r.converged);
    EXPECT_NEAR(r.root, -2.0, 1e-6);
}

TEST(BisectionTest, ConvergesToTolerance) {
    auto f = [](double x) { return std::sin(x); };
    auto r = bisection(f, 3.0, 4.0, 1e-12);
    EXPECT_TRUE(r.converged);
    EXPECT_NEAR(r.root, std::numbers::pi, 1e-9);
}

// ============================================================================
// Practical use case: trim-style problem
// ============================================================================

TEST(NewtonRaphsonTest, TrimLikeProblemFindsEquilibrium) {
    // Simulate the trim problem: find alpha such that the vertical force
    // balances. Model a simple lift curve: L = 2*alpha + 0.5 (per rad),
    // W = 1.0 (weight). Solve L - W = 0 -> alpha = 0.25 rad.
    auto f  = [](double alpha) { return 2.0 * alpha + 0.5 - 1.0; };
    auto df = [](double) { return 2.0; };
    auto r = newton_raphson(f, df, 0.0);
    EXPECT_TRUE(r.converged);
    EXPECT_NEAR(r.root, 0.25, 1e-9);
}
