// f4-flight-model/tests/test_gear.cpp
//
// Unit tests for GearModel — the gear/ground model.
//
// Focuses on the branches that the integration tests don't exercise:
//   - calcMuFric: the 5-branch friction coefficient selector
//   - updateGearPos: constant-rate actuation with snap-to-target
//   - updateStrutCompression: per-wheel compression from AGL
//   - computeMinHeight: gear-down clearance

#include "f4/flight/gear.hpp"
#include "f4/flight/constants.hpp"

#include <gtest/gtest.h>

#include <cmath>

namespace f4::flight {
namespace {

// Minimal geometry for gear tests: 3 gear points (nose, left main, right main).
data::AircraftGeometry makeGeom() {
    data::AircraftGeometry g;
    g.emptyWeight_lbs = 10000.0;
    g.gear = {
        { /*x=*/10.0, /*y=*/0.0,  /*z=*/3.0 },   // nose
        { /*x=*/-2.0, /*y=*/5.0,  /*z=*/4.0 },   // left main
        { /*x=*/-2.0, /*y=*/-5.0, /*z=*/4.0 },   // right main
    };
    return g;
}

data::AuxAero makeAux() {
    data::AuxAero a;
    return a;
}

}  // namespace

// ============================================================================
// calcMuFric — the 5-branch friction selector
// ============================================================================

TEST(GearModelCalcMuFric, OnObjectReturnsHighFriction) {
    // Carrier deck / hard surface → effectively infinite friction.
    EXPECT_DOUBLE_EQ(GearModel::calcMuFric(false, false, true, false), 20.0);
    // onObject takes priority over all other flags.
    EXPECT_DOUBLE_EQ(GearModel::calcMuFric(true,  true,  true, true),  20.0);
}

TEST(GearModelCalcMuFric, ParkingBrakeReturns07) {
    EXPECT_DOUBLE_EQ(GearModel::calcMuFric(false, true, false, false), 0.7);
    EXPECT_DOUBLE_EQ(GearModel::calcMuFric(false, true, false, true),  0.7);
}

TEST(GearModelCalcMuFric, WheelBrakesReturns07) {
    EXPECT_DOUBLE_EQ(GearModel::calcMuFric(true, false, false, false), 0.7);
    EXPECT_DOUBLE_EQ(GearModel::calcMuFric(true, false, false, true),  0.7);
}

TEST(GearModelCalcMuFric, OverRunwayReturns004) {
    EXPECT_DOUBLE_EQ(GearModel::calcMuFric(false, false, false, true), 0.04);
}

TEST(GearModelCalcMuFric, GrassReturns05) {
    EXPECT_DOUBLE_EQ(GearModel::calcMuFric(false, false, false, false), 0.5);
}

TEST(GearModelCalcMuFric, PriorityOrderIsObjectThenParkingThenBrakesThenRunway) {
    // The if/else chain defines a strict priority. Verify each level overrides
    // the ones below it.
    EXPECT_DOUBLE_EQ(GearModel::calcMuFric(/*brakes=*/true, /*park=*/true,  /*obj=*/true,  /*rwy=*/false), 20.0);
    EXPECT_DOUBLE_EQ(GearModel::calcMuFric(/*brakes=*/true, /*park=*/true,  /*obj=*/false, /*rwy=*/false), 0.7);
    EXPECT_DOUBLE_EQ(GearModel::calcMuFric(/*brakes=*/true, /*park=*/false, /*obj=*/false, /*rwy=*/false), 0.7);
    EXPECT_DOUBLE_EQ(GearModel::calcMuFric(/*brakes=*/false,/*park=*/false, /*obj=*/false, /*rwy=*/true),  0.04);
    EXPECT_DOUBLE_EQ(GearModel::calcMuFric(/*brakes=*/false,/*park=*/false, /*obj=*/false, /*rwy=*/false), 0.5);
}

// ============================================================================
// updateGearPos — constant-rate actuation
// ============================================================================

TEST(GearModelUpdateGearPos, RetractsTowardZero) {
    data::AircraftGeometry g = makeGeom();
    data::AuxAero a = makeAux();
    GearModel gm(&g, &a);

    double pos = 1.0;  // gear down
    double handle = -1.0;  // retract

    // GEAR_RATE = 1/3 per second. After 1.0s, pos should move from 1.0 to ~0.667.
    double result = gm.updateGearPos(pos, handle, 1.0);
    EXPECT_NEAR(result, 1.0 - (1.0/3.0), 1e-9);
    EXPECT_NEAR(pos,    1.0 - (1.0/3.0), 1e-9);
}

TEST(GearModelUpdateGearPos, ExtendsTowardOne) {
    data::AircraftGeometry g = makeGeom();
    data::AuxAero a = makeAux();
    GearModel gm(&g, &a);

    double pos = 0.0;  // gear up
    double handle = 1.0;  // extend

    double result = gm.updateGearPos(pos, handle, 1.0);
    EXPECT_NEAR(result, 1.0/3.0, 1e-9);
}

TEST(GearModelUpdateGearPos, SnapsToTargetWhenClose) {
    data::AircraftGeometry g = makeGeom();
    data::AuxAero a = makeAux();
    GearModel gm(&g, &a);

    double pos = 0.99;  // very close to 1.0
    double handle = 1.0;
    // step = 1/3 * 0.5 = 0.1667, diff = 0.01 < step → snap
    double result = gm.updateGearPos(pos, handle, 0.5);
    EXPECT_DOUBLE_EQ(result, 1.0);
    EXPECT_DOUBLE_EQ(pos,    1.0);
}

TEST(GearModelUpdateGearPos, FullTravelTakesThreeSeconds) {
    data::AircraftGeometry g = makeGeom();
    data::AuxAero a = makeAux();
    GearModel gm(&g, &a);

    double pos = 0.0;
    // Step in 0.5s increments for 3.0s total.
    for (int i = 0; i < 6; ++i) {
        gm.updateGearPos(pos, 1.0, 0.5);
    }
    EXPECT_NEAR(pos, 1.0, 1e-9) << "full travel should complete in 3 seconds";
}

TEST(GearModelUpdateGearPos, NegativeHandleRetracts) {
    data::AircraftGeometry g = makeGeom();
    data::AuxAero a = makeAux();
    GearModel gm(&g, &a);

    double pos = 0.5;
    gm.updateGearPos(pos, -0.5, 0.3);  // any negative value retracts
    EXPECT_LT(pos, 0.5);
}

// ============================================================================
// computeMinHeight — gear-down clearance
// ============================================================================

TEST(GearModelComputeMinHeight, ReturnsZeroWhenGearUp) {
    data::AircraftGeometry g = makeGeom();
    data::AuxAero a = makeAux();
    GearModel gm(&g, &a);

    GearState gear;
    // gearPos = 0 → gear up → no clearance.
    double h = gm.computeMinHeight(gear, 0.0);
    EXPECT_DOUBLE_EQ(h, 0.0);
}

TEST(GearModelComputeMinHeight, ReturnsMaxStrutWhenGearDown) {
    // computeMinHeight returns the MAXIMUM strut length (the lowest point
    // of the aircraft = the gear that touches the ground first). With gear
    // points z = {3.0, 4.0, 4.0}, the max is 4.0.
    data::AircraftGeometry g = makeGeom();
    data::AuxAero a = makeAux();
    GearModel gm(&g, &a);

    GearState gear;
    double h = gm.computeMinHeight(gear, 1.0);
    EXPECT_NEAR(h, 4.0, 1e-9);
}

TEST(GearModelComputeMinHeight, ScalesLinearlyWithGearPos) {
    data::AircraftGeometry g = makeGeom();
    data::AuxAero a = makeAux();
    GearModel gm(&g, &a);

    GearState gear;
    // gearPos = 0.5 → half of max strut (4.0).
    double h = gm.computeMinHeight(gear, 0.5);
    EXPECT_NEAR(h, 2.0, 1e-9);
}

// ============================================================================
// updateStrutCompression — per-wheel compression from AGL
// ============================================================================

TEST(GearModelUpdateStrutCompression, CompressesWhenOnGround) {
    data::AircraftGeometry g = makeGeom();
    data::AuxAero a = makeAux();
    GearModel gm(&g, &a);

    GearState gear;
    gm.init(gear);
    ASSERT_EQ(gear.wheels.size(), 3u);

    // Aircraft Z = 0 (on the ground), ground Z = 0.
    // AGL = |0 - 0| = 0. Strut max = 3.0 (nose), 4.0 (mains).
    // compression = strutMax - AGL = 3.0 / 4.0.
    gm.updateStrutCompression(gear, /*groundZ=*/0.0, /*z=*/0.0, /*vt=*/0.0, /*dt=*/0.1);
    EXPECT_NEAR(gear.wheels[0].strutCompression_ft, 3.0, 1e-9);
    EXPECT_NEAR(gear.wheels[1].strutCompression_ft, 4.0, 1e-9);
    EXPECT_NEAR(gear.wheels[2].strutCompression_ft, 4.0, 1e-9);
    EXPECT_TRUE(gear.wheels[0].onGround);
    EXPECT_TRUE(gear.wheels[1].onGround);
    EXPECT_TRUE(gear.wheels[2].onGround);
}

TEST(GearModelUpdateStrutCompression, NoCompressionWhenAirborne) {
    data::AircraftGeometry g = makeGeom();
    data::AuxAero a = makeAux();
    GearModel gm(&g, &a);

    GearState gear;
    gm.init(gear);

    // AGL = |0 - (-100)| = 100 ft. Well above strut max.
    gm.updateStrutCompression(gear, /*groundZ=*/0.0, /*z=*/-100.0, /*vt=*/0.0, /*dt=*/0.1);
    for (const auto& w : gear.wheels) {
        EXPECT_DOUBLE_EQ(w.strutCompression_ft, 0.0);
        EXPECT_FALSE(w.onGround);
    }
}

TEST(GearModelUpdateStrutCompression, CompressionClampedToLowerBound) {
    // AGL = fabs(groundZ - z). When AGL > strutMax, compression goes negative
    // and is clamped to 0.
    data::AircraftGeometry g = makeGeom();
    data::AuxAero a = makeAux();
    GearModel gm(&g, &a);

    GearState gear;
    gm.init(gear);

    // z = -100 (100 ft AGL). strutMax = 3 (nose). compression = 3 - 100 = -97 → clamped to 0.
    gm.updateStrutCompression(gear, /*groundZ=*/0.0, /*z=*/-100.0, /*vt=*/0.0, /*dt=*/0.1);
    EXPECT_DOUBLE_EQ(gear.wheels[0].strutCompression_ft, 0.0);
    EXPECT_FALSE(gear.wheels[0].onGround);
}

TEST(GearModelUpdateStrutCompression, WheelAngleIntegratesWithSpeed) {
    data::AircraftGeometry g = makeGeom();
    data::AuxAero a = makeAux();
    GearModel gm(&g, &a);

    GearState gear;
    gm.init(gear);

    // Small dt so the angle doesn't wrap.
    gm.updateStrutCompression(gear, 0.0, 0.0, /*vt=*/10.0, /*dt=*/0.01);
    // wheelRadius = max(0.5, strutMax * 0.3) = max(0.5, 0.9) = 0.9
    // delta_angle = vt * dt / wheelRadius = 10 * 0.01 / 0.9 ≈ 0.111 rad
    double expected_delta = 10.0 * 0.01 / 0.9;
    double actual_delta = gear.wheels[0].wheelAngle_rad;
    EXPECT_NEAR(actual_delta, expected_delta, 0.01);
}

// ============================================================================
// init — sizes the wheels vector
// ============================================================================

TEST(GearModelInit, SizesWheelsToConfigGearCount) {
    data::AircraftGeometry g = makeGeom();
    data::AuxAero a = makeAux();
    GearModel gm(&g, &a);

    GearState gear;
    gm.init(gear);
    EXPECT_EQ(gear.wheels.size(), g.gear.size());
}

TEST(GearModelInit, DoesNotResizeIfAlreadyCorrect) {
    data::AircraftGeometry g = makeGeom();
    data::AuxAero a = makeAux();
    GearModel gm(&g, &a);

    GearState gear;
    gear.wheels.resize(3);
    gm.init(gear);
    EXPECT_EQ(gear.wheels.size(), 3u);
}

}  // namespace f4::flight
