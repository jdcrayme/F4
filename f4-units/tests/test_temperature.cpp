#include <f4/f4_units.hpp>
#include "catch.hpp"
// floating matchers included in amalgamated

using namespace f4;
using namespace Catch::Matchers;

// ============================================================================
// Celsius <-> Kelvin
// ============================================================================

TEST_CASE("Celsius to Kelvin: 0 C = 273.15 K", "[temperature]") {
    auto c = Quantity<Celsius>(0.0);
    REQUIRE_THAT(c.to<Kelvin>().value(), WithinAbs(273.15, 1e-9));
}

TEST_CASE("Kelvin to Celsius: 273.15 K = 0 C", "[temperature]") {
    auto k = Quantity<Kelvin>(273.15);
    REQUIRE_THAT(k.to<Celsius>().value(), WithinAbs(0.0, 1e-9));
}

// ============================================================================
// Fahrenheit <-> Kelvin
// ============================================================================

TEST_CASE("Fahrenheit to Kelvin: 32 F = 273.15 K (freezing)", "[temperature]") {
    auto f = Quantity<Fahrenheit>(32.0);
    REQUIRE_THAT(f.to<Kelvin>().value(), WithinAbs(273.15, 1e-6));
}

TEST_CASE("Fahrenheit to Kelvin: 212 F = 373.15 K (boiling)", "[temperature]") {
    auto f = Quantity<Fahrenheit>(212.0);
    REQUIRE_THAT(f.to<Kelvin>().value(), WithinAbs(373.15, 1e-6));
}

TEST_CASE("Kelvin to Fahrenheit: 273.15 K = 32 F", "[temperature]") {
    auto k = Quantity<Kelvin>(273.15);
    REQUIRE_THAT(k.to<Fahrenheit>().value(), WithinAbs(32.0, 1e-6));
}

// ============================================================================
// Fahrenheit <-> Celsius
// ============================================================================

TEST_CASE("Fahrenheit to Celsius: 100 F = 37.7778 C", "[temperature]") {
    auto f = Quantity<Fahrenheit>(100.0);
    REQUIRE_THAT(f.to<Celsius>().value(), WithinAbs(37.7778, 1e-4));
}

TEST_CASE("Celsius to Fahrenheit: 100 C = 212 F", "[temperature]") {
    auto c = Quantity<Celsius>(100.0);
    REQUIRE_THAT(c.to<Fahrenheit>().value(), WithinAbs(212.0, 1e-6));
}

TEST_CASE("Fahrenheit to Celsius: -40 F = -40 C (convergence)", "[temperature]") {
    auto f = Quantity<Fahrenheit>(-40.0);
    REQUIRE_THAT(f.to<Celsius>().value(), WithinAbs(-40.0, 1e-6));
}

// ============================================================================
// Rankine
// ============================================================================

TEST_CASE("Rankine to Kelvin: 491.67 R = 273.15 K", "[temperature]") {
    auto r = Quantity<Rankine>(491.67);
    REQUIRE_THAT(r.to<Kelvin>().value(), WithinAbs(273.15, 1e-6));
}

TEST_CASE("Fahrenheit to Rankine: 32 F = 491.67 R", "[temperature]") {
    auto f = Quantity<Fahrenheit>(32.0);
    REQUIRE_THAT(f.to<Rankine>().value(), WithinAbs(491.67, 1e-3));
}

TEST_CASE("Rankine to Fahrenheit: 0 R = -459.67 F", "[temperature]") {
    auto r = Quantity<Rankine>(0.0);
    REQUIRE_THAT(r.to<Fahrenheit>().value(), WithinAbs(-459.67, 1e-3));
}

// ============================================================================
// Roundtrips (verify no accumulated error)
// ============================================================================

TEST_CASE("Roundtrip: C -> K -> C", "[temperature][roundtrip]") {
    auto orig = Quantity<Celsius>(-40.0);
    auto rt = orig.to<Kelvin>().to<Celsius>();
    REQUIRE_THAT(rt.value(), WithinAbs(-40.0, 1e-9));
}

TEST_CASE("Roundtrip: F -> K -> F", "[temperature][roundtrip]") {
    auto orig = Quantity<Fahrenheit>(-40.0);
    auto rt = orig.to<Kelvin>().to<Fahrenheit>();
    REQUIRE_THAT(rt.value(), WithinAbs(-40.0, 1e-6));
}

TEST_CASE("Roundtrip: F -> C -> F", "[temperature][roundtrip]") {
    auto orig = Quantity<Fahrenheit>(98.6);
    auto rt = orig.to<Celsius>().to<Fahrenheit>();
    REQUIRE_THAT(rt.value(), WithinAbs(98.6, 1e-6));
}

TEST_CASE("Roundtrip: C -> R -> C", "[temperature][roundtrip]") {
    auto orig = Quantity<Celsius>(20.0);
    auto rt = orig.to<Rankine>().to<Celsius>();
    REQUIRE_THAT(rt.value(), WithinAbs(20.0, 1e-6));
}

TEST_CASE("Roundtrip: K -> F -> K", "[temperature][roundtrip]") {
    auto orig = Quantity<Kelvin>(300.0);
    auto rt = orig.to<Fahrenheit>().to<Kelvin>();
    REQUIRE_THAT(rt.value(), WithinAbs(300.0, 1e-6));
}

// ============================================================================
// ISA reference values
// ============================================================================

TEST_CASE("ISA sea level: 288.15 K = 15 C", "[temperature][isa]") {
    auto k = Quantity<Kelvin>(288.15);
    REQUIRE_THAT(k.to<Celsius>().value(), WithinAbs(15.0, 1e-9));
}

TEST_CASE("ISA tropopause: 216.65 K = -56.5 C", "[temperature][isa]") {
    auto k = Quantity<Kelvin>(216.65);
    REQUIRE_THAT(k.to<Celsius>().value(), WithinAbs(-56.5, 1e-9));
}

// ============================================================================
// Temperature arithmetic (same unit)
// ============================================================================

TEST_CASE("Temperature addition (same unit)", "[temperature][arithmetic]") {
    // Note: adding absolute temperatures is physically questionable.
    // This is allowed for simplicity; a future refinement could
    // distinguish absolute vs. relative temperatures (as in Boost.Units).
    auto a = Quantity<Kelvin>(100.0);
    auto b = Quantity<Kelvin>(50.0);
    REQUIRE_THAT((a + b).value(), WithinAbs(150.0, 1e-9));
}

TEST_CASE("Temperature scalar multiplication", "[temperature][arithmetic]") {
    auto k = Quantity<Kelvin>(300.0);
    REQUIRE_THAT((k * 2.0).value(), WithinAbs(600.0, 1e-9));
}

TEST_CASE("Temperature comparison (different units)", "[temperature][comparison]") {
    auto k = Quantity<Kelvin>(273.15);
    auto c = Quantity<Celsius>(0.0);
    REQUIRE(k == c);
    REQUIRE(k > Quantity<Celsius>(-1.0));
    REQUIRE(k < Quantity<Celsius>(1.0));
}