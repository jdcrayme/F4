#include <f4/f4_units.hpp>
#include "catch.hpp"
// floating matchers included in amalgamated

using namespace f4;
using namespace f4::literals;
using namespace Catch::Matchers;

// ============================================================================
// Speed of sound
// ============================================================================

TEST_CASE("Speed of sound at ISA sea level (288.15 K)", "[derived][sos]") {
    auto sos = speed_of_sound(isa::sea_level_temp);
    REQUIRE_THAT(sos.value(), WithinAbs(340.29, 0.01));
}

TEST_CASE("Speed of sound at tropopause (216.65 K)", "[derived][sos]") {
    auto temp = Quantity<Kelvin>(216.65);
    auto sos = speed_of_sound(temp);
    REQUIRE_THAT(sos.value(), WithinAbs(295.07, 0.01));
}

TEST_CASE("Speed of sound at high temperature (310 K)", "[derived][sos]") {
    auto temp = Quantity<Kelvin>(310.0);
    auto sos = speed_of_sound(temp);
    // a = sqrt(1.4 * 287.058 * 310) = sqrt(124539.3) = 352.9 m/s
    REQUIRE_THAT(sos.value(), WithinAbs(352.9, 0.1));
}

TEST_CASE("Speed of sound increases with temperature", "[derived][sos]") {
    auto sos_cold = speed_of_sound(Quantity<Kelvin>(250.0));
    auto sos_hot  = speed_of_sound(Quantity<Kelvin>(300.0));
    REQUIRE(sos_hot > sos_cold);
}

// ============================================================================
// Dynamic pressure
// ============================================================================

TEST_CASE("Dynamic pressure at sea level, 340 m/s", "[derived][qbar]") {
    auto rho = isa::sea_level_density;  // 1.225 kg/m^3
    auto tas = Quantity<MetersPerSecond>(340.0);
    auto q = dynamic_pressure(rho, tas);
    // q = 0.5 * 1.225 * 340^2 = 0.5 * 1.225 * 115600 = 70805 Pa
    REQUIRE_THAT(q.to<Pascals>().value(), WithinAbs(70805.0, 1.0));
}

TEST_CASE("Dynamic pressure at zero speed is zero", "[derived][qbar]") {
    auto rho = isa::sea_level_density;
    auto tas = Quantity<MetersPerSecond>(0.0);
    auto q = dynamic_pressure(rho, tas);
    REQUIRE_THAT(q.to<Pascals>().value(), WithinAbs(0.0, 1e-9));
}

TEST_CASE("Dynamic pressure at Mach 1, sea level ~70900 Pa", "[derived][qbar]") {
    auto rho = isa::sea_level_density;
    auto tas = isa::sea_level_sos;
    auto q = dynamic_pressure(rho, tas);
    // q = 0.5 * 1.225 * 340.29^2 = 70930 Pa
    REQUIRE_THAT(q.to<Pascals>().value(), WithinAbs(70930.0, 10.0));
}

// ============================================================================
// Mach number
// ============================================================================

TEST_CASE("Mach number: 340 m/s / 340.3 m/s ~ 0.999", "[derived][mach]") {
    auto tas = Quantity<MetersPerSecond>(340.0);
    auto sos = Quantity<MetersPerSecond>(340.3);
    auto m = mach_number(tas, sos);
    REQUIRE_THAT(m.value(), WithinAbs(0.999, 0.001));
}

TEST_CASE("Mach number: subsonic", "[derived][mach]") {
    auto tas = Quantity<MetersPerSecond>(250.0);
    auto sos = isa::sea_level_sos;
    auto m = mach_number(tas, sos);
    REQUIRE_THAT(m.value(), WithinAbs(0.735, 0.001));
}

TEST_CASE("Mach number: supersonic", "[derived][mach]") {
    auto tas = Quantity<MetersPerSecond>(500.0);
    auto sos = isa::sea_level_sos;
    auto m = mach_number(tas, sos);
    REQUIRE_THAT(m.value(), WithinAbs(1.469, 0.001));
}

