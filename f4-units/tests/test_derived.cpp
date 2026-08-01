#include <f4/f4_units.hpp>
#include <gtest/gtest.h>

using namespace f4;
using namespace f4::literals;

// ============================================================================
// Speed of sound
// ============================================================================

TEST(SpeedOfSound, AtISABeaLevel) {
    auto sos = speed_of_sound(isa::sea_level_temp);
    EXPECT_NEAR(sos.value(), 340.29, 0.01);
}

TEST(SpeedOfSound, AtTropopause) {
    auto temp = Quantity<Kelvin>(216.65);
    auto sos = speed_of_sound(temp);
    EXPECT_NEAR(sos.value(), 295.07, 0.01);
}

TEST(SpeedOfSound, AtHighTemperature) {
    auto temp = Quantity<Kelvin>(310.0);
    auto sos = speed_of_sound(temp);
    EXPECT_NEAR(sos.value(), 352.9, 0.1);
}

TEST(SpeedOfSound, IncreasesWithTemperature) {
    auto sos_cold = speed_of_sound(Quantity<Kelvin>(250.0));
    auto sos_hot  = speed_of_sound(Quantity<Kelvin>(300.0));
    EXPECT_GT(sos_hot, sos_cold);
}

// ============================================================================
// Dynamic pressure
// ============================================================================

TEST(DynamicPressure, AtSeaLevel340Mps) {
    auto rho = isa::sea_level_density;
    auto tas = Quantity<MetersPerSecond>(340.0);
    auto q = dynamic_pressure(rho, tas);
    EXPECT_NEAR(q.to<Pascals>().value(), 70805.0, 1.0);
}

TEST(DynamicPressure, AtZeroSpeedIsZero) {
    auto rho = isa::sea_level_density;
    auto tas = Quantity<MetersPerSecond>(0.0);
    auto q = dynamic_pressure(rho, tas);
    EXPECT_NEAR(q.to<Pascals>().value(), 0.0, 1e-9);
}

TEST(DynamicPressure, Mach1SeaLevel) {
    auto rho = isa::sea_level_density;
    auto tas = isa::sea_level_sos;
    auto q = dynamic_pressure(rho, tas);
    EXPECT_NEAR(q.to<Pascals>().value(), 70930.0, 10.0);
}

// ============================================================================
// Mach number
// ============================================================================

TEST(MachNumber, SubsonicApprox1) {
    auto tas = Quantity<MetersPerSecond>(340.0);
    auto sos = Quantity<MetersPerSecond>(340.3);
    auto m = mach_number(tas, sos);
    EXPECT_NEAR(m.value(), 0.999, 0.001);
}

TEST(MachNumber, Subsonic250Mps) {
    auto tas = Quantity<MetersPerSecond>(250.0);
    auto sos = isa::sea_level_sos;
    auto m = mach_number(tas, sos);
    EXPECT_NEAR(m.value(), 0.735, 0.001);
}

TEST(MachNumber, Supersonic) {
    auto tas = Quantity<MetersPerSecond>(500.0);
    auto sos = isa::sea_level_sos;
    auto m = mach_number(tas, sos);
    EXPECT_NEAR(m.value(), 1.469, 0.001);
}

// ============================================================================
// Wing loading
// ============================================================================

TEST(WingLoading, F16C) {
    auto mass = Quantity<Kilograms>(12000.0);
    auto weight = weight_from_mass(mass);
    auto area = Quantity<SquareMeters>(27.87);
    auto wl = wing_loading(weight, area);
    EXPECT_NEAR(wl.to<Pascals>().value(), 4223.5, 1.0);
    EXPECT_NEAR(wl.to<PSF>().value(), 88.2, 0.1);
}

TEST(WingLoading, LightAircraft) {
    auto mass = Quantity<Kilograms>(1000.0);
    auto weight = weight_from_mass(mass);
    auto area = Quantity<SquareMeters>(16.2);
    auto wl = wing_loading(weight, area);
    EXPECT_NEAR(wl.to<Pascals>().value(), 605.3, 1.0);
}

// ============================================================================
// Weight from mass
// ============================================================================

TEST(WeightFromMass, OneKg) {
    auto w = weight_from_mass(Quantity<Kilograms>(1.0));
    EXPECT_NEAR(w.to<Newtons>().value(), 9.80665, 1e-9);
}

TEST(WeightFromMass, OneThousandKg) {
    auto w = weight_from_mass(Quantity<Kilograms>(1000.0));
    EXPECT_NEAR(w.to<Newtons>().value(), 9806.65, 1e-6);
}

TEST(WeightFromMass, InLbf) {
    auto w = weight_from_mass(Quantity<Kilograms>(100.0));
    EXPECT_NEAR(w.to<PoundForce>().value(), 220.46, 0.01);
}

// ============================================================================
// ISA reference values
// ============================================================================

TEST(ISAReferenceValues, SeaLevel) {
    EXPECT_NEAR(isa::sea_level_temp.to<Celsius>().value(), 15.0, 1e-9);
    EXPECT_NEAR(isa::sea_level_pressure.to<Pascals>().value(), 101325.0, 1e-6);
    EXPECT_NEAR(isa::sea_level_density.value(), 1.225, 1e-9);
    EXPECT_NEAR(isa::sea_level_sos.to<MetersPerSecond>().value(), 340.29, 0.01);
}

// ============================================================================
// Combined: realistic scenario
// ============================================================================

TEST(Scenario, F16At250KnotsISABeaLevel) {
    auto tas = 250.0_kn;
    auto sos = speed_of_sound(isa::sea_level_temp);
    auto mach = mach_number(tas, sos);
    auto rho = isa::sea_level_density;
    auto q = dynamic_pressure(rho, tas);

    EXPECT_NEAR(mach.value(), 0.378, 0.001);
    EXPECT_NEAR(q.to<Pascals>().value(), 10130.0, 10.0);
    EXPECT_NEAR(q.to<PSF>().value(), 211.5, 1.0);
}