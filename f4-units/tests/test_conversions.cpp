#include <f4/f4_units.hpp>
#include "catch.hpp"
// floating matchers included in amalgamated

using namespace f4;
using namespace Catch::Matchers;

// ============================================================================
// Length
// ============================================================================

TEST_CASE("Length: feet to meters", "[conversions][length]") {
    auto ft = Quantity<Feet>(1000.0);
    auto m = ft.to<Meters>();
    REQUIRE_THAT(m.value(), WithinAbs(304.8, 1e-9));
}

TEST_CASE("Length: nautical miles to meters", "[conversions][length]") {
    auto nm = Quantity<NauticalMiles>(1.0);
    REQUIRE_THAT(nm.to<Meters>().value(), WithinAbs(1852.0, 1e-6));
}

TEST_CASE("Length: kilometers to miles", "[conversions][length]") {
    auto km = Quantity<Kilometers>(1.0);
    REQUIRE_THAT(km.to<Miles>().value(), WithinAbs(0.621371, 1e-5));
}

TEST_CASE("Length: roundtrip meters -> feet -> meters", "[conversions][length]") {
    auto orig = Quantity<Meters>(100.0);
    auto rt = orig.to<Feet>().to<Meters>();
    REQUIRE_THAT(rt.value(), WithinAbs(100.0, 1e-9));
}

TEST_CASE("Length: roundtrip nautical miles -> feet -> nautical miles", "[conversions][length]") {
    auto orig = Quantity<NauticalMiles>(5.0);
    auto rt = orig.to<Feet>().to<NauticalMiles>();
    REQUIRE_THAT(rt.value(), WithinAbs(5.0, 1e-9));
}

TEST_CASE("Length: inches to centimeters", "[conversions][length]") {
    auto in = Quantity<Inches>(1.0);
    REQUIRE_THAT(in.to<Centimeters>().value(), WithinAbs(2.54, 1e-9));
}

// ============================================================================
// Speed
// ============================================================================

TEST_CASE("Speed: knots to m/s", "[conversions][speed]") {
    auto kts = Quantity<Knots>(100.0);
    REQUIRE_THAT(kts.to<MetersPerSecond>().value(), WithinAbs(51.4444, 1e-3));
}

TEST_CASE("Speed: knots to mph", "[conversions][speed]") {
    auto kts = Quantity<Knots>(100.0);
    REQUIRE_THAT(kts.to<MilesPerHour>().value(), WithinAbs(115.078, 1e-3));
}

TEST_CASE("Speed: roundtrip knots -> m/s -> knots", "[conversions][speed]") {
    auto orig = Quantity<Knots>(250.0);
    auto rt = orig.to<MetersPerSecond>().to<Knots>();
    REQUIRE_THAT(rt.value(), WithinAbs(250.0, 1e-9));
}

TEST_CASE("Speed: m/s to kph", "[conversions][speed]") {
    auto mps = Quantity<MetersPerSecond>(10.0);
    REQUIRE_THAT(mps.to<KilometersPerHour>().value(), WithinAbs(36.0, 1e-9));
}

TEST_CASE("Speed: ft/s to m/s", "[conversions][speed]") {
    auto fps = Quantity<FeetPerSecond>(100.0);
    REQUIRE_THAT(fps.to<MetersPerSecond>().value(), WithinAbs(30.48, 1e-6));
}

// ============================================================================
// Pressure
// ============================================================================

TEST_CASE("Pressure: PSI to pascals", "[conversions][pressure]") {
    auto psi = Quantity<PSI>(1.0);
    REQUIRE_THAT(psi.to<Pascals>().value(), WithinAbs(6894.757, 1e-3));
}

TEST_CASE("Pressure: atmosphere to pascals", "[conversions][pressure]") {
    auto atm = Quantity<Atmospheres>(1.0);
    REQUIRE_THAT(atm.to<Pascals>().value(), WithinAbs(101325.0, 1e-6));
}

TEST_CASE("Pressure: 29.92 inHg = 1 atm", "[conversions][pressure]") {
    auto inhg = Quantity<InHg>(29.92);
    auto pa = inhg.to<Pascals>();
    REQUIRE_THAT(pa.value(), WithinAbs(101325.0, 10.0));
}

TEST_CASE("Pressure: 144 PSF = 1 PSI", "[conversions][pressure]") {
    auto psf = Quantity<PSF>(144.0);
    REQUIRE_THAT(psf.to<PSI>().value(), WithinAbs(1.0, 1e-9));
}

TEST_CASE("Pressure: hPa = mbar", "[conversions][pressure]") {
    auto hpa = Quantity<Hectopascals>(1013.25);
    REQUIRE_THAT(hpa.to<Millibars>().value(), WithinAbs(1013.25, 1e-9));
}

