// test_strike_module.cpp — the A-G release fire control (M5 slice).
//
// The module is a pure decision: given the aircraft state + the aim point,
// pulse the release exactly when the PREDICTED IMPACT POINT (CCIP pipper)
// falls within tolerance of the aim — a ballistic solve (throw = ground
// speed x fall time, sink-aware, drag-scaled) projected along the
// aircraft's heading. Plus the salvo pacer (committed stick, interval,
// max, abort on target loss) and the ROE gate. No world, no weapons.
//
// Test geometry convention: the aircraft flies NORTH (heading 0) toward
// the aim; "pipper on" = positioned so aim == position + throw * north.

#include "f4/ai/modules/strike_module.hpp"

#include <gtest/gtest.h>

#include <cmath>

using namespace f4::ai::modules;

namespace {

constexpr double DT = 1.0 / 60.0;

class TestAircraftState : public f4::flight::IAircraftState {
public:
    double east_ft{0.0};
    double north_ft{0.0};
    double alt_msl_ft{5000.0};
    double gs_fps_{675.0};     // ~400 kts
    double vcas_kts_{400.0};
    double heading_rad_{0.0};  // north by default (flying toward the aim)
    double vs_fpm_{0.0};

    double position_east_ft()  const override { return east_ft; }
    double position_north_ft() const override { return north_ft; }
    double altitude_msl_ft()   const override { return alt_msl_ft; }
    double altitude_agl_ft()   const override { return alt_msl_ft; }
    double vcas_kts()          const override { return vcas_kts_; }
    double ground_speed_fps()  const override { return gs_fps_; }
    double heading_rad()       const override { return heading_rad_; }
    double pitch_angle_rad()   const override { return 0.0; }
    double roll_angle_rad()    const override { return 0.0; }
    double roll_rate_radps()   const override { return 0.0; }
    double pitch_rate_radps()  const override { return 0.0; }
    double yaw_rate_radps()    const override { return 0.0; }
    double vertical_speed_fpm() const override { return vs_fpm_; }
    bool   on_ground()         const override { return false; }
    double fuel_lbs()          const override { return 5000.0; }

    /// Closed-form vacuum release range for the current geometry.
    double vacuum_throw_ft(double dz) const {
        return gs_fps_ * std::sqrt(2.0 * dz /
                                   StrikeModule::Config::kGravityFps2);
    }
};

/// The aim point: 20,000 ft north of the origin, on the ground.
constexpr double kAimNorth = 20000.0;
const f4::geo::WorldPosition kAim{0.0, kAimNorth, 0.0};

} // anonymous namespace

// ============================================================================
// The trigger geometry (CCIP)
// ============================================================================

TEST(StrikeFireControl, NoPulseOutsideTheEnvelope) {
    TestAircraftState own;
    StrikeModule sm;
    sm.set_target(42);
    // 20,000 ft south of the aim with a ~10,100 ft throw: the predicted
    // impact sits ~9,900 ft short of the aim — pipper off, no release.
    sm.update(DT, &own, kAim, true);
    EXPECT_FALSE(sm.release_pulse());
    EXPECT_TRUE(sm.armed());
    EXPECT_GT(sm.computed_release_range_ft(), 9500.0);
    EXPECT_LT(sm.computed_release_range_ft(), 11000.0);
    EXPECT_GT(sm.predicted_miss_ft(), 9000.0);
}

TEST(StrikeFireControl, PulsesWhenPipperIsOn) {
    TestAircraftState own;
    StrikeModule sm;
    sm.config.drag_factor = 1.0;
    sm.set_target(42);
    // Position so aim == position + throw (vacuum): the pipper is exactly
    // on the target.
    const double throw_ft = own.vacuum_throw_ft(5000.0);
    own.north_ft = kAimNorth - throw_ft;
    sm.update(DT, &own, kAim, true);
    EXPECT_TRUE(sm.release_pulse());
    EXPECT_EQ(sm.release_target_id(), 42u);
    EXPECT_EQ(sm.salvo_fired(), 1);
    EXPECT_LE(sm.predicted_miss_ft(), 10.0);
}

