// f4-anim/tests/test_rig.cpp
//
// Gear sequencer and blink evaluator tests. Every assertion mirrors a
// concrete FreeFalcon behavior in surface.cpp RunGearSurfaces() /
// RunLightSurfaces() so a regression here is a behavioral drift from
// the reference implementation, not just a broken invariant.

#include <f4/anim/rig.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using namespace f4::anim;

namespace {
constexpr float kDtr = 0.017453293f;  // degrees → radians (FF's DTR)
}

// ── Gear sequencer ────────────────────────────────────────────────────────

namespace {

/// The FF default 3-station layout: 40° legs, 90° doors (generous test
/// ranges; the exact values come from auxaeroData per airframe).
GearStationParams test_params() {
    GearStationParams p;
    p.num_gear = 3;
    for (int i = 0; i < 3; ++i) {
        p.leg_range_rad[i] = 40.f * kDtr;
        p.door_range_rad[i] = 90.f * kDtr;
    }
    return p;
}

} // namespace

TEST(GearSequencer, UpGearParksEverythingShut) {
    const auto p = test_params();
    const auto cmd = eval_gear(0.0f, nullptr, p);
    for (int i = 0; i < 3; ++i) {
        EXPECT_FLOAT_EQ(cmd.door_pos[i], 0.0f);
        EXPECT_FLOAT_EQ(cmd.leg_pos[i], 0.0f);
    }
    // Nothing visible: doors shut, legs retracted into the bays.
    EXPECT_EQ(cmd.door_visible, 0u);
    EXPECT_EQ(cmd.leg_visible, 0u);
    EXPECT_EQ(cmd.hole_visible, 0u);
    EXPECT_EQ(cmd.broken_visible, 0u);
}

TEST(GearSequencer, DoorsMoveBeforeLegs) {
    // The core FreeFalcon choreography: at gearPos 0.25 the doors are
    // half open (0.25*2) but the legs have not started ((0.25-0.5)*2 < 0).
    const auto p = test_params();
    const auto cmd = eval_gear(0.25f, nullptr, p);
    EXPECT_NEAR(cmd.door_pos[0], 0.5f * p.door_range_rad[0], 1e-6f);
    EXPECT_FLOAT_EQ(cmd.leg_pos[0], 0.0f);

    // At gearPos 0.75 the doors are fully open (0.75*2 clamps to 1) and
    // the legs are half extended.
    const auto mid = eval_gear(0.75f, nullptr, p);
    EXPECT_FLOAT_EQ(mid.door_pos[0], p.door_range_rad[0]);
    EXPECT_NEAR(mid.leg_pos[0], 0.5f * p.leg_range_rad[0], 1e-6f);
}

TEST(GearSequencer, DownAndLockedShowsLegsBehindOpenDoors) {
    const auto p = test_params();
    const auto cmd = eval_gear(1.0f, nullptr, p);
    for (int i = 0; i < 3; ++i) {
        EXPECT_FLOAT_EQ(cmd.door_pos[i], p.door_range_rad[i]);
        EXPECT_FLOAT_EQ(cmd.leg_pos[i], p.leg_range_rad[i]);
    }
    EXPECT_EQ(cmd.door_visible, 0b111u);
    EXPECT_EQ(cmd.leg_visible, 0b111u);
    EXPECT_EQ(cmd.hole_visible, 0b111u);
}

TEST(GearSequencer, VisibilityFollowsDofWithThreshold) {
    // A door barely cracked open (below the 5° threshold) must NOT be
    // shown — FreeFalcon drives the visibility switches from the DOF
    // values with hysteresis (surface.cpp:1741-1748).
    auto p = test_params();
    p.door_range_rad[0] = 4.f * kDtr;   // full door travel is sub-threshold
    p.leg_range_rad[0] = 4.f * kDtr;
    const auto cmd = eval_gear(1.0f, nullptr, p);
    EXPECT_FLOAT_EQ(cmd.door_pos[0], 4.f * kDtr);
    EXPECT_EQ(cmd.door_visible & 1u, 0u);
    EXPECT_EQ(cmd.leg_visible & 1u, 0u);
}

