#include <f4/f4_units.hpp>
#include <gtest/gtest.h>

using namespace f4;

// ============================================================================
// Length
// ============================================================================

TEST(LengthConversion, FeetToMeters) {
    auto ft = Quantity<Feet>(1000.0);
    EXPECT_NEAR(ft.to<Meters>().value(), 304.8, 1e-9);
}

TEST(LengthConversion, NauticalMilesToMeters) {
    auto nm = Quantity<NauticalMiles>(1.0);
    EXPECT_NEAR(nm.to<Meters>().value(), 1852.0, 1e-6);
}

TEST(LengthConversion, KilometersToMiles) {
    auto km = Quantity<Kilometers>(1.0);
    EXPECT_NEAR(km.to<Miles>().value(), 0.621371, 1e-5);
}

TEST(LengthConversion, RoundtripMetersFeetMeters) {
    auto orig = Quantity<Meters>(100.0);
    auto rt = orig.to<Feet>().to<Meters>();
    EXPECT_NEAR(rt.value(), 100.0, 1e-9);
}

TEST(LengthConversion, RoundtripNmFeetNm) {
    auto orig = Quantity<NauticalMiles>(5.0);
    auto rt = orig.to<Feet>().to<NauticalMiles>();
    EXPECT_NEAR(rt.value(), 5.0, 1e-9);
}

TEST(LengthConversion, InchesToCentimeters) {
    auto in = Quantity<Inches>(1.0);
    EXPECT_NEAR(in.to<Centimeters>().value(), 2.54, 1e-9);
}

// ============================================================================
// Speed
// ============================================================================

TEST(SpeedConversion, KnotsToMps) {
    auto kts = Quantity<Knots>(100.0);
    EXPECT_NEAR(kts.to<MetersPerSecond>().value(), 51.4444, 1e-3);
}

TEST(SpeedConversion, KnotsToMph) {
    auto kts = Quantity<Knots>(100.0);
    EXPECT_NEAR(kts.to<MilesPerHour>().value(), 115.078, 1e-3);
}

TEST(SpeedConversion, RoundtripKnotsMpsKnots) {
    auto orig = Quantity<Knots>(250.0);
    auto rt = orig.to<MetersPerSecond>().to<Knots>();
    EXPECT_NEAR(rt.value(), 250.0, 1e-9);
}

TEST(SpeedConversion, MpsToKph) {
    auto mps = Quantity<MetersPerSecond>(10.0);
    EXPECT_NEAR(mps.to<KilometersPerHour>().value(), 36.0, 1e-9);
}

TEST(SpeedConversion, FpsToMps) {
    auto fps = Quantity<FeetPerSecond>(100.0);
    EXPECT_NEAR(fps.to<MetersPerSecond>().value(), 30.48, 1e-6);
}

// ============================================================================
// Pressure
// ============================================================================

TEST(PressureConversion, PsiToPascals) {
    auto psi = Quantity<PSI>(1.0);
    EXPECT_NEAR(psi.to<Pascals>().value(), 6894.757, 1e-3);
}

TEST(PressureConversion, AtmosphereToPascals) {
    auto atm = Quantity<Atmospheres>(1.0);
    EXPECT_NEAR(atm.to<Pascals>().value(), 101325.0, 1e-6);
}

TEST(PressureConversion, InHgToPascals) {
    auto inhg = Quantity<InHg>(29.92);
    EXPECT_NEAR(inhg.to<Pascals>().value(), 101325.0, 10.0);
}

TEST(PressureConversion, PsfToPsi) {
    auto psf = Quantity<PSF>(144.0);
    EXPECT_NEAR(psf.to<PSI>().value(), 1.0, 1e-9);
}

TEST(PressureConversion, HPaEqualsMbar) {
    auto hpa = Quantity<Hectopascals>(1013.25);
    EXPECT_NEAR(hpa.to<Millibars>().value(), 1013.25, 1e-9);
}

TEST(PressureConversion, RoundtripPsiInHgPsi) {
    auto orig = Quantity<PSI>(14.696);
    auto rt = orig.to<InHg>().to<PSI>();
    EXPECT_NEAR(rt.value(), 14.696, 1e-6);
}

