// test_atmosphere.cpp — tests for the atmosphere model.
//
// Verifies that the 3-layer standard atmosphere produces correct values at
// sea level, the tropopause, and in the stratosphere. Also tests the
// Mach<->KCAS inverse relationship.

#include "f4/flight/atmosphere.hpp"

#include <gtest/gtest.h>

#include <cmath>

using namespace f4::flight;

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

TEST(AtmosphereTest, SeaLevelDensityPressureSound) {
    auto out = computeAtmosphere(0.0, 1000.0, 300.0, 620.0);
    EXPECT_NEAR(out.rho,   RHOASL, 1e-9);
    EXPECT_NEAR(out.pa,    PASL,   1e-6);
    EXPECT_NEAR(out.sound, AASL,   1e-9);
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

TEST(AtmosphereTest, TropopauseBoundary) {
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

TEST(AtmosphereTest, ZeroAirspeedDoesNotProduceNaN) {
    auto out = computeAtmosphere(0.0, 0.0, 300.0, 620.0);
    EXPECT_TRUE(std::isfinite(out.mach));
    EXPECT_TRUE(std::isfinite(out.qbar));
    EXPECT_TRUE(std::isfinite(out.vcas));
}

TEST(AtmosphereTest, ZeroMassDoesNotProduceNaN) {
    auto out = computeAtmosphere(0.0, 1000.0, 300.0, 0.0);
    EXPECT_TRUE(std::isfinite(out.qsom));
}

TEST(AtmosphereTest, NegativeAltitudeHandledGracefully) {
    // Below sea level (e.g., Dead Sea) — should still produce valid values
    auto out = computeAtmosphere(-1000.0, 500.0, 300.0, 620.0);
    EXPECT_GT(out.rho, RHOASL);  // denser than sea level
    EXPECT_GT(out.pa,  PASL);
}
