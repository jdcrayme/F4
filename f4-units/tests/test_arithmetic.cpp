#include <f4/f4_units.hpp>
#include <gtest/gtest.h>

using namespace f4;
using namespace f4::literals;

// ============================================================================
// Same-type arithmetic
// ============================================================================

TEST(Arithmetic, SameTypeAddition) {
    auto a = Quantity<Meters>(100.0);
    auto b = Quantity<Meters>(50.0);
    EXPECT_NEAR((a + b).value(), 150.0, 1e-9);
}

TEST(Arithmetic, SameTypeSubtraction) {
    auto a = Quantity<Meters>(100.0);
    auto b = Quantity<Meters>(30.0);
    EXPECT_NEAR((a - b).value(), 70.0, 1e-9);
}

TEST(Arithmetic, CompoundAssignment) {
    auto d = Quantity<Meters>(100.0);
    d += Quantity<Meters>(50.0);
    EXPECT_NEAR(d.value(), 150.0, 1e-9);
    d -= Quantity<Meters>(25.0);
    EXPECT_NEAR(d.value(), 125.0, 1e-9);
    d *= 2.0;
    EXPECT_NEAR(d.value(), 250.0, 1e-9);
    d /= 5.0;
    EXPECT_NEAR(d.value(), 50.0, 1e-9);
}

TEST(Arithmetic, UnaryPlusAndMinus) {
    auto a = Quantity<Meters>(50.0);
    EXPECT_NEAR((+a).value(), 50.0, 1e-9);
    EXPECT_NEAR((-a).value(), -50.0, 1e-9);
}

// ============================================================================
// Scalar arithmetic
// ============================================================================

TEST(Arithmetic, ScalarMultiplicationRight) {
    auto d = 100.0_m;
    EXPECT_NEAR((d * 2.0).value(), 200.0, 1e-9);
    EXPECT_NEAR((d * 0.5).value(), 50.0, 1e-9);
}

TEST(Arithmetic, ScalarMultiplicationLeft) {
    auto d = 100.0_m;
    EXPECT_NEAR((3.0 * d).value(), 300.0, 1e-9);
    EXPECT_NEAR((0.25 * d).value(), 25.0, 1e-9);
}

TEST(Arithmetic, ScalarDivision) {
    auto d = 100.0_m;
    EXPECT_NEAR((d / 4.0).value(), 25.0, 1e-9);
}

TEST(Arithmetic, ScalarOverQuantityGivesInverseDimension) {
    auto inv = 2.0 / 100.0_m;
    EXPECT_NEAR(inv.value(), 0.02, 1e-9);
}

// ============================================================================
// Heterogeneous arithmetic (same dimension, different unit)
// ============================================================================

TEST(Arithmetic, HeterogeneousAdditionMetersFeet) {
    auto m = Quantity<Meters>(100.0);
    auto ft = Quantity<Feet>(328.084);
    auto result = m + ft;
    EXPECT_NEAR(result.value(), 199.9975, 1e-3);
}

TEST(Arithmetic, HeterogeneousSubtractionFeet) {
    auto d = 1000.0_ft;
    auto cut = 400.0_ft;
    EXPECT_NEAR((d - cut).value(), 600.0, 1e-9);
}

TEST(Arithmetic, HeterogeneousAdditionKnotsMps) {
    auto kts = 200.0_kn;
    auto mps = 50.0_mps;
    auto result = kts + mps;
    EXPECT_NEAR(result.value(), 297.27, 0.01);
}

// ============================================================================
// Cross-dimension arithmetic
// ============================================================================

TEST(Arithmetic, SpeedTimesTimeEqualsLength) {
    auto speed = 100.0_kn;
    auto time = 2.0_hr;
    auto dist = speed * time;
    auto km = dist.to<Kilometers>();
    EXPECT_NEAR(km.value(), 370.4, 0.01);
}

TEST(Arithmetic, MassTimesAccelerationEqualsForce) {
    auto mass = Quantity<Kilograms>(1000.0);
    auto accel = Quantity<MetersPerSecondSquared>(9.81);
    auto force = mass * accel;
    EXPECT_NEAR(force.to<Newtons>().value(), 9810.0, 1e-6);
}

