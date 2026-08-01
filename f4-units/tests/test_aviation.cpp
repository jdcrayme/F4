#include <f4/f4_units.hpp>
#include "catch.hpp"
// floating matchers included in amalgamated

using namespace f4;
using namespace f4::literals;
using namespace Catch::Matchers;

// ============================================================================
// CAS type safety
// ============================================================================

TEST_CASE("CAS is constructible from raw value", "[aviation][cas]") {
    auto cas = Quantity<CASKnots>(450.0);
    REQUIRE_THAT(cas.value(), WithinAbs(450.0, 1e-9));
}

TEST_CASE("CAS from literal", "[aviation][cas]") {
    auto cas = 450.0_kcas;
    REQUIRE_THAT(cas.value(), WithinAbs(450.0, 1e-9));
}

TEST_CASE("CAS display-unit conversion: knots to m/s", "[aviation][cas]") {
    auto cas = Quantity<CASKnots>(100.0);
    auto mps = cas.to<CASMetersPerSecond>();
    // Same ratio as speed knots -> m/s
    REQUIRE_THAT(mps.value(), WithinAbs(51.4444, 1e-3));
}

TEST_CASE("CAS display-unit roundtrip", "[aviation][cas]") {
    auto orig = Quantity<CASKnots>(350.0);
    auto rt = orig.to<CASMetersPerSecond>().to<CASKnots>();
    REQUIRE_THAT(rt.value(), WithinAbs(350.0, 1e-9));
}

TEST_CASE("CAS scalar arithmetic", "[aviation][cas]") {
    auto cas = 400.0_kcas;
    auto doubled = cas * 2.0;
    REQUIRE_THAT(doubled.value(), WithinAbs(800.0, 1e-9));
}

// ============================================================================
// Mach type safety
// ============================================================================

TEST_CASE("Mach is constructible from raw value", "[aviation][mach]") {
    auto m = Quantity<MachUnit>(0.85);
    REQUIRE_THAT(m.value(), WithinAbs(0.85, 1e-9));
}

TEST_CASE("Mach from literal", "[aviation][mach]") {
    auto m = 0.85_mach;
    REQUIRE_THAT(m.value(), WithinAbs(0.85, 1e-9));
}

TEST_CASE("Mach scalar arithmetic", "[aviation][mach]") {
    auto m = 0.8_mach;
    auto bumped = m + Quantity<MachUnit>(0.05);
    REQUIRE_THAT(bumped.value(), WithinAbs(0.85, 1e-9));
}

// ============================================================================
// Compile-time isolation (verified via static_assert in aviation.hpp,
// exercised here to confirm they hold at runtime too)
// ============================================================================

TEST_CASE("CAS and Mach are distinct types", "[aviation][isolation]") {
    static_assert(!same_dimension_v<CASDim, MachDim>,
        "CAS and Mach must be distinct");
    static_assert(!same_dimension_v<CASDim, SpeedDim>,
        "CAS and Speed must be distinct");
    static_assert(!same_dimension_v<MachDim, Dimensionless>,
        "Mach and Dimensionless must be distinct");
    REQUIRE(true);
}

TEST_CASE("CAS supports same-type comparison", "[aviation][isolation]") {
    auto a = 400.0_kcas;
    auto b = 450.0_kcas;
    REQUIRE(a < b);
    REQUIRE(b > a);
    REQUIRE(a != b);
}

TEST_CASE("Mach supports same-type comparison", "[aviation][isolation]") {
    auto a = 0.8_mach;
    auto b = 1.2_mach;
    REQUIRE(a < b);
    REQUIRE(b > a);
    REQUIRE(a != b);
}