TEST(GearSequencer, BrokenLegParksAtSixtyPercent) {
    // surface.cpp:1717-1722: a broken/stuck leg parks at range * 0.6.
    auto p = test_params();
    GearStationFlags flags[3];
    flags[1].gear_broken = true;
    const auto cmd = eval_gear(1.0f, flags, p);
    EXPECT_FLOAT_EQ(cmd.leg_pos[1], p.leg_range_rad[1] * 0.6f);
    EXPECT_EQ(cmd.leg_visible & (1u << 1), 1u << 1);
    EXPECT_EQ(cmd.broken_visible & (1u << 1), 1u << 1);
    // The other stations are healthy.
    EXPECT_FLOAT_EQ(cmd.leg_pos[0], p.leg_range_rad[0]);
}

TEST(GearSequencer, BrokenDoorParksOpenWithHole) {
    auto p = test_params();
    GearStationFlags flags[3];
    flags[2].door_stuck = true;
    const auto cmd = eval_gear(1.0f, flags, p);
    EXPECT_FLOAT_EQ(cmd.door_pos[2], p.door_range_rad[2]);
    EXPECT_EQ(cmd.hole_visible & (1u << 2), 1u << 2);
    EXPECT_EQ(cmd.door_visible & (1u << 2), 1u << 2);
}

TEST(GearSequencer, BrokenOverridesAreArmedNearFullExtension) {
    // The broken variants only appear once gearPos >= 0.9
    // (surface.cpp:1724-1738) — mid-travel the healthy variant runs.
    auto p = test_params();
    GearStationFlags flags[3];
    flags[0].gear_broken = true;
    const auto early = eval_gear(0.75f, flags, p);
    EXPECT_EQ(early.broken_visible & 1u, 0u);
    const auto late = eval_gear(0.95f, flags, p);
    EXPECT_EQ(late.broken_visible & 1u, 1u);
}

TEST(GearSequencer, StationsBeyondNumGearStayInert) {
    auto p = test_params();  // num_gear = 3
    const auto cmd = eval_gear(1.0f, nullptr, p);
    for (int i = 3; i < 8; ++i) {
        EXPECT_EQ(cmd.door_visible & (1u << i), 0u);
        EXPECT_EQ(cmd.leg_visible & (1u << i), 0u);
    }
}

TEST(GearSequencer, ApplyWritesChannelGroups) {
    const auto p = test_params();
    const auto cmd = eval_gear(1.0f, nullptr, p);
    AnimValues v;
    apply_gear_command(cmd, v);
    EXPECT_FLOAT_EQ(v[Channel::gear_leg_pos_0], p.leg_range_rad[0]);
    EXPECT_FLOAT_EQ(v[Channel::gear_door_pos_2], p.door_range_rad[2]);
    EXPECT_FLOAT_EQ(v[Channel::sw_gear_leg_0], 1.0f);
    EXPECT_FLOAT_EQ(v[Channel::sw_gear_door_1], 1.0f);
    EXPECT_FLOAT_EQ(v[Channel::sw_gear_hole_0], 1.0f);
    EXPECT_FLOAT_EQ(v[Channel::sw_gear_broken_0], 0.0f);
}

TEST(GearSequencer, IsDeterministic) {
    const auto p = test_params();
    const auto a = eval_gear(0.62f, nullptr, p);
    const auto b = eval_gear(0.62f, nullptr, p);
    EXPECT_EQ(a.leg_visible, b.leg_visible);
    for (int i = 0; i < 8; ++i) {
        EXPECT_FLOAT_EQ(a.leg_pos[i], b.leg_pos[i]);
        EXPECT_FLOAT_EQ(a.door_pos[i], b.door_pos[i]);
    }
}

// ── Blink patterns ────────────────────────────────────────────────────────

TEST(BlinkPattern, NavPatternMatchesFreeFalconDutyCycle) {
    // FreeFalcon: animWingFlashOnTime 0.4 s, OffTime 0.5 s.
    const auto nav = BlinkPattern::nav(0);
    EXPECT_FLOAT_EQ(nav.on_seconds, 0.4f);
    EXPECT_FLOAT_EQ(nav.off_seconds, 0.5f);
}

TEST(BlinkPattern, StrobePatternMatchesFreeFalconDutyCycle) {
    // FreeFalcon: animStrobeOnTime 0.08 s, OffTime 2.0 s.
    const auto strobe = BlinkPattern::strobe(0);
    EXPECT_NEAR(strobe.on_seconds, 0.08f, 1e-6f);
    EXPECT_NEAR(strobe.off_seconds, 2.0f, 1e-6f);
}