TEST(Arithmetic, ForceOverAreaEqualsPressure) {
    auto force = 10000.0_N;
    auto area = Quantity<SquareMeters>(1.0);
    auto pressure = force / area;
    EXPECT_NEAR(pressure.to<Pascals>().value(), 10000.0, 1e-6);
    EXPECT_NEAR(pressure.to<PSI>().value(), 1.45038, 1e-3);
}

TEST(Arithmetic, LengthTimesLengthEqualsArea) {
    auto l = 10.0_m;
    auto area = l * l;
    EXPECT_NEAR(area.to<SquareMeters>().value(), 100.0, 1e-9);
    EXPECT_NEAR(area.to<SquareFeet>().value(), 1076.39, 0.01);
}

TEST(Arithmetic, LengthOverTimeEqualsSpeed) {
    auto dist = 100.0_m;
    auto time = Quantity<Seconds>(10.0);
    auto speed = dist / time;
    EXPECT_NEAR(speed.to<MetersPerSecond>().value(), 10.0, 1e-9);
}

// ============================================================================
// qpow and qsqrt
// ============================================================================

TEST(Arithmetic, QpowLengthSquaredEqualsArea) {
    auto l = 10.0_m;
    auto area = qpow<2>(l);
    EXPECT_NEAR(area.to<SquareMeters>().value(), 100.0, 1e-9);
}

TEST(Arithmetic, QpowLengthCubedEqualsVolume) {
    auto l = 5.0_m;
    auto vol = qpow<3>(l);
    EXPECT_NEAR(vol.to<CubicMeters>().value(), 125.0, 1e-9);
}

TEST(Arithmetic, QpowNegativeExponent) {
    auto l = 10.0_m;
    auto inv = qpow<-1>(l);
    EXPECT_NEAR(inv.value(), 0.1, 1e-9);
}

TEST(Arithmetic, QsqrtAreaToLength) {
    auto area = Quantity<SquareMeters>(100.0);
    auto l = qsqrt(area);
    EXPECT_NEAR(l.to<Meters>().value(), 10.0, 1e-9);
}

TEST(Arithmetic, QsqrtL4ToLengthViaDoubleSqrt) {
    auto l = 3.0_m;
    auto l4 = qpow<4>(l);
    auto l2 = qsqrt(l4);
    auto back = qsqrt(l2);
    EXPECT_NEAR(back.to<Meters>().value(), 3.0, 1e-9);
}

// ============================================================================
// Comparisons
// ============================================================================

TEST(Arithmetic, SameTypeComparison) {
    auto a = 100.0_m;
    auto b = 100.0_m;
    auto c = 200.0_m;
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
    EXPECT_LT(a, c);
    EXPECT_LE(a, b);
    EXPECT_GT(c, a);
    EXPECT_GE(c, b);
}

TEST(Arithmetic, HeterogeneousComparisonMetersFeet) {
    auto a = 100.0_m;
    auto b = 300.0_ft;
    auto c = 400.0_ft;
    EXPECT_GT(a, b);
    EXPECT_LT(a, c);
    EXPECT_NE(a, b);
}

TEST(Arithmetic, HeterogeneousComparisonKnotsMps) {
    auto kts = 100.0_kn;
    auto mps = 50.0_mps;
    EXPECT_GT(kts, mps);
}

// ============================================================================
// Literals
// ============================================================================

TEST(Arithmetic, LiteralOperators) {
    auto d = 5.0_m + 3.0_ft;
    EXPECT_NEAR(d.value(), 5.9144, 1e-9);

    auto s = 250.0_kn;
    EXPECT_NEAR(s.to<MetersPerSecond>().value(), 128.611, 1e-3);

    auto p = 14.7_psi;
    EXPECT_NEAR(p.to<Pascals>().value(), 101325.0, 1.0);

    auto a = 2.0_m * 3.0_m;
    EXPECT_NEAR(a.to<SquareMeters>().value(), 6.0, 1e-9);
}

// ============================================================================
// in() shorthand
// ============================================================================

TEST(Arithmetic, InExtractsValueInTargetUnit) {
    auto d = 1000.0_ft;
    EXPECT_NEAR(d.in<Meters>(), 304.8, 1e-9);
}

TEST(Arithmetic, InBaseExtractsValueInSI) {
    auto d = 1000.0_ft;
    EXPECT_NEAR(d.in_base(), 304.8, 1e-9);
}
