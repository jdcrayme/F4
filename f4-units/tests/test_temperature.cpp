#include <f4/f4_units.hpp>
#include <gtest/gtest.h>

using namespace f4;

// ============================================================================
// Celsius <-> Kelvin
// ============================================================================

TEST(Temperature, CelsiusToKelvin) {
    auto c = Quantity<Celsius>(0.0);
    EXPECT_NEAR(c.to<Kelvin>().value(), 273.15, 1e-9);
}

TEST(Temperature, KelvinToCelsius) {
    auto k = Quantity<Kelvin>(273.15);
    EXPECT_NEAR(k.to<Celsius>().value(), 0.0, 1e-9);
}

// ============================================================================
// Fahrenheit <-> Kelvin
// ============================================================================

TEST(Temperature, FahrenheitToKelvinFreezing) {
    auto f = Quantity<Fahrenheit>(32.0);
    EXPECT_NEAR(f.to<Kelvin>().value(), 273.15, 1e-6);
}

TEST(Temperature, FahrenheitToKelvinBoiling) {
    auto f = Quantity<Fahrenheit>(212.0);
    EXPECT_NEAR(f.to<Kelvin>().value(), 373.15, 1e-6);
}

TEST(Temperature, KelvinToFahrenheit) {
    auto k = Quantity<Kelvin>(273.15);
    EXPECT_NEAR(k.to<Fahrenheit>().value(), 32.0, 1e-6);
}

// ============================================================================
// Fahrenheit <-> Celsius
// ============================================================================

TEST(Temperature, FahrenheitToCelsius) {
    auto f = Quantity<Fahrenheit>(100.0);
    EXPECT_NEAR(f.to<Celsius>().value(), 37.7778, 1e-4);
}

TEST(Temperature, CelsiusToFahrenheit) {
    auto c = Quantity<Celsius>(100.0);
    EXPECT_NEAR(c.to<Fahrenheit>().value(), 212.0, 1e-6);
}

TEST(Temperature, FahrenheitCelsiusConvergence) {
    auto f = Quantity<Fahrenheit>(-40.0);
    EXPECT_NEAR(f.to<Celsius>().value(), -40.0, 1e-6);
}

// ============================================================================
// Rankine
// ============================================================================

TEST(Temperature, RankineToKelvin) {
    auto r = Quantity<Rankine>(491.67);
    EXPECT_NEAR(r.to<Kelvin>().value(), 273.15, 1e-6);
}

TEST(Temperature, FahrenheitToRankine) {
    auto f = Quantity<Fahrenheit>(32.0);
    EXPECT_NEAR(f.to<Rankine>().value(), 491.67, 1e-3);
}

TEST(Temperature, RankineToFahrenheit) {
    auto r = Quantity<Rankine>(0.0);
    EXPECT_NEAR(r.to<Fahrenheit>().value(), -459.67, 1e-3);
}

// ============================================================================
// Roundtrips
// ============================================================================

TEST(Temperature, RoundtripCelsiusKelvinCelsius) {
    auto orig = Quantity<Celsius>(-40.0);
    auto rt = orig.to<Kelvin>().to<Celsius>();
    EXPECT_NEAR(rt.value(), -40.0, 1e-9);
}

TEST(Temperature, RoundtripFahrenheitKelvinFahrenheit) {
    auto orig = Quantity<Fahrenheit>(-40.0);
    auto rt = orig.to<Kelvin>().to<Fahrenheit>();
    EXPECT_NEAR(rt.value(), -40.0, 1e-6);
}

TEST(Temperature, RoundtripFahrenheitCelsiusFahrenheit) {
    auto orig = Quantity<Fahrenheit>(98.6);
    auto rt = orig.to<Celsius>().to<Fahrenheit>();
    EXPECT_NEAR(rt.value(), 98.6, 1e-6);
}

TEST(Temperature, RoundtripCelsiusRankineCelsius) {
    auto orig = Quantity<Celsius>(20.0);
    auto rt = orig.to<Rankine>().to<Celsius>();
    EXPECT_NEAR(rt.value(), 20.0, 1e-6);
}

TEST(Temperature, RoundtripKelvinFahrenheitKelvin) {
    auto orig = Quantity<Kelvin>(300.0);
    auto rt = orig.to<Fahrenheit>().to<Kelvin>();
    EXPECT_NEAR(rt.value(), 300.0, 1e-6);
}

// ============================================================================
// ISA reference values
// ============================================================================

TEST(Temperature, ISASeaLevel) {
    auto k = Quantity<Kelvin>(288.15);
    EXPECT_NEAR(k.to<Celsius>().value(), 15.0, 1e-9);
}

TEST(Temperature, ISATropopause) {
    auto k = Quantity<Kelvin>(216.65);
    EXPECT_NEAR(k.to<Celsius>().value(), -56.5, 1e-9);
}

// ============================================================================
// Temperature arithmetic (same unit)
// ============================================================================

TEST(Temperature, SameUnitAddition) {
    auto a = Quantity<Kelvin>(100.0);
    auto b = Quantity<Kelvin>(50.0);
    EXPECT_NEAR((a + b).value(), 150.0, 1e-9);
}

TEST(Temperature, ScalarMultiplication) {
    auto k = Quantity<Kelvin>(300.0);
    EXPECT_NEAR((k * 2.0).value(), 600.0, 1e-9);
}

TEST(Temperature, CrossUnitComparison) {
    auto k = Quantity<Kelvin>(273.15);
    auto c = Quantity<Celsius>(0.0);
    EXPECT_EQ(k, c);
    EXPECT_GT(k, Quantity<Celsius>(-1.0));
    EXPECT_LT(k, Quantity<Celsius>(1.0));
}