TEST(BlinkPattern, IsOnThenOffWithinOneCycle) {
    const auto nav = BlinkPattern::nav(42);
    const float period = nav.on_seconds + nav.off_seconds;

    // Sample the waveform and measure the on-fraction over one period
    // starting from a phase-aligned time (t where the pattern is on).
    // We don't know the seed-derived phase; instead verify that over a
    // long window the duty cycle ratio approaches on/period.
    const double dt = 0.001;
    long on_samples = 0;
    long total = 0;
    for (double t = 0.0; t < 100.0; t += dt) {
        ++total;
        if (eval_blink(nav, t) > 0.5f) ++on_samples;
    }
    const double measured = static_cast<double>(on_samples) / total;
    const double expected =
        static_cast<double>(nav.on_seconds) / period;
    EXPECT_NEAR(measured, expected, 0.02);
}

TEST(BlinkPattern, IsDeterministicPerSeed) {
    const auto nav = BlinkPattern::nav(7);
    for (double t = 0.0; t < 20.0; t += 0.017) {
        ASSERT_EQ(eval_blink(nav, t), eval_blink(nav, t));
    }
}

TEST(BlinkPattern, DifferentSeedsDesynchronize) {
    // Formation lights must not blink in unison: with different seeds
    // the waveforms differ somewhere within the first few cycles.
    const auto a = BlinkPattern::strobe(1);
    const auto b = BlinkPattern::strobe(2);
    bool differ = false;
    for (double t = 0.0; t < 5.0; t += 0.01) {
        if (eval_blink(a, t) != eval_blink(b, t)) {
            differ = true;
            break;
        }
    }
    EXPECT_TRUE(differ);
}

TEST(BlinkPattern, PhasesShiftAsTimeAdvances) {
    // The output is a square wave: within one on-window every sample is
    // 1, within one off-window every sample is 0.
    const auto nav = BlinkPattern::nav(3);
    bool saw_on = false, saw_off = false;
    float last = -1.0f;
    for (double t = 0.0; t < 2.0; t += 0.01) {
        const float v = eval_blink(nav, t);
        ASSERT_TRUE(v == 0.0f || v == 1.0f);
        if (v > 0.5f) saw_on = true;
        else saw_off = true;
        last = v;
    }
    EXPECT_TRUE(saw_on);
    EXPECT_TRUE(saw_off);
}

// ── Spin integrator ───────────────────────────────────────────────────────

TEST(SpinIntegrator, AccumulatesByRateTimesDt) {
    EXPECT_NEAR(integrate_spin(0.0f, 10.0f, 0.25), 2.5f, 1e-5f);
}

TEST(SpinIntegrator, WrapsAtTwoPi) {
    const float a = integrate_spin(6.0f, 1.0f, 1.0);  // 7 → 7 - 2π
    EXPECT_NEAR(a, 7.0f - 6.283185307179586f, 1e-5f);
    EXPECT_GE(a, 0.0f);
    EXPECT_LT(a, 6.283185307179586f);
}

TEST(SpinIntegrator, HandlesNegativeRates) {
    const float a = integrate_spin(0.5f, -1.0f, 1.0);  // -0.5 → 2π - 0.5
    EXPECT_NEAR(a, 6.283185307179586f - 0.5f, 1e-5f);
}

TEST(SpinIntegrator, LongRunStaysBounded) {
    float angle = 0.0f;
    for (int i = 0; i < 100000; ++i) {
        angle = integrate_spin(angle, 50.0f, 0.016);
        ASSERT_GE(angle, 0.0f);
        ASSERT_LT(angle, 6.283185307179586f);
    }
}

// ── Spinners (rotors / radar dishes) ──────────────────────────────────────

TEST(Spinners, SeedIsDeterministicAndPhaseSeparates) {
    AnimValues a, b;
    seed_spinners(12345u, a);
    seed_spinners(12345u, b);
    // Deterministic: the same seed seeds the same phases.
    EXPECT_EQ(a[Channel::rotor_main], b[Channel::rotor_main]);
    EXPECT_EQ(a[Channel::radar_dish_spin], b[Channel::radar_dish_spin]);

    // Adjacent entity ids must not spin in lockstep.
    seed_spinners(12346u, b);
    EXPECT_NE(a[Channel::rotor_main], b[Channel::rotor_main]);

    // All seeded channels land in [0, 2π).
    for (const Channel c : {Channel::rotor_main, Channel::rotor_tail,
                            Channel::radar_dish_spin}) {
        EXPECT_GE(a[c], 0.0f);
        EXPECT_LT(a[c], 6.283185307179586f);
    }
}

