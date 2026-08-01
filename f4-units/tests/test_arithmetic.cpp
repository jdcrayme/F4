#include <f4/f4_units.hpp>
#include "catch.hpp"
// floating matchers included in amalgamated

using namespace f4;
using namespace f4::literals;
using namespace Catch::Matchers;

// ============================================================================
// Same-type arithmetic
// ============================================================================

TEST_CASE("Same-type addition", "[arithmetic]") {
    auto a = Quantity<Meters>(100.0);
    auto b = Quantity<Meters>(50.0);
    REQUIRE_THAT((a + b).value(), WithinAbs(150.0, 1e-9));
}

TEST_CASE("Same-type subtraction", "[arithmetic]") {
    auto a = Quantity<Meters>(100.0);
    auto b = Quantity<Meters>(30.0);
    REQUIRE_THAT((a - b).value(), WithinAbs(70.0, 1e-9));
}

TEST_CASE("Compound assignment", "[arithmetic]") {
    auto d = Quantity<Meters>(100.0);
    d += Quantity<Meters>(50.0);
    REQUIRE_THAT(d.value(), WithinAbs(150.0, 1e-9));
    d -= Quantity<Meters>(25.0);
    REQUIRE_THAT(d.value(), WithinAbs(125.0, 1e-9));
    d *= 2.0;
    REQUIRE_THAT(d.value(), WithinAbs(250.0, 1e-9));
    d /= 5.0;
    REQUIRE_THAT(d.value(), WithinAbs(50.0, 1e-9));
}

TEST_CASE("Unary plus and minus", "[arithmetic]") {
    auto a = Quantity<Meters>(50.0);
    REQUIRE_THAT((+a).value(), WithinAbs(50.0, 1e-9));
    REQUIRE_THAT((-a).value(), WithinAbs(-50.0, 1e-9));
}

// ============================================================================
// Scalar arithmetic
// ============================================================================

TEST_CASE("Scalar multiplication (right)", "[arithmetic]") {
    auto d = 100.0_m;
    REQUIRE_THAT((d * 2.0).value(), WithinAbs(200.0, 1e-9));
    REQUIRE_THAT((d * 0.5).value(), WithinAbs(50.0, 1e-9));
}

TEST_CASE("Scalar multiplication (left)", "[arithmetic]") {
    auto d = 100.0_m;
    REQUIRE_THAT((3.0 * d).value(), WithinAbs(300.0, 1e-9));
    REQUIRE_THAT((0.25 * d).value(), WithinAbs(25.0, 1e-9));
}

TEST_CASE("Scalar division", "[arithmetic]") {
    auto d = 100.0_m;
    REQUIRE_THAT((d / 4.0).value(), WithinAbs(25.0, 1e-9));
}

TEST_CASE("Scalar / Quantity gives inverse dimension", "[arithmetic]") {
    auto inv = 2.0 / 100.0_m;
    // 2.0 / 100.0 m = 0.02 1/m
    // Result is in base units of dim_invert(LengthDim) = 1/m
    REQUIRE_THAT(inv.value(), WithinAbs(0.02, 1e-9));
}

// ============================================================================
// Heterogeneous arithmetic (same dimension, different unit)
// ============================================================================

TEST_CASE("Heterogeneous addition: meters + feet", "[arithmetic]") {
    auto m = Quantity<Meters>(100.0);
    auto ft = Quantity<Feet>(328.084);  // ~100 m
    auto result = m + ft;  // result in meters (LHS unit)
    REQUIRE_THAT(result.value(), WithinAbs(199.9975, 1e-3));
}

TEST_CASE("Heterogeneous subtraction: meters - feet", "[arithmetic]") {
    auto d = 1000.0_ft;
    auto cut = 400.0_ft;
    // 1000 ft - 400 ft in feet
    REQUIRE_THAT((d - cut).value(), WithinAbs(600.0, 1e-9));
}

TEST_CASE("Heterogeneous addition: knots + m/s", "[arithmetic]") {
    auto kts = 200.0_kn;
    auto mps = 50.0_mps;
    auto result = kts + mps;  // result in knots (LHS unit)
    // 200 kn = 102.888 m/s, + 50 m/s = 152.888 m/s = 297.27 kn
    REQUIRE_THAT(result.value(), WithinAbs(297.27, 0.01));
}

// ============================================================================
// Cross-dimension arithmetic
// ============================================================================

TEST_CASE("Speed * Time = Length", "[arithmetic][cross-dim]") {
    auto speed = 100.0_kn;  // 100 knots
    auto time = 2.0_hr;     // 2 hours
    auto dist = speed * time;  // in meters (SI base)
    // 100 kn = 51.444 m/s, 2 hr = 7200 s => 370,400 m = 370.4 km
    auto km = dist.to<Kilometers>();
    REQUIRE_THAT(km.value(), WithinAbs(370.4, 0.01));
}

TEST_CASE("Mass * Acceleration = Force", "[arithmetic][cross-dim]") {
    auto mass = Quantity<Kilograms>(1000.0);
    auto accel = Quantity<MetersPerSecondSquared>(9.81);
    auto force = mass * accel;
    REQUIRE_THAT(force.to<Newtons>().value(), WithinAbs(9810.0, 1e-6));
}

