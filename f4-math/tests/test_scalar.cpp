#include <f4/math/scalar.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <numbers>

using namespace f4::math;

// ============================================================================
// limit
// ============================================================================

TEST(LimitTest, WithinRangeReturnsInput) {
    EXPECT_EQ(limit(5.0, 0.0, 10.0), 5.0);
}

TEST(LimitTest, BelowLowReturnsLow) {
    EXPECT_EQ(limit(-1.0, 0.0, 10.0), 0.0);
}

TEST(LimitTest, AboveHighReturnsHigh) {
    EXPECT_EQ(limit(11.0, 0.0, 10.0), 10.0);
}

TEST(LimitTest, SymmetricClampPositive) {
    EXPECT_EQ(limit(5.0, 3.0), 3.0);
}

TEST(LimitTest, SymmetricClampNegative) {
    EXPECT_EQ(limit(-5.0, 3.0), -3.0);
}

TEST(LimitTest, SymmetricClampWithinRange) {
    EXPECT_EQ(limit(2.0, 3.0), 2.0);
}

TEST(LimitTest, IntegerType) {
    EXPECT_EQ(limit(15, 0, 10), 10);
    EXPECT_EQ(limit(-5, 0, 10), 0);
}

TEST(LimitTest, BoundaryExact) {
    EXPECT_EQ(limit(0.0, 0.0, 10.0), 0.0);
    EXPECT_EQ(limit(10.0, 0.0, 10.0), 10.0);
}

// ============================================================================
// deadBand
// ============================================================================

TEST(DeadBandTest, InsideBandReturnsZero) {
    EXPECT_EQ(deadBand(0.5, 1.0), 0.0);
    EXPECT_EQ(deadBand(-0.5, 1.0), 0.0);
    EXPECT_EQ(deadBand(0.0, 1.0), 0.0);
}

TEST(DeadBandTest, BoundaryReturnsZero) {
    EXPECT_EQ(deadBand(1.0, 1.0), 0.0);
    EXPECT_EQ(deadBand(-1.0, 1.0), 0.0);
}

TEST(DeadBandTest, OutsideBandReturnsInputUnchanged) {
    EXPECT_EQ(deadBand(2.0, 1.0), 2.0);
    EXPECT_EQ(deadBand(-2.0, 1.0), -2.0);
    // Note: FF semantics — does NOT subtract the band. Confirm this matches.
    EXPECT_NE(deadBand(2.0, 1.0), 1.0);
}

// ============================================================================
// wrapPi
// ============================================================================

TEST(WrapPiTest, ZeroStaysZero) {
    EXPECT_NEAR(wrapPi(0.0), 0.0, 1e-12);
}

TEST(WrapPiTest, PiStaysPi) {
    // +pi maps to +pi (not -pi) — the boundary convention we chose.
    EXPECT_NEAR(wrapPi(std::numbers::pi), std::numbers::pi, 1e-12);
}

TEST(WrapPiTest, NegativePiBecomesPositivePi) {
    EXPECT_NEAR(wrapPi(-std::numbers::pi), std::numbers::pi, 1e-12);
}

TEST(WrapPiTest, TwoPiWrapsToZero) {
    EXPECT_NEAR(wrapPi(2.0 * std::numbers::pi), 0.0, 1e-12);
}

TEST(WrapPiTest, ThreeHalvesPiWrapsToMinusHalfPi) {
    EXPECT_NEAR(wrapPi(1.5 * std::numbers::pi), -0.5 * std::numbers::pi, 1e-12);
}

TEST(WrapPiTest, WrapInvariantUnderTwoPiShift) {
    // For all x in a reasonable range, wrapPi(x + 2*pi) == wrapPi(x).
    for (int i = -100; i <= 100; ++i) {
        double x = i * 0.12345;
        double a = wrapPi(x);
        double b = wrapPi(x + 2.0 * std::numbers::pi);
        EXPECT_NEAR(a, b, 1e-9) << "x = " << x;
    }
}

TEST(WrapPiTest, ResultInRange) {
    for (int i = -1000; i <= 1000; ++i) {
        double x = i * 0.317;
        double y = wrapPi(x);
        EXPECT_GT(y, -std::numbers::pi - 1e-9);
        EXPECT_LE(y, std::numbers::pi + 1e-9);
    }
}