TEST(Spinners, IntegrateAdvancesAndWraps) {
    AnimValues v;
    seed_spinners(7u, v);
    const float before = v[Channel::rotor_main];
    integrate_spinner(Channel::rotor_main, 10.0f, 0.1, v);
    EXPECT_NEAR(v[Channel::rotor_main],
                std::fmod(before + 1.0f, 6.283185307179586f), 1e-5f);
}

TEST(Spinners, IntegrateStoppedRateHoldsAngle) {
    AnimValues v;
    v[Channel::radar_dish_spin] = 1.234f;
    integrate_spinner(Channel::radar_dish_spin, 0.0f, 0.5, v);
    integrate_spinner(Channel::radar_dish_spin, -2.0f, 0.5, v);
    EXPECT_FLOAT_EQ(v[Channel::radar_dish_spin], 1.234f);
}

// ── Pilot surfaces ────────────────────────────────────────────────────────

TEST(SurfaceCommand, StabsAreSymmetricAndFollowPitch) {
    AnimValues v;
    SurfaceCommand cmd;
    cmd.pitch_stick = 0.5f;   // pull
    apply_surface_command(cmd, v);
    // Both stabs deflect identically (symmetric pair, identical frames).
    EXPECT_NEAR(v[Channel::stab_l], v[Channel::stab_r], 1e-6f);
    // Pull = trailing edge up = negative deflection.
    EXPECT_LT(v[Channel::stab_l], 0.0f);
    // 0.5 stick → half of the 25 deg max.
    EXPECT_NEAR(v[Channel::stab_l],
                -0.5f * 25.0f * 0.017453292519943295f, 1e-5f);
}

TEST(SurfaceCommand, FlaperonsScheduleCommonPlusRollDifferential) {
    AnimValues v;
    SurfaceCommand cmd;
    cmd.tef = 1.0f;          // full droop
    cmd.roll_stick = 0.0f;
    apply_surface_command(cmd, v);
    // Pure schedule: both sides deflect the same amount.
    EXPECT_NEAR(v[Channel::flap_l], v[Channel::flap_r], 1e-6f);
    EXPECT_NEAR(v[Channel::flap_l], 20.0f * 0.017453292519943295f, 1e-5f);

    cmd.roll_stick = 1.0f;   // full right roll
    apply_surface_command(cmd, v);
    // Differential: left up, right down by the roll amount.
    EXPECT_GT(v[Channel::flap_l], v[Channel::flap_r]);
    EXPECT_NEAR(v[Channel::flap_l] - v[Channel::flap_r],
                2.0f * 15.0f * 0.017453292519943295f, 1e-5f);
}

TEST(SurfaceCommand, BrakeDrivesAllFourPanels) {
    AnimValues v;
    SurfaceCommand cmd;
    cmd.brake = 0.5f;
    apply_surface_command(cmd, v);
    EXPECT_NEAR(v[Channel::airbrake_top_l],
                0.5f * 55.0f * 0.017453292519943295f, 1e-5f);
    EXPECT_FLOAT_EQ(v[Channel::airbrake_top_l], v[Channel::airbrake_bot_l]);
    EXPECT_FLOAT_EQ(v[Channel::airbrake_top_l], v[Channel::airbrake_top_r]);
    EXPECT_FLOAT_EQ(v[Channel::airbrake_top_l], v[Channel::airbrake_bot_r]);
}

TEST(SurfaceCommand, AfterburnerLightsPlume) {
    AnimValues v;
    SurfaceCommand cmd;
    cmd.afterburner = true;
    apply_surface_command(cmd, v);
    EXPECT_FLOAT_EQ(v[Channel::sw_ab], 1.0f);
    EXPECT_FLOAT_EQ(v[Channel::ab_scale], 1.0f);
    cmd.afterburner = false;
    apply_surface_command(cmd, v);
    EXPECT_FLOAT_EQ(v[Channel::sw_ab], 0.0f);
    EXPECT_FLOAT_EQ(v[Channel::ab_scale], 0.0f);
}
