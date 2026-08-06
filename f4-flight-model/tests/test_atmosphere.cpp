// test_atmosphere.cpp — tests for the atmosphere model.
//
// Verifies that the 3-layer standard atmosphere produces correct values at
// sea level, the tropopause, and in the stratosphere. Also tests the
// Mach<->KCAS inverse relationship.
//
// Where possible, tests assert against PUBLISHED ISA-1976 reference values
// (independent of the implementation's own constants) rather than against
// the implementation's internal formulas. This catches silent regressions
// in the layer breakpoints or exponents that "self-consistent" tests miss.

#include "f4/flight/atmosphere.hpp"

#include <gtest/gtest.h>

#include <cmath>

using namespace f4::flight;

// ============================================================================
// Published ISA-1976 reference values (independent of f4's own constants)
//
// Source: NOAA/NASA/USAF, "U.S. Standard Atmosphere, 1976", Table 4.
// The f4 implementation uses the legacy Falcon-4 constants (TROPO_ALT_FT =
// 36089 vs ISA-1976's 11000 m = 36089.24 ft, and exponent 4.255876 vs
// ISA-1976's 4.2558797), so published values are matched to ~0.5% tolerance
// — loose enough to admit the legacy constants, but tight enough to catch
// a wrong layer breakpoint, a swapped exponent, or a missing factor of g.
// ============================================================================
namespace isa1976 {

// Sea level
constexpr double RHO0_SLUGS_PER_FT3 = 0.00237689;   // = 1.225 kg/m^3 in slugs/ft^3
constexpr double P0_LB_PER_FT2      = 2116.22;       // = 101325 Pa in lb/ft^2
constexpr double A0_FT_PER_S        = 1116.44;       // = 340.29 m/s in ft/s

// 18000 ft (representative mid-troposphere, ~5486 m)
constexpr double RHO_18K  = 0.00135462;
constexpr double P_18K    = 1056.80;
constexpr double A_18K    = 1045.07;

// 36089 ft (tropopause, ~11000 m)
constexpr double RHO_TP   = 0.00070612;
constexpr double P_TP     = 472.68;
constexpr double A_TP     = 968.06;
constexpr double TTHETA_TP = 0.751865;  // isothermal stratosphere start

// 50000 ft (lower stratosphere, ~15240 m)
constexpr double RHO_50K  = 0.00036183;
constexpr double P_50K    = 242.22;
constexpr double A_50K    = 968.08;       // same as tropopause — isothermal layer

} // namespace isa1976

// ============================================================================
// Sea level: all ratios should be 1.0
// ============================================================================

TEST(AtmosphereTest, SeaLevelRatiosAreUnity) {
    double ttheta, rsigma;
    double pdelta = calcPressureRatio(0.0, ttheta, rsigma);
    EXPECT_NEAR(ttheta, 1.0, 1e-9);
    EXPECT_NEAR(rsigma, 1.0, 1e-9);
    EXPECT_NEAR(pdelta,  1.0, 1e-9);
}

TEST(AtmosphereTest, SeaLevelDensityMatchesIsa1976) {
    // Independent check: rho at sea level must match the published ISA-1976
    // value (0.00237692 slugs/ft^3 = 1.225 kg/m^3). The previous version of
    // this test compared against the implementation's own RHOASL constant,
    // which is a self-consistency check, not a correctness check.
    auto out = computeAtmosphere(0.0, 1000.0, 300.0, 620.0);
    EXPECT_NEAR(out.rho,   isa1976::RHO0_SLUGS_PER_FT3, 1e-7);
    EXPECT_NEAR(out.pa,    isa1976::P0_LB_PER_FT2,      1e-2);
    EXPECT_NEAR(out.sound, isa1976::A0_FT_PER_S,        1e-2);
}

// ============================================================================
// Troposphere: temperature decreases linearly
// ============================================================================

TEST(AtmosphereTest, TroposphereTemperatureDecreases) {
    double ttheta, rsigma;
    calcPressureRatio(18000.0, ttheta, rsigma);  // 18000 ft
    // ttheta = 1 - 6.875e-6 * 18000 = 1 - 0.12375 = 0.87625
    EXPECT_NEAR(ttheta, 0.87625, 1e-9);
    // rsigma = ttheta^4.255876
    EXPECT_NEAR(rsigma, std::pow(0.87625, 4.255876), 1e-9);
}