// ============================================================================
// Mass
// ============================================================================

TEST(MassConversion, PoundsToKilograms) {
    auto lb = Quantity<Pounds>(1.0);
    EXPECT_NEAR(lb.to<Kilograms>().value(), 0.45359237, 1e-9);
}

TEST(MassConversion, SlugsToKilograms) {
    auto sl = Quantity<Slugs>(1.0);
    EXPECT_NEAR(sl.to<Kilograms>().value(), 14.593903, 1e-6);
}

TEST(MassConversion, RoundtripKgLbKg) {
    auto orig = Quantity<Kilograms>(100.0);
    auto rt = orig.to<Pounds>().to<Kilograms>();
    EXPECT_NEAR(rt.value(), 100.0, 1e-9);
}

// ============================================================================
// Time
// ============================================================================

TEST(TimeConversion, HoursToSeconds) {
    auto hr = Quantity<Hours>(1.0);
    EXPECT_NEAR(hr.to<Seconds>().value(), 3600.0, 1e-9);
}

TEST(TimeConversion, MinutesToSeconds) {
    auto min = Quantity<Minutes>(1.0);
    EXPECT_NEAR(min.to<Seconds>().value(), 60.0, 1e-9);
}

TEST(TimeConversion, RoundtripSecondsHoursSeconds) {
    auto orig = Quantity<Seconds>(7200.0);
    auto rt = orig.to<Hours>().to<Seconds>();
    EXPECT_NEAR(rt.value(), 7200.0, 1e-9);
}

// ============================================================================
// Angle
// ============================================================================

TEST(AngleConversion, DegreesToRadians) {
    auto deg = Quantity<Degrees>(180.0);
    EXPECT_NEAR(deg.to<Radians>().value(), 3.14159265358979, 1e-9);
}

TEST(AngleConversion, FullCircleTo2PiRadians) {
    auto deg = Quantity<Degrees>(360.0);
    EXPECT_NEAR(deg.to<Radians>().value(), 6.28318530717959, 1e-9);
}

TEST(AngleConversion, MilsToDegrees) {
    auto mil = Quantity<Mils>(6400.0);
    EXPECT_NEAR(mil.to<Degrees>().value(), 360.0, 1e-6);
}

TEST(AngleConversion, RoundtripDegreesRadiansDegrees) {
    auto orig = Quantity<Degrees>(45.0);
    auto rt = orig.to<Radians>().to<Degrees>();
    EXPECT_NEAR(rt.value(), 45.0, 1e-9);
}

// ============================================================================
// Area
// ============================================================================

TEST(AreaConversion, SquareFeetToSquareMeters) {
    auto ft2 = Quantity<SquareFeet>(1.0);
    EXPECT_NEAR(ft2.to<SquareMeters>().value(), 0.09290304, 1e-9);
}

TEST(AreaConversion, RoundtripSqMSqFtSqM) {
    auto orig = Quantity<SquareMeters>(100.0);
    auto rt = orig.to<SquareFeet>().to<SquareMeters>();
    EXPECT_NEAR(rt.value(), 100.0, 1e-9);
}

// ============================================================================
// Density
// ============================================================================

TEST(DensityConversion, SlugsPerFt3ToKgPerM3) {
    auto sl = Quantity<SlugsPerCubicFoot>(1.0);
    EXPECT_NEAR(sl.to<KilogramsPerCubicMeter>().value(), 515.379, 1e-3);
}

// ============================================================================
// Force
// ============================================================================

TEST(ForceConversion, PoundForceToNewtons) {
    auto lbf = Quantity<PoundForce>(1.0);
    EXPECT_NEAR(lbf.to<Newtons>().value(), 4.44822, 1e-5);
}

TEST(ForceConversion, KiloNewtonsToNewtons) {
    auto kn = Quantity<KiloNewtons>(5.0);
    EXPECT_NEAR(kn.to<Newtons>().value(), 5000.0, 1e-9);
}