TEST_CASE("Force / Area = Pressure", "[arithmetic][cross-dim]") {
    auto force = 10000.0_N;
    auto area = Quantity<SquareMeters>(1.0);
    auto pressure = force / area;
    REQUIRE_THAT(pressure.to<Pascals>().value(), WithinAbs(10000.0, 1e-6));
    auto psi = pressure.to<PSI>();
    REQUIRE_THAT(psi.value(), WithinAbs(1.45038, 1e-3));
}

TEST_CASE("Length * Length = Area", "[arithmetic][cross-dim]") {
    auto l = 10.0_m;
    auto area = l * l;
    // Result is in base unit (m^2), which IS SquareMeters
    REQUIRE_THAT(area.to<SquareMeters>().value(), WithinAbs(100.0, 1e-9));
    auto ft2 = area.to<SquareFeet>();
    REQUIRE_THAT(ft2.value(), WithinAbs(1076.39, 0.01));
}

TEST_CASE("Length / Time = Speed", "[arithmetic][cross-dim]") {
    auto dist = 100.0_m;
    auto time = Quantity<Seconds>(10.0);
    auto speed = dist / time;
    REQUIRE_THAT(speed.to<MetersPerSecond>().value(), WithinAbs(10.0, 1e-9));
}

// ============================================================================
// qpow and qsqrt
// ============================================================================

TEST_CASE("qpow: Length^2 = Area", "[arithmetic][pow]") {
    auto l = 10.0_m;
    auto area = qpow<2>(l);
    REQUIRE_THAT(area.to<SquareMeters>().value(), WithinAbs(100.0, 1e-9));
}

TEST_CASE("qpow: Length^3 = Volume", "[arithmetic][pow]") {
    auto l = 5.0_m;
    auto vol = qpow<3>(l);
    REQUIRE_THAT(vol.to<CubicMeters>().value(), WithinAbs(125.0, 1e-9));
}

TEST_CASE("qpow: negative exponent", "[arithmetic][pow]") {
    auto l = 10.0_m;
    auto inv = qpow<-1>(l);
    REQUIRE_THAT(inv.value(), WithinAbs(0.1, 1e-9));
}

TEST_CASE("qsqrt: Area -> Length", "[arithmetic][sqrt]") {
    auto area = Quantity<SquareMeters>(100.0);
    auto l = qsqrt(area);
    REQUIRE_THAT(l.to<Meters>().value(), WithinAbs(10.0, 1e-9));
}

TEST_CASE("qsqrt: Volume -> Length", "[arithmetic][sqrt]") {
    auto vol = Quantity<CubicMeters>(27.0);
    auto l = qsqrt(qsqrt(vol));  // sqrt twice = fourth root
    REQUIRE_THAT(l.to<Meters>().value(), WithinAbs(3.0, 1e-9));
}

// ============================================================================
// Comparisons
// ============================================================================

TEST_CASE("Same-type comparison", "[arithmetic][comparison]") {
    auto a = 100.0_m;
    auto b = 100.0_m;
    auto c = 200.0_m;
    REQUIRE(a == b);
    REQUIRE(a != c);
    REQUIRE(a < c);
    REQUIRE(a <= b);
    REQUIRE(c > a);
    REQUIRE(c >= b);
}

TEST_CASE("Heterogeneous comparison: meters vs feet", "[arithmetic][comparison]") {
    auto a = 100.0_m;        // 100 m
    auto b = 300.0_ft;       // 91.44 m
    auto c = 400.0_ft;       // 121.92 m
    REQUIRE(a > b);   // 100 m > 91.44 m
    REQUIRE(a < c);   // 100 m < 121.92 m
    REQUIRE(!(a == b));
}

TEST_CASE("Heterogeneous comparison: knots vs m/s", "[arithmetic][comparison]") {
    auto kts = 100.0_kn;
    auto mps = 50.0_mps;
    // 100 kn = 51.44 m/s > 50 m/s
    REQUIRE(kts > mps);
}

// ============================================================================
// Literals
// ============================================================================

TEST_CASE("Literal operators", "[arithmetic][literals]") {
    auto d = 5.0_m + 3.0_ft;
    REQUIRE_THAT(d.value(), WithinAbs(5.9144, 1e-9));

    auto s = 250.0_kn;
    REQUIRE_THAT(s.to<MetersPerSecond>().value(), WithinAbs(128.611, 1e-3));

    auto p = 14.7_psi;
    REQUIRE_THAT(p.to<Pascals>().value(), WithinAbs(101325.0, 1.0));

    auto a = 2.0_m * 3.0_m;
    REQUIRE_THAT(a.to<SquareMeters>().value(), WithinAbs(6.0, 1e-9));
}

// ============================================================================
// in() shorthand
// ============================================================================

TEST_CASE("in() extracts value in target unit", "[arithmetic]") {
    auto d = 1000.0_ft;
    REQUIRE_THAT(d.in<Meters>(), WithinAbs(304.8, 1e-9));
}

TEST_CASE("in_base() extracts value in SI base unit", "[arithmetic]") {
    auto d = 1000.0_ft;
    REQUIRE_THAT(d.in_base(), WithinAbs(304.8, 1e-9));
}
