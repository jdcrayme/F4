// f4-flight-api/tests/test_pilot_input.cpp
//
// Unit tests for PilotInput — the per-frame control input struct
// moved from f4-flight-model to f4-flight-api as part of Phase 2+.

#include <gtest/gtest.h>

#include <f4/flight/api/pilot_input.hpp>

using namespace f4::flight;

// ============================================================================
// Default construction
// ============================================================================
TEST(PilotInputTest, DefaultConstructionHasIdleControls) {
    PilotInput pi;
    EXPECT_DOUBLE_EQ(pi.pstick, 0.0);
    EXPECT_DOUBLE_EQ(pi.rstick, 0.0);
    EXPECT_DOUBLE_EQ(pi.ypedal, 0.0);
    EXPECT_DOUBLE_EQ(pi.throttle, 0.0);
    EXPECT_DOUBLE_EQ(pi.speedBrake, -1.0);  // retracted
    EXPECT_DOUBLE_EQ(pi.gearHandle, 1.0);   // down
    EXPECT_DOUBLE_EQ(pi.hookHandle, 0.0);
    EXPECT_DOUBLE_EQ(pi.tefCmd, 0.0);
    EXPECT_DOUBLE_EQ(pi.lefCmd, 0.0);
    EXPECT_FALSE(pi.wheelBrakes);
    EXPECT_FALSE(pi.parkingBrake);
    EXPECT_TRUE(pi.noseSteerOn);
    EXPECT_FALSE(pi.refueling);
}

// ============================================================================
// validate() — clamping
// ============================================================================
TEST(PilotInputTest, ValidateClampsPstick) {
    PilotInput pi;
    pi.pstick = 1.5;
    pi.validate();
    EXPECT_DOUBLE_EQ(pi.pstick, 1.0);

    pi.pstick = -2.0;
    pi.validate();
    EXPECT_DOUBLE_EQ(pi.pstick, -1.0);
}

TEST(PilotInputTest, ValidateClampsThrottle) {
    PilotInput pi;
    pi.throttle = 2.0;
    pi.validate();
    EXPECT_DOUBLE_EQ(pi.throttle, 1.5);

    pi.throttle = -0.5;
    pi.validate();
    EXPECT_DOUBLE_EQ(pi.throttle, 0.0);
}

TEST(PilotInputTest, ValidateClampsSpeedBrake) {
    PilotInput pi;
    pi.speedBrake = 2.0;
    pi.validate();
    EXPECT_DOUBLE_EQ(pi.speedBrake, 1.0);

    pi.speedBrake = -2.0;
    pi.validate();
    EXPECT_DOUBLE_EQ(pi.speedBrake, -1.0);
}

TEST(PilotInputTest, ValidateClampsGearHandle) {
    PilotInput pi;
    pi.gearHandle = 2.0;
    pi.validate();
    EXPECT_DOUBLE_EQ(pi.gearHandle, 1.0);

    pi.gearHandle = -2.0;
    pi.validate();
    EXPECT_DOUBLE_EQ(pi.gearHandle, -1.0);
}

TEST(PilotInputTest, ValidateClampsFlapCommands) {
    PilotInput pi;
    pi.tefCmd = 1.5;
    pi.lefCmd = -0.5;
    pi.validate();
    EXPECT_DOUBLE_EQ(pi.tefCmd, 1.0);
    EXPECT_DOUBLE_EQ(pi.lefCmd, 0.0);
}

TEST(PilotInputTest, ValidateLeavesValidInputsUnchanged) {
    PilotInput pi;
    pi.pstick = 0.5;
    pi.rstick = -0.3;
    pi.ypedal = 0.1;
    pi.throttle = 0.8;
    pi.speedBrake = 0.0;
    pi.gearHandle = -1.0;
    pi.tefCmd = 0.5;
    pi.lefCmd = 0.7;

    const auto copy = pi;
    pi.validate();

    EXPECT_DOUBLE_EQ(pi.pstick, copy.pstick);
    EXPECT_DOUBLE_EQ(pi.rstick, copy.rstick);
    EXPECT_DOUBLE_EQ(pi.ypedal, copy.ypedal);
    EXPECT_DOUBLE_EQ(pi.throttle, copy.throttle);
    EXPECT_DOUBLE_EQ(pi.speedBrake, copy.speedBrake);
    EXPECT_DOUBLE_EQ(pi.gearHandle, copy.gearHandle);
    EXPECT_DOUBLE_EQ(pi.tefCmd, copy.tefCmd);
    EXPECT_DOUBLE_EQ(pi.lefCmd, copy.lefCmd);
}

// ============================================================================
// Value semantics
// ============================================================================
TEST(PilotInputTest, CopyCopiesAllFields) {
    PilotInput pi;
    pi.pstick = 0.7;
    pi.throttle = 1.2;
    pi.wheelBrakes = true;

    auto pi2 = pi;
    EXPECT_DOUBLE_EQ(pi2.pstick, 0.7);
    EXPECT_DOUBLE_EQ(pi2.throttle, 1.2);
    EXPECT_TRUE(pi2.wheelBrakes);
}