// ============================================================================
// Wing loading
// ============================================================================

TEST_CASE("Wing loading: F-16C", "[derived][wing]") {
    // F-16C: mass ~12000 kg, wing area ~27.87 m^2
    auto mass = Quantity<Kilograms>(12000.0);
    auto weight = weight_from_mass(mass);
    auto area = Quantity<SquareMeters>(27.87);
    auto wl = wing_loading(weight, area);
    // W = 12000 * 9.80665 = 117679.8 N
    // WL = 117679.8 / 27.87 = 4223.5 Pa
    REQUIRE_THAT(wl.to<Pascals>().value(), WithinAbs(4223.5, 1.0));
    // In PSF: 4223.5 / 47.88 = 88.2 psf
    REQUIRE_THAT(wl.to<PSF>().value(), WithinAbs(88.2, 0.1));
}

TEST_CASE("Wing loading: light aircraft", "[derived][wing]") {
    // Cessna 172: mass ~1000 kg, wing area ~16.2 m^2
    auto mass = Quantity<Kilograms>(1000.0);
    auto weight = weight_from_mass(mass);
    auto area = Quantity<SquareMeters>(16.2);
    auto wl = wing_loading(weight, area);
    // W = 1000 * 9.80665 = 9806.65 N
    // WL = 9806.65 / 16.2 = 605.3 Pa
    REQUIRE_THAT(wl.to<Pascals>().value(), WithinAbs(605.3, 1.0));
}

// ============================================================================
// Weight from mass
// ============================================================================

TEST_CASE("Weight from mass: 1 kg = 9.80665 N", "[derived][weight]") {
    auto w = weight_from_mass(Quantity<Kilograms>(1.0));
    REQUIRE_THAT(w.to<Newtons>().value(), WithinAbs(9.80665, 1e-9));
}

TEST_CASE("Weight from mass: 1000 kg = 9806.65 N", "[derived][weight]") {
    auto w = weight_from_mass(Quantity<Kilograms>(1000.0));
    REQUIRE_THAT(w.to<Newtons>().value(), WithinAbs(9806.65, 1e-6));
}

TEST_CASE("Weight from mass in lbf", "[derived][weight]") {
    auto w = weight_from_mass(Quantity<Kilograms>(100.0));
    // 100 kg * 9.80665 = 980.665 N = 220.46 lbf
    REQUIRE_THAT(w.to<PoundForce>().value(), WithinAbs(220.46, 0.01));
}

// ============================================================================
// ISA reference values
// ============================================================================

TEST_CASE("ISA sea level values are correct", "[derived][isa]") {
    REQUIRE_THAT(isa::sea_level_temp.to<Celsius>().value(), WithinAbs(15.0, 1e-9));
    REQUIRE_THAT(isa::sea_level_pressure.to<Pascals>().value(), WithinAbs(101325.0, 1e-6));
    REQUIRE_THAT(isa::sea_level_density.value(), WithinAbs(1.225, 1e-9));
    REQUIRE_THAT(isa::sea_level_sos.to<MetersPerSecond>().value(), WithinAbs(340.29, 0.01));
}

// ============================================================================
// Combined: realistic scenario
// ============================================================================

TEST_CASE("Full scenario: F-16 at 250 knots, ISA sea level", "[derived][scenario]") {
    auto tas = 250.0_kn;
    auto sos = speed_of_sound(isa::sea_level_temp);
    auto mach = mach_number(tas, sos);
    auto rho = isa::sea_level_density;
    auto q = dynamic_pressure(rho, tas);

    // 250 kn = 128.6 m/s, sos = 340.29 m/s -> M = 0.378
    REQUIRE_THAT(mach.value(), WithinAbs(0.378, 0.001));

    // q = 0.5 * 1.225 * 128.6^2 = 10130 Pa
    REQUIRE_THAT(q.to<Pascals>().value(), WithinAbs(10130.0, 10.0));

    // q in PSF: 10130 / 47.88 = 211.5 psf
    REQUIRE_THAT(q.to<PSF>().value(), WithinAbs(211.5, 1.0));
}