TEST(AtmosphereTest, TroposphereAt18000FtMatchesIsa1976) {
    // Full atmosphere check at 18000 ft against published ISA values.
    // Use relative tolerance (~0.5%) to admit the legacy Falcon-4 lapse
    // rate and exponent while still catching a wrong layer or formula.
    auto out = computeAtmosphere(18000.0, 1000.0, 300.0, 620.0);
    EXPECT_NEAR(out.rho,   isa1976::RHO_18K, isa1976::RHO_18K * 5e-3);
    EXPECT_NEAR(out.pa,    isa1976::P_18K,   isa1976::P_18K * 5e-3);
    EXPECT_NEAR(out.sound, isa1976::A_18K,   isa1976::A_18K * 5e-3);
}

TEST(AtmosphereTest, TroposphereDensityDecreases) {
    auto out0 = computeAtmosphere(0.0, 1000.0, 300.0, 620.0);
    auto out18 = computeAtmosphere(18000.0, 1000.0, 300.0, 620.0);
    EXPECT_LT(out18.rho, out0.rho);
    EXPECT_LT(out18.pa,  out0.pa);
    EXPECT_LT(out18.sound, out0.sound);
}

// ============================================================================
// Tropopause (36089 ft): boundary between troposphere and stratosphere
// ============================================================================

TEST(AtmosphereTest, TropopauseBoundaryMatchesIsa1976) {
    auto out = computeAtmosphere(TROPO_ALT_FT, 1000.0, 300.0, 620.0);
    EXPECT_NEAR(out.rho,    isa1976::RHO_TP,     isa1976::RHO_TP * 5e-3);
    EXPECT_NEAR(out.pa,     isa1976::P_TP,       isa1976::P_TP * 5e-3);
    EXPECT_NEAR(out.sound,  isa1976::A_TP,       isa1976::A_TP * 5e-3);
    // The implementation's TROPO_ALT_FT (36089) is 0.24 ft below the
    // ISA-1976 tropopause (36089.24 ft = 11000 m exactly). At 36089 ft the
    // troposphere formula is still active and yields ttheta = 0.75189
    // (slightly above STRATO_TTHETA = 0.751865). The 2.3e-5 mismatch is
    // the legacy constant drift, not a bug; allow 5e-4.
    EXPECT_NEAR(out.ttheta, isa1976::TTHETA_TP, 5e-4);
}

TEST(AtmosphereTest, TropopauseBoundaryIsContinuous) {
    double ttheta_tropo, rsigma_tropo;
    calcPressureRatio(TROPO_ALT_FT, ttheta_tropo, rsigma_tropo);

    double ttheta_strato, rsigma_strato;
    calcPressureRatio(TROPO_ALT_FT + 1.0, ttheta_strato, rsigma_strato);

    // Temperature should be continuous across the boundary
    EXPECT_NEAR(ttheta_tropo, ttheta_strato, 0.01);
    // Density should be continuous
    EXPECT_NEAR(rsigma_tropo, rsigma_strato, 0.01);
}

// ============================================================================
// Lower stratosphere (36089 to 65617 ft): isothermal
// ============================================================================

TEST(AtmosphereTest, StratosphereIsothermal) {
    double ttheta1, rsigma1;
    calcPressureRatio(40000.0, ttheta1, rsigma1);

    double ttheta2, rsigma2;
    calcPressureRatio(50000.0, ttheta2, rsigma2);

    // Temperature is constant in the lower stratosphere
    EXPECT_NEAR(ttheta1, ttheta2, 1e-9);
    EXPECT_NEAR(ttheta1, STRATO_TTHETA, 1e-9);
    // Density decreases with altitude
    EXPECT_LT(rsigma2, rsigma1);
}

TEST(AtmosphereTest, StratosphereAt50000FtMatchesIsa1976) {
    // Independent check: at 50000 ft the lower stratosphere is isothermal,
    // so the speed of sound must equal the tropopause value (968.06 ft/s).
    auto out = computeAtmosphere(50000.0, 1000.0, 300.0, 620.0);
    EXPECT_NEAR(out.sound, isa1976::A_50K,  isa1976::A_50K * 5e-3);
    EXPECT_NEAR(out.rho,   isa1976::RHO_50K, isa1976::RHO_50K * 5e-3);
    EXPECT_NEAR(out.pa,    isa1976::P_50K,   isa1976::P_50K * 5e-3);
    // Isothermal check: sound at 50k must equal sound at the tropopause.
    // The legacy Falcon-4 model uses STRATO_TTHETA = 0.751865 throughout
    // the isothermal layer, but the troposphere formula at TROPO_ALT_FT
    // (which is 0.24 ft below the ISA-1976 tropopause) yields a slightly
    // different ttheta = 0.75189. The resulting sound speed mismatch is
    // ~0.015 ft/s (relative error 1.5e-5) — allow 0.05 ft/s.
    auto out_tp = computeAtmosphere(TROPO_ALT_FT, 1000.0, 300.0, 620.0);
    EXPECT_NEAR(out.sound, out_tp.sound, 0.05);
}

