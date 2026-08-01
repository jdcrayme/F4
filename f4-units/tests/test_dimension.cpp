#include <f4/f4_units.hpp>
#include <gtest/gtest.h>

using namespace f4;

// ============================================================================
// Static (compile-time) dimension tests
// ============================================================================

TEST(Dimension, IdenticalPhysicalDimensionsMatch) {
    static_assert(same_dimension_v<LengthDim, LengthDim>);
    static_assert(same_dimension_v<SpeedDim, SpeedDim>);
    static_assert(same_dimension_v<PressureDim, PressureDim>);
    static_assert(same_dimension_v<Dimensionless, Dimensionless>);
    SUCCEED();
}

TEST(Dimension, DifferentPhysicalDimensionsDoNotMatch) {
    static_assert(!same_dimension_v<LengthDim, SpeedDim>);
    static_assert(!same_dimension_v<MassDim, ForceDim>);
    static_assert(!same_dimension_v<PressureDim, EnergyDim>);
    static_assert(!same_dimension_v<LengthDim, AreaDim>);
    static_assert(!same_dimension_v<SpeedDim, AccelerationDim>);
    SUCCEED();
}

TEST(Dimension, PhantomDimensionsAreNotPhysicalDimensions) {
    static_assert(!same_dimension_v<CASDim, SpeedDim>);
    static_assert(!same_dimension_v<CASDim, PressureDim>);
    static_assert(!same_dimension_v<CASDim, FrequencyDim>);
    static_assert(!same_dimension_v<CASDim, Dimensionless>);
    static_assert(!same_dimension_v<MachDim, Dimensionless>);
    static_assert(!same_dimension_v<MachDim, SpeedDim>);
    SUCCEED();
}

TEST(Dimension, DifferentPhantomTagsAreDistinct) {
    static_assert(!same_dimension_v<CASDim, MachDim>);
    SUCCEED();
}

TEST(Dimension, SamePhantomTagsMatch) {
    static_assert(same_dimension_v<CASDim, CASDim>);
    static_assert(same_dimension_v<MachDim, MachDim>);
    SUCCEED();
}

// ============================================================================
// Dimension arithmetic
// ============================================================================

TEST(DimensionMultiply, SpeedTimesTimeEqualsLength) {
    using Result = dim_multiply<SpeedDim, TimeDim>;
    static_assert(Result::length == 1);
    static_assert(Result::mass == 0);
    static_assert(Result::time == 0);
    static_assert(Result::temperature == 0);
    static_assert(Result::angle == 0);
    static_assert(same_dimension_v<Result, LengthDim>);
    SUCCEED();
}

TEST(DimensionMultiply, MassTimesAccelerationEqualsForce) {
    using Result = dim_multiply<MassDim, AccelerationDim>;
    static_assert(Result::length == 1);
    static_assert(Result::mass == 1);
    static_assert(Result::time == -2);
    static_assert(same_dimension_v<Result, ForceDim>);
    SUCCEED();
}

TEST(DimensionMultiply, LengthTimesLengthEqualsArea) {
    using Result = dim_multiply<LengthDim, LengthDim>;
    static_assert(Result::length == 2);
    static_assert(Result::mass == 0);
    static_assert(Result::time == 0);
    static_assert(same_dimension_v<Result, AreaDim>);
    SUCCEED();
}

TEST(DimensionDivide, ForceOverAreaEqualsPressure) {
    using Result = dim_divide<ForceDim, AreaDim>;
    static_assert(Result::length == -1);
    static_assert(Result::mass == 1);
    static_assert(Result::time == -2);
    static_assert(same_dimension_v<Result, PressureDim>);
    SUCCEED();
}

TEST(DimensionDivide, LengthOverTimeEqualsSpeed) {
    using Result = dim_divide<LengthDim, TimeDim>;
    static_assert(Result::length == 1);
    static_assert(Result::time == -1);
    static_assert(same_dimension_v<Result, SpeedDim>);
    SUCCEED();
}

TEST(DimensionInvert, NegatesAllExponents) {
    using Result = dim_invert<SpeedDim>;
    static_assert(Result::length == -1);
    static_assert(Result::mass == 0);
    static_assert(Result::time == 1);
    SUCCEED();
}

TEST(Dimension, PhysicalDimensionConcept) {
    static_assert(physical_dimension<LengthDim>);
    static_assert(physical_dimension<SpeedDim>);
    static_assert(physical_dimension<PressureDim>);
    static_assert(physical_dimension<Dimensionless>);
    static_assert(!physical_dimension<CASDim>);
    static_assert(!physical_dimension<MachDim>);
    SUCCEED();
}

TEST(DimensionMultiply, DensityTimesSpeedSquaredEqualsPressure) {
    using SpeedSquared = dim_multiply<SpeedDim, SpeedDim>;
    using Result = dim_multiply<DensityDim, SpeedSquared>;
    static_assert(same_dimension_v<Result, PressureDim>);
    SUCCEED();
}