TEST(WrapPiTest, FloatOverloadMatchesDouble) {
    EXPECT_NEAR(wrapPi(5.0f), static_cast<float>(wrapPi(5.0)), 1e-5f);
}

// ============================================================================
// wrap2Pi
// ============================================================================

TEST(Wrap2PiTest, ZeroStaysZero) {
    EXPECT_NEAR(wrap2Pi(0.0), 0.0, 1e-12);
}

TEST(Wrap2PiTest, TwoPiWrapsToZero) {
    EXPECT_NEAR(wrap2Pi(2.0 * std::numbers::pi), 0.0, 1e-12);
}

TEST(Wrap2PiTest, NegativeBecomesPositive) {
    EXPECT_NEAR(wrap2Pi(-0.5), 2.0 * std::numbers::pi - 0.5, 1e-12);
}

TEST(Wrap2PiTest, ResultInRange) {
    for (int i = -1000; i <= 1000; ++i) {
        double x = i * 0.317;
        double y = wrap2Pi(x);
        EXPECT_GE(y, -1e-9);
        EXPECT_LT(y, 2.0 * std::numbers::pi + 1e-9);
    }
}

// ============================================================================
// lerp
// ============================================================================

TEST(LerpTest, Endpoints) {
    EXPECT_NEAR(lerp(0.0, 10.0, 0.0), 0.0, 1e-12);
    EXPECT_NEAR(lerp(0.0, 10.0, 1.0), 10.0, 1e-12);
}

TEST(LerpTest, Midpoint) {
    EXPECT_NEAR(lerp(0.0, 10.0, 0.5), 5.0, 1e-12);
}

TEST(LerpTest, ExtrapolatesBeyondB) {
    EXPECT_NEAR(lerp(0.0, 10.0, 1.5), 15.0, 1e-12);
}

TEST(LerpTest, NegativeDirection) {
    EXPECT_NEAR(lerp(10.0, 0.0, 0.25), 7.5, 1e-12);
}

// ============================================================================
// rescale
// ============================================================================

TEST(RescaleTest, IdentityMap) {
    EXPECT_NEAR(rescale(5.0, 0.0, 10.0, 0.0, 10.0), 5.0, 1e-12);
}

TEST(RescaleTest, AffineMap) {
    // Map [0,10] -> [100,200]
    EXPECT_NEAR(rescale(0.0, 0.0, 10.0, 100.0, 200.0), 100.0, 1e-12);
    EXPECT_NEAR(rescale(5.0, 0.0, 10.0, 100.0, 200.0), 150.0, 1e-12);
    EXPECT_NEAR(rescale(10.0, 0.0, 10.0, 100.0, 200.0), 200.0, 1e-12);
}

TEST(RescaleTest, InvertedOutputRange) {
    EXPECT_NEAR(rescale(0.0, 0.0, 10.0, 200.0, 100.0), 200.0, 1e-12);
    EXPECT_NEAR(rescale(10.0, 0.0, 10.0, 200.0, 100.0), 100.0, 1e-12);
}

// ============================================================================
// sign
// ============================================================================

TEST(SignTest, Positive)   { EXPECT_EQ(sign( 3.0),  1.0); }
TEST(SignTest, Negative)   { EXPECT_EQ(sign(-3.0), -1.0); }
TEST(SignTest, Zero)       { EXPECT_EQ(sign( 0.0),  0.0); }
TEST(SignTest, IntegerType) {
    EXPECT_EQ(sign( 5),  1);
    EXPECT_EQ(sign(-5), -1);
    EXPECT_EQ(sign( 0),  0);
}

// ============================================================================
// squared
// ============================================================================

TEST(SquaredTest, Double)  { EXPECT_EQ(squared(3.0), 9.0); }
TEST(SquaredTest, Negative) { EXPECT_EQ(squared(-4.0), 16.0); }
TEST(SquaredTest, Zero)    { EXPECT_EQ(squared(0.0), 0.0); }
TEST(SquaredTest, Int)     { EXPECT_EQ(squared(7), 49); }