TEST_CASE("Pressure: roundtrip PSI -> inHg -> PSI", "[conversions][pressure]") {
    auto orig = Quantity<PSI>(14.696);
    auto rt = orig.to<InHg>().to<PSI>();
    REQUIRE_THAT(rt.value(), WithinAbs(14.696, 1e-6));
}

// ============================================================================
// Mass
// ============================================================================

TEST_CASE("Mass: pounds to kilograms", "[conversions][mass]") {
    auto lb = Quantity<Pounds>(1.0);
    REQUIRE_THAT(lb.to<Kilograms>().value(), WithinAbs(0.45359237, 1e-9));
}

TEST_CASE("Mass: slugs to kilograms", "[conversions][mass]") {
    auto sl = Quantity<Slugs>(1.0);
    REQUIRE_THAT(sl.to<Kilograms>().value(), WithinAbs(14.593903, 1e-6));
}

TEST_CASE("Mass: roundtrip kg -> lb -> kg", "[conversions][mass]") {
    auto orig = Quantity<Kilograms>(100.0);
    auto rt = orig.to<Pounds>().to<Kilograms>();
    REQUIRE_THAT(rt.value(), WithinAbs(100.0, 1e-9));
}

// ============================================================================
// Time
// ============================================================================

TEST_CASE("Time: hours to seconds", "[conversions][time]") {
    auto hr = Quantity<Hours>(1.0);
    REQUIRE_THAT(hr.to<Seconds>().value(), WithinAbs(3600.0, 1e-9));
}

TEST_CASE("Time: minutes to seconds", "[conversions][time]") {
    auto min = Quantity<Minutes>(1.0);
    REQUIRE_THAT(min.to<Seconds>().value(), WithinAbs(60.0, 1e-9));
}

TEST_CASE("Time: roundtrip seconds -> hours -> seconds", "[conversions][time]") {
    auto orig = Quantity<Seconds>(7200.0);
    auto rt = orig.to<Hours>().to<Seconds>();
    REQUIRE_THAT(rt.value(), WithinAbs(7200.0, 1e-9));
}

// ============================================================================
// Angle
// ============================================================================

TEST_CASE("Angle: 180 degrees to pi radians", "[conversions][angle]") {
    auto deg = Quantity<Degrees>(180.0);
    REQUIRE_THAT(deg.to<Radians>().value(), WithinAbs(3.14159265358979, 1e-9));
}

TEST_CASE("Angle: 360 degrees to 2*pi radians", "[conversions][angle]") {
    auto deg = Quantity<Degrees>(360.0);
    REQUIRE_THAT(deg.to<Radians>().value(), WithinAbs(6.28318530717959, 1e-9));
}

TEST_CASE("Angle: 6400 mils to 360 degrees", "[conversions][angle]") {
    auto mil = Quantity<Mils>(6400.0);
    REQUIRE_THAT(mil.to<Degrees>().value(), WithinAbs(360.0, 1e-6));
}

TEST_CASE("Angle: roundtrip degrees -> radians -> degrees", "[conversions][angle]") {
    auto orig = Quantity<Degrees>(45.0);
    auto rt = orig.to<Radians>().to<Degrees>();
    REQUIRE_THAT(rt.value(), WithinAbs(45.0, 1e-9));
}

// ============================================================================
// Area
// ============================================================================

TEST_CASE("Area: square feet to square meters", "[conversions][area]") {
    auto ft2 = Quantity<SquareFeet>(1.0);
    REQUIRE_THAT(ft2.to<SquareMeters>().value(), WithinAbs(0.09290304, 1e-9));
}

TEST_CASE("Area: roundtrip m^2 -> ft^2 -> m^2", "[conversions][area]") {
    auto orig = Quantity<SquareMeters>(100.0);
    auto rt = orig.to<SquareFeet>().to<SquareMeters>();
    REQUIRE_THAT(rt.value(), WithinAbs(100.0, 1e-9));
}

// ============================================================================
// Density
// ============================================================================

TEST_CASE("Density: slugs/ft^3 to kg/m^3", "[conversions][density]") {
    auto sl = Quantity<SlugsPerCubicFoot>(1.0);
    REQUIRE_THAT(sl.to<KilogramsPerCubicMeter>().value(), WithinAbs(515.379, 1e-3));
}

// ============================================================================
// Force
// ============================================================================

TEST_CASE("Force: pounds-force to newtons", "[conversions][force]") {
    auto lbf = Quantity<PoundForce>(1.0);
    REQUIRE_THAT(lbf.to<Newtons>().value(), WithinAbs(4.44822, 1e-5));
}

TEST_CASE("Force: kN to N", "[conversions][force]") {
    auto kn = Quantity<KiloNewtons>(5.0);
    REQUIRE_THAT(kn.to<Newtons>().value(), WithinAbs(5000.0, 1e-9));
}