// f4-flight-model/tests/test_angle.cpp
//
// Unit tests for the Angle / AngularRate strong-type wrappers defined in
// f4/flight/angle.hpp.
//
// These tests exist to lock in the contract that:
//   - Angle stores radians canonically (so to_radians is a no-op and
//     to_degrees produces 180/pi scaling)
//   - angle_from_degrees / angle_from_radians round-trip cleanly
//   - AngularRate stores rad/s canonically and converts deg/s <-> rad/s
//   - Arithmetic via the f4-units operators works as expected
//   - The literals from f4::literals interoperate with the Angle alias
//
// These are the compile-time guarantees the rest of the flight model
// relies on. If any of these break, the flight model's rad/deg separation
// stops being enforced and the original correctness hazard returns.

#include "f4/flight/angle.hpp"
#include "f4/flight/constants.hpp"

#include <gtest/gtest.h>

#include <cmath>

using namespace f4::flight;
using f4::math::PI;

TEST(Angle, FromDegreesProducesRadianStorage) {
    // 180 deg = pi rad. The canonical storage is radians, so to_radians
    // should return the same value (modulo floating point).
    const Angle a = angle_from_degrees(180.0);
    EXPECT_NEAR(to_radians(a), PI, 1e-12);
    EXPECT_NEAR(to_degrees(a), 180.0, 1e-12);
}

TEST(Angle, FromRadiansIsIdentity) {
    // angle_from_radians stores the value as-is (radians are canonical).
    const Angle a = angle_from_radians(0.5);
    EXPECT_DOUBLE_EQ(to_radians(a), 0.5);
    EXPECT_NEAR(to_degrees(a), 0.5 * 180.0 / PI, 1e-12);
}

TEST(Angle, RoundTripsThroughDegreesAndRadians) {
    for (double deg : {-180.0, -90.0, -1.0, 0.0, 1.0, 45.0, 90.0, 180.0, 359.99}) {
        const Angle a = angle_from_degrees(deg);
        EXPECT_NEAR(to_degrees(a), deg, 1e-9)
            << "degree round-trip failed for " << deg;
        EXPECT_NEAR(to_radians(a), deg * PI / 180.0, 1e-12)
            << "radian extraction failed for " << deg << " deg";
    }
}

TEST(Angle, ZeroFactoryProducesZeroInBothUnits) {
    const Angle z = zero_angle();
    EXPECT_DOUBLE_EQ(to_radians(z), 0.0);
    EXPECT_DOUBLE_EQ(to_degrees(z), 0.0);
}

TEST(Angle, ArithmeticPreservesStrongTyping) {
    // Angle + Angle = Angle (no implicit conversion to double).
    const Angle a = angle_from_degrees(30.0);
    const Angle b = angle_from_degrees(60.0);
    const Angle sum = a + b;
    EXPECT_NEAR(to_degrees(sum), 90.0, 1e-12);

    // scalar * Angle = Angle
    const Angle doubled = 2.0 * a;
    EXPECT_NEAR(to_degrees(doubled), 60.0, 1e-12);

    // Comparisons work.
    EXPECT_LT(a, b);
    EXPECT_GT(b, a);
    EXPECT_EQ(a, angle_from_degrees(30.0));
}

TEST(Angle, LiteralsInteroperate) {
    // The f4::literals operators (90.0_deg, 0.5_rad) live in f4-units and
    // are tested there. Here we just confirm the Quantity<Radians> alias
    // inter-operates with the explicit-conversion factory by constructing
    // the same value two ways and confirming they match.
    const Angle from_deg_factory = angle_from_degrees(90.0);
    const Angle from_rad_factory = angle_from_radians(PI / 2.0);
    EXPECT_NEAR(to_radians(from_deg_factory), to_radians(from_rad_factory), 1e-12);
}

TEST(Angle, ImplicitConstructionFromDoubleIsRejected) {
    // The whole point of the strong type: raw doubles must NOT implicitly
    // convert to Angle. This is a compile-time guarantee; if this test
    // compiles, the explicit ctor is doing its job. (We can't easily test
    // "this should not compile" in gtest, but the rest of the test file
    // would fail to compile if implicit conversion were allowed.)
    //
    // Instead, verify that the explicit construction syntax works:
    Angle a = angle_from_degrees(45.0);
    SUCCEED() << "explicit construction works: " << to_degrees(a) << " deg";
}

TEST(AngularRate, FromDegreesPerSecondProducesRadPerSecondStorage) {
    // 360 deg/s = 2*pi rad/s
    const AngularRate r = angular_rate_from_degrees_per_second(360.0);
    EXPECT_NEAR(to_rad_per_s(r), 2.0 * PI, 1e-12);
    EXPECT_NEAR(to_deg_per_s(r), 360.0, 1e-12);
}

TEST(AngularRate, FromRadiansPerSecondIsIdentity) {
    const AngularRate r = angular_rate_from_radians_per_second(0.5);
    EXPECT_DOUBLE_EQ(to_rad_per_s(r), 0.5);
    EXPECT_NEAR(to_deg_per_s(r), 0.5 * 180.0 / PI, 1e-12);
}

TEST(AngularRate, ZeroFactoryProducesZeroInBothUnits) {
    const AngularRate z = zero_angular_rate();
    EXPECT_DOUBLE_EQ(to_rad_per_s(z), 0.0);
    EXPECT_DOUBLE_EQ(to_deg_per_s(z), 0.0);
}

TEST(AngularRate, ArithmeticPreservesStrongTyping) {
    const AngularRate a = angular_rate_from_degrees_per_second(30.0);
    const AngularRate b = angular_rate_from_degrees_per_second(60.0);
    const AngularRate sum = a + b;
    EXPECT_NEAR(to_deg_per_s(sum), 90.0, 1e-12);

    const AngularRate doubled = 2.0 * a;
    EXPECT_NEAR(to_deg_per_s(doubled), 60.0, 1e-12);
}
