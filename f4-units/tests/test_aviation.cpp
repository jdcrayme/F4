#include <f4/f4_units.hpp>
#include <gtest/gtest.h>

using namespace f4;
using namespace f4::literals;

// ============================================================================
// CAS type safety
// ============================================================================

TEST(AviationCAS, ConstructibleFromRawValue) {
    auto cas = Quantity<CASKnots>(450.0);
    EXPECT_NEAR(cas.value(), 450.0, 1e-9);
}

TEST(AviationCAS, FromLiteral) {
    auto cas = 450.0_kcas;
    EXPECT_NEAR(cas.value(), 450.0, 1e-9);
}

TEST(AviationCAS, DisplayUnitConversionKnotsToMps) {
    auto cas = Quantity<CASKnots>(100.0);
    auto mps = cas.to<CASMetersPerSecond>();
    EXPECT_NEAR(mps.value(), 51.4444, 1e-3);
}

TEST(AviationCAS, DisplayUnitRoundtrip) {
    auto orig = Quantity<CASKnots>(350.0);
    auto rt = orig.to<CASMetersPerSecond>().to<CASKnots>();
    EXPECT_NEAR(rt.value(), 350.0, 1e-9);
}

TEST(AviationCAS, ScalarArithmetic) {
    auto cas = 400.0_kcas;
    auto doubled = cas * 2.0;
    EXPECT_NEAR(doubled.value(), 800.0, 1e-9);
}

// ============================================================================
// Mach type safety
// ============================================================================

TEST(AviationMach, ConstructibleFromRawValue) {
    auto m = Quantity<MachUnit>(0.85);
    EXPECT_NEAR(m.value(), 0.85, 1e-9);
}

TEST(AviationMach, FromLiteral) {
    auto m = 0.85_mach;
    EXPECT_NEAR(m.value(), 0.85, 1e-9);
}

TEST(AviationMach, ScalarArithmetic) {
    auto m = 0.8_mach;
    auto bumped = m + Quantity<MachUnit>(0.05);
    EXPECT_NEAR(bumped.value(), 0.85, 1e-9);
}

// ============================================================================
// Compile-time isolation
// ============================================================================

TEST(AviationIsolation, CASAndMachAreDistinctTypes) {
    static_assert(!same_dimension_v<CASDim, MachDim>,
        "CAS and Mach must be distinct");
    static_assert(!same_dimension_v<CASDim, SpeedDim>,
        "CAS and Speed must be distinct");
    static_assert(!same_dimension_v<MachDim, Dimensionless>,
        "Mach and Dimensionless must be distinct");
    SUCCEED();
}

TEST(AviationIsolation, CASSupportsSameTypeComparison) {
    auto a = 400.0_kcas;
    auto b = 450.0_kcas;
    EXPECT_LT(a, b);
    EXPECT_GT(b, a);
    EXPECT_NE(a, b);
}

TEST(AviationIsolation, MachSupportsSameTypeComparison) {
    auto a = 0.8_mach;
    auto b = 1.2_mach;
    EXPECT_LT(a, b);
    EXPECT_GT(b, a);
    EXPECT_NE(a, b);
}