TEST(StrikeFireControl, MisalignedAircraftHoldsTheRelease) {
    // The target is inside the throw range but 60 deg off the nose — the
    // first TestCamp A-G QC failure mode: the bomb sails thousands of
    // feet wide. The CCIP gate must hold until the aircraft points at the
    // target.
    TestAircraftState own;
    StrikeModule sm;
    sm.config.drag_factor = 1.0;
    sm.set_target(42);
    const double throw_ft = own.vacuum_throw_ft(5000.0);
    // 60 deg off: the aircraft is at the range where a straight-in
    // solution exists, but pointed 60 deg away.
    own.north_ft = kAimNorth - throw_ft;
    own.east_ft = 0.0;
    own.heading_rad_ = 60.0 * 3.14159265358979323846 / 180.0;
    sm.update(DT, &own, kAim, true);
    EXPECT_FALSE(sm.release_pulse());
    EXPECT_GT(sm.predicted_miss_ft(),
              0.5 * throw_ft);   // pipper far off-target
}

TEST(StrikeFireControl, SinkRateShortensTheThrow) {
    // A 3,000 fpm sink hands the bomb initial downward velocity: the
    // fall time shortens, and the trigger must solve the quadratic (the
    // level form would release ~1,000 ft early — the first TestCamp
    // stick's systematic miss).
    TestAircraftState own;
    StrikeModule sm;
    sm.config.drag_factor = 1.0;
    sm.set_target(42);
    own.vs_fpm_ = -3000.0;   // descending
    const double w = 50.0;   // 3000 fpm = 50 ft/s down
    const double fall = (-w + std::sqrt(w * w + 2.0 * 32.174 * 5000.0)) /
                        32.174;
    const double throw_ft = 675.0 * fall;
    own.north_ft = kAimNorth - throw_ft;
    sm.update(DT, &own, kAim, true);
    EXPECT_TRUE(sm.release_pulse());
    EXPECT_NEAR(sm.predicted_miss_ft(), 0.0, 10.0);
    // And materially shorter than the level-release throw.
    EXPECT_LT(sm.computed_release_range_ft(),
              own.vacuum_throw_ft(5000.0) - 800.0);
}

TEST(StrikeFireControl, DragFactorShrinksTheThrow) {
    TestAircraftState own;
    StrikeModule vacuum, dragged;
    vacuum.config.drag_factor = 1.0;
    dragged.config.drag_factor = 0.85;
    vacuum.set_target(42);
    dragged.set_target(42);
    own.north_ft = 0.0;
    vacuum.update(DT, &own, kAim, true);
    dragged.update(DT, &own, kAim, true);
    EXPECT_NEAR(dragged.computed_release_range_ft(),
                0.85 * vacuum.computed_release_range_ft(), 1.0);
}

TEST(StrikeFireControl, MinReleaseAglGatesTheTrigger) {
    TestAircraftState own;
    own.alt_msl_ft = 400.0;   // below the 500 ft floor over a z=0 aim
    StrikeModule sm;
    sm.set_target(42);
    own.north_ft = kAimNorth;   // directly overhead — any throw lands on
    sm.update(DT, &own, kAim, true);
    EXPECT_FALSE(sm.release_pulse());
    EXPECT_DOUBLE_EQ(sm.computed_release_range_ft(), 0.0);
}

TEST(StrikeFireControl, HoldFireNeverPulses) {
    TestAircraftState own;
    StrikeModule sm;
    sm.config.hold_fire = true;
    sm.set_target(42);
    sm.config.drag_factor = 1.0;
    own.north_ft = kAimNorth - own.vacuum_throw_ft(5000.0);
    sm.update(DT, &own, kAim, true);
    EXPECT_FALSE(sm.release_pulse());
    // The gate blocks the pulse BEFORE the shot is counted (the module
    // contract: hold at the fire control, never count phantom shots).
    EXPECT_EQ(sm.salvo_fired(), 0);
}

// ============================================================================
// The salvo pacer (committed stick)
// ============================================================================

