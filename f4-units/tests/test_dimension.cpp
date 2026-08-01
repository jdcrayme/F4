#include <f4/f4_units.hpp>
#include "catch.hpp"

using namespace f4;

// ============================================================================
// Static (compile-time) dimension tests
// ============================================================================

TEST_CASE("same_dimension: identical physical dimensions match", "[dimension]") {
    static_assert(same_dimension_v<LengthDim, LengthDim>);
    static_assert(same_dimension_v<SpeedDim, SpeedDim>);
    static_assert(same_dimension_v<PressureDim, PressureDim>);
    static_assert(same_dimension_v<Dimensionless, Dimensionless>);
    REQUIRE(true);
}

TEST_CASE("same_dimension: different physical dimensions do not match", "[dimension]") {
    static_assert(!same_dimension_v<LengthDim, SpeedDim>);
    static_assert(!same_dimension_v<MassDim, ForceDim>);
    static_assert(!same_dimension_v<PressureDim, EnergyDim>);
    static_assert(!same_dimension_v<LengthDim, AreaDim>);
    static_assert(!same_dimension_v<SpeedDim, AccelerationDim>);
    REQUIRE(true);
}

TEST_CASE("same_dimension: phantom dimensions are not physical dimensions", "[dimension]") {
    static_assert(!same_dimension_v<CASDim, SpeedDim>);
    static_assert(!same_dimension_v<CASDim, PressureDim>);
    static_assert(!same_dimension_v<CASDim, FrequencyDim>);
    static_assert(!same_dimension_v<CASDim, Dimensionless>);
    static_assert(!same_dimension_v<MachDim, Dimensionless>);
    static_assert(!same_dimension_v<MachDim, SpeedDim>);
    REQUIRE(true);
}

TEST_CASE("same_dimension: different phantom tags are distinct", "[dimension]") {
    static_assert(!same_dimension_v<CASDim, MachDim>);
    REQUIRE(true);
}

TEST_CASE("same_dimension: same phantom tags match", "[dimension]") {
    // CASDim is PhantomDimension<CASTag>, so two CASDim should match
    static_assert(same_dimension_v<CASDim, CASDim>);
    static_assert(same_dimension_v<MachDim, MachDim>);
    REQUIRE(true);
}

// ============================================================================
// Dimension arithmetic
// ============================================================================

TEST_CASE("dim_multiply: Speed * Time = Length", "[dimension]") {
    using Result = dim_multiply<SpeedDim, TimeDim>;
    static_assert(Result::length == 1);
    static_assert(Result::mass == 0);
    static_assert(Result::time == 0);
    static_assert(Result::temperature == 0);
    static_assert(Result::angle == 0);
    static_assert(same_dimension_v<Result, LengthDim>);
    REQUIRE(true);
}

TEST_CASE("dim_multiply: Mass * Acceleration = Force", "[dimension]") {
    using Result = dim_multiply<MassDim, AccelerationDim>;
    static_assert(Result::length == 1);
    static_assert(Result::mass == 1);
    static_assert(Result::time == -2);
    static_assert(same_dimension_v<Result, ForceDim>);
    REQUIRE(true);
}

TEST_CASE("dim_multiply: Length * Length = Area", "[dimension]") {
    using Result = dim_multiply<LengthDim, LengthDim>;
    static_assert(Result::length == 2);
    static_assert(Result::mass == 0);
    static_assert(Result::time == 0);
    static_assert(same_dimension_v<Result, AreaDim>);
    REQUIRE(true);
}

TEST_CASE("dim_divide: Force / Area = Pressure", "[dimension]") {
    using Result = dim_divide<ForceDim, AreaDim>;
    static_assert(Result::length == -1);
    static_assert(Result::mass == 1);
    static_assert(Result::time == -2);
    static_assert(same_dimension_v<Result, PressureDim>);
    REQUIRE(true);
}

TEST_CASE("dim_divide: Length / Time = Speed", "[dimension]") {
    using Result = dim_divide<LengthDim, TimeDim>;
    static_assert(Result::length == 1);
    static_assert(Result::time == -1);
    static_assert(same_dimension_v<Result, SpeedDim>);
    REQUIRE(true);
}

TEST_CASE("dim_invert: negates all exponents", "[dimension]") {
    using Result = dim_invert<SpeedDim>;
    static_assert(Result::length == -1);
    static_assert(Result::mass == 0);
    static_assert(Result::time == 1);
    REQUIRE(true);
}

TEST_CASE("physical_dimension concept", "[dimension]") {
    static_assert(physical_dimension<LengthDim>);
    static_assert(physical_dimension<SpeedDim>);
    static_assert(physical_dimension<PressureDim>);
    static_assert(physical_dimension<Dimensionless>);
    static_assert(!physical_dimension<CASDim>);
    static_assert(!physical_dimension<MachDim>);
    REQUIRE(true);
}

TEST_CASE("dim_multiply: density * speed^2 = pressure", "[dimension]") {
    // This is the dimension of dynamic pressure (q = 0.5 * rho * V^2)
    using SpeedSquared = dim_multiply<SpeedDim, SpeedDim>;  // L^2 T^-2
    using Result = dim_multiply<DensityDim, SpeedSquared>;  // L^-1 M^1 T^-2
    static_assert(same_dimension_v<Result, PressureDim>);
    REQUIRE(true);
}