// ============================================================================
// Mach number computation
// ============================================================================

TEST(AtmosphereTest, MachNumberAtSeaLevel) {
    // At sea level, Mach 1 = 1116.44 ft/s
    auto out = computeAtmosphere(0.0, AASL, 300.0, 620.0);
    EXPECT_NEAR(out.mach, 1.0, 1e-6);
}

TEST(AtmosphereTest, MachNumberAtAltitude) {
    // Speed of sound decreases with altitude in the troposphere
    auto out0 = computeAtmosphere(0.0, 1000.0, 300.0, 620.0);
    auto out18 = computeAtmosphere(18000.0, 1000.0, 300.0, 620.0);
    // Same true airspeed, lower speed of sound at altitude -> higher Mach
    EXPECT_GT(out18.mach, out0.mach);
}

// ============================================================================
// Dynamic pressure
// ============================================================================

TEST(AtmosphereTest, DynamicPressureAtSeaLevel) {
    // q = 0.5 * rho * V^2
    auto out = computeAtmosphere(0.0, 1000.0, 300.0, 620.0);
    double expected_q = 0.5 * RHOASL * 1000.0 * 1000.0;
    EXPECT_NEAR(out.qbar, expected_q, 1e-6);
}

TEST(AtmosphereTest, QsomIsQbarTimesAreaOverMass) {
    // qsom = q * S / m
    auto out = computeAtmosphere(0.0, 1000.0, 300.0, 620.0);
    double expected_qsom = out.qbar * 300.0 / 620.0;
    EXPECT_NEAR(out.qsom, expected_qsom, 1e-9);
}

// ============================================================================
// KCAS <-> Mach inverse relationship
// ============================================================================

TEST(AtmosphereTest, MachToKcasBackToMach) {
    // For a range of Mach numbers, Mach -> KCAS -> Mach should round-trip
    auto out = computeAtmosphere(10000.0, 800.0, 300.0, 620.0);
    for (double mach : {0.3, 0.5, 0.8, 0.9, 0.95}) {
        double kcas = calcKcasFromMach(mach, out.pa);
        double mach2 = calcMachFromKcas(kcas, out.pa);
        EXPECT_NEAR(mach, mach2, 0.01) << "mach=" << mach;
    }
}

TEST(AtmosphereTest, SupersonicMachToKcasBackToMach) {
    // Supersonic round-trip (uses the Rayleigh formula + Newton-Raphson).
    // The supersonic inverse is iterative and converges to ~0.05 tolerance.
    auto out = computeAtmosphere(20000.0, 1500.0, 300.0, 620.0);
    for (double mach : {1.1, 1.5, 2.0}) {
        double kcas = calcKcasFromMach(mach, out.pa);
        double mach2 = calcMachFromKcas(kcas, out.pa);
        EXPECT_NEAR(mach, mach2, 0.08) << "mach=" << mach;
    }
}

// ============================================================================
// Edge cases
// ============================================================================

TEST(AtmosphereTest, ZeroAirspeedProducesZeroMachAndZeroQbar) {
    // Implementation floors vt to 1.0 ft/s internally to avoid div-by-zero,
    // but the user-visible Mach at zero airspeed MUST be zero (a stationary
    // aircraft is not moving through the air). The previous test only
    // asserted `isfinite(out.mach)`, which masked a floor that produced a
    // small nonzero Mach. Tighten to assert zero.
    auto out = computeAtmosphere(0.0, 0.0, 300.0, 620.0);
    EXPECT_NEAR(out.mach, 0.0, 1e-9);
    EXPECT_NEAR(out.qbar, 0.0, 1e-9);
    EXPECT_TRUE(std::isfinite(out.vcas));
    EXPECT_TRUE(std::isfinite(out.sound));
}

TEST(AtmosphereTest, ZeroMassDoesNotProduceNaN) {
    auto out = computeAtmosphere(0.0, 1000.0, 300.0, 0.0);
    EXPECT_TRUE(std::isfinite(out.qsom));
    // Implementation floors mass to 1e-6 slugs, so qsom should be very
    // large but finite (not infinity, not NaN).
    EXPECT_GT(out.qsom, 1.0e6);
}

TEST(AtmosphereTest, NegativeAltitudeHandledGracefully) {
    // Below sea level (e.g., Dead Sea) — should still produce valid values
    auto out = computeAtmosphere(-1000.0, 500.0, 300.0, 620.0);
    EXPECT_GT(out.rho, RHOASL);  // denser than sea level
    EXPECT_GT(out.pa,  PASL);
}