TEST(StrikeFireControl, SalvoSpacesReleasesAndStopsAtLimit) {
    TestAircraftState own;
    StrikeModule sm;
    sm.config.drag_factor = 1.0;
    sm.config.salvo_interval_s = 0.5;    // 30 ticks at 60 Hz
    sm.config.salvo_max = 3;
    sm.set_target(42);
    // Start with the pipper exactly on; the aircraft creeps north ~11.8
    // ft/tick, walking the pipper through the aim.
    own.north_ft = kAimNorth - own.vacuum_throw_ft(5000.0);

    sm.update(DT, &own, kAim, true);
    EXPECT_TRUE(sm.release_pulse());      // first bomb: pipper on
    EXPECT_EQ(sm.salvo_fired(), 1);

    // The next 29 ticks: pacing, no pulse (the pipper has walked past
    // the aim by ~100 ft — still inside the 150 ft tolerance, but the
    // stick is COMMITTED anyway: real doctrine drops the whole stick at
    // fixed intervals once the pickle is pressed).
    for (int i = 0; i < 29; ++i) {
        sm.update(DT, &own, kAim, true);
        EXPECT_FALSE(sm.release_pulse()) << "tick " << i;
    }
    // Tick 30: the second release.
    sm.update(DT, &own, kAim, true);
    EXPECT_TRUE(sm.release_pulse());
    EXPECT_EQ(sm.salvo_fired(), 2);

    // Jump to the third release, then the stick is complete.
    for (int i = 0; i < 30; ++i) sm.update(DT, &own, kAim, true);
    EXPECT_EQ(sm.salvo_fired(), 3);
    EXPECT_TRUE(sm.delivered());
    // Further updates never pulse again — even well past the target.
    own.north_ft = kAimNorth + 5000.0;
    for (int i = 0; i < 120; ++i) {
        sm.update(DT, &own, kAim, true);
        EXPECT_FALSE(sm.release_pulse());
    }
}

TEST(StrikeFireControl, TargetLossAbortsTheStick) {
    TestAircraftState own;
    StrikeModule sm;
    sm.config.drag_factor = 1.0;
    sm.set_target(42);
    own.north_ft = kAimNorth - own.vacuum_throw_ft(5000.0);
    sm.update(DT, &own, kAim, true);      // bomb 1 away
    EXPECT_EQ(sm.salvo_fired(), 1);
    // The target dies mid-stick: aim goes invalid.
    sm.update(DT, &own, kAim, false);
    EXPECT_TRUE(sm.delivered());
    EXPECT_FALSE(sm.armed());
    // Re-validating the aim does NOT resume (delivered sticks to the
    // target; only a NEW target id resets — see set_target).
    sm.update(DT, &own, kAim, true);
    EXPECT_FALSE(sm.release_pulse());
}

TEST(StrikeFireControl, NewTargetResetsTheStick) {
    TestAircraftState own;
    StrikeModule sm;
    sm.config.drag_factor = 1.0;
    sm.set_target(42);
    own.north_ft = kAimNorth - own.vacuum_throw_ft(5000.0);
    sm.update(DT, &own, kAim, true);
    EXPECT_EQ(sm.salvo_fired(), 1);
    // A second strike waypoint against a different objective: fresh stick.
    sm.set_target(99);
    EXPECT_FALSE(sm.delivered());
    EXPECT_EQ(sm.salvo_fired(), 0);
    sm.update(DT, &own, kAim, true);
    EXPECT_EQ(sm.salvo_fired(), 1);
    EXPECT_EQ(sm.release_target_id(), 99u);
}

TEST(StrikeFireControl, ClearTargetThenSameTargetStartsFreshStick) {
    TestAircraftState own;
    StrikeModule sm;
    sm.config.drag_factor = 1.0;
    sm.set_target(42);
    own.north_ft = kAimNorth - own.vacuum_throw_ft(5000.0);
    sm.update(DT, &own, kAim, true);
    EXPECT_EQ(sm.salvo_fired(), 1);
    sm.clear_target();
    EXPECT_FALSE(sm.armed());
    EXPECT_FALSE(sm.has_target());
    // Re-arming the same target after a disarm starts a FRESH stick: the
    // disarm dropped the id, so the re-set reads as a new assignment —
    // exactly the re-attack doctrine (a second pass on the same objective
    // gets a fresh salvo).
    sm.set_target(42);
    EXPECT_EQ(sm.salvo_fired(), 0);
    sm.update(DT, &own, kAim, true);
    EXPECT_EQ(sm.salvo_fired(), 1);
}

// ============================================================================
// Delivery-action classification (the brain's arming input)
// ============================================================================

TEST(StrikeActions, DeliveryActionsClassify) {
    EXPECT_TRUE(is_ag_delivery_action(14));  // GNDSTRIKE
    EXPECT_TRUE(is_ag_delivery_action(15));  // NAVSTRIKE
    EXPECT_TRUE(is_ag_delivery_action(17));  // STRIKE
    EXPECT_TRUE(is_ag_delivery_action(18));  // BOMB
    EXPECT_TRUE(is_ag_delivery_action(19));  // SEAD
    EXPECT_FALSE(is_ag_delivery_action(0));  // NOTHING
    EXPECT_FALSE(is_ag_delivery_action(1));  // TAKEOFF
    EXPECT_FALSE(is_ag_delivery_action(12)); // CAP
    EXPECT_FALSE(is_ag_delivery_action(7));  // LAND
}
