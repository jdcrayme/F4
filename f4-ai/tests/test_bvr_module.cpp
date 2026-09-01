// test_bvr_module.cpp — unit tests for the BVRModule (AI_IMPLEMENTATION_PLAN.md
// §5 Step 8 validation table):
//   * range bands (entry ring 1.3x, WVR 3 NM, merge 2 NM)
//   * state ladder None -> Entering -> Employing -> (fire) -> Separating -> None
//   * lock intent on while fighting, off while separating
//   * fire control: release pulses once, cooldown, shoot-shoot limit
//   * crank offset 30-60 deg off the target bearing
//   * target lost (dead) -> None, empty output (brain returns to nav)
//
// The module is engine-agnostic: every test drives it with a mock
// IAircraftState and hand-built TargetInfo snapshots — no EntityWorld.

#include <gtest/gtest.h>

#include <cmath>
#include <memory>

#include <f4/ai/modules/bvr_module.hpp>

using namespace f4::ai;
using namespace f4::ai::modules;
namespace geo = f4::geo;
using namespace f4::flight;

namespace {

constexpr double FT_PER_NM = 6076.11548;
constexpr double kPi = 3.14159265358979323846;
constexpr double DT = 1.0 / 60.0;

class TestAircraftState : public IAircraftState {
public:
    double east_ft{0.0};
    double north_ft{0.0};
    double alt_msl_ft{20000.0};
    double alt_agl_ft_{20000.0};
    double vcas_kts_{450.0};
    double heading_rad_{0.0};       // north
    double pitch_rad_{0.0};
    double roll_rad_{0.0};
    double roll_rate_radps_{0.0};
    double pitch_rate_radps_{0.0};
    double yaw_rate_radps_{0.0};
    double vs_fpm_{0.0};
    bool on_ground_{false};
    double fuel_lbs_{5000.0};

    double position_east_ft()  const override { return east_ft; }
    double position_north_ft() const override { return north_ft; }
    double altitude_msl_ft()   const override { return alt_msl_ft; }
    double altitude_agl_ft()   const override { return alt_agl_ft_; }
    double vcas_kts()          const override { return vcas_kts_; }
    double heading_rad()       const override { return heading_rad_; }
    double pitch_angle_rad()   const override { return pitch_rad_; }
    double roll_angle_rad()    const override { return roll_rad_; }
    double roll_rate_radps()   const override { return roll_rate_radps_; }
    double pitch_rate_radps()  const override { return pitch_rate_radps_; }
    double yaw_rate_radps()    const override { return yaw_rate_radps_; }
    double vertical_speed_fpm() const override { return vs_fpm_; }
    bool   on_ground()         const override { return on_ground_; }
    double fuel_lbs()          const override { return fuel_lbs_; }
};

/// A hostile, radar-tracked fighter `range_nm` north of a northbound
/// ownship at 20000 ft, flying north (the classic stern chase).
TargetInfo hostile_ahead(double range_nm) {
    TargetInfo t;
    t.entity_id = 42;
    t.is_hostile = true;
    t.detected_by_radar = true;         // weapons-grade track
    t.range_nm = range_nm;
    t.range_ft = range_nm * FT_PER_NM;
    t.position = geo::WorldPosition(0.0, range_nm * FT_PER_NM, 20000.0);
    t.velocity = geo::WorldPosition(0.0, 420.0 * 1.68781, 0.0);  // north
    t.ata_rad = kPi;                     // target tail toward us
    t.rangedot = 100.0;                 // closing
    t.combat_class = 4;
    return t;
}

double wrap_2pi(double a) {
    while (a < 0.0) a += 2.0 * kPi;
    while (a >= 2.0 * kPi) a -= 2.0 * kPi;
    return a;
}

} // anonymous namespace

// ============================================================================
// Range bands (plan validation: target at 4NM -> BVR band, 2.5NM -> WVR)
// ============================================================================

TEST(BVRModule, RangeBandsFollowPlanConstants) {
    BVRModule bvr;
    // Defaults: max_pk 20 NM, entry mult 1.3 -> entry ring 26 NM.
    EXPECT_DOUBLE_EQ(bvr.entry_range_nm(), 26.0);

    EXPECT_EQ(bvr.band_for(30.0), BVRRangeBand::OutOfEnvelope);
    EXPECT_EQ(bvr.band_for(26.5), BVRRangeBand::OutOfEnvelope);
    EXPECT_EQ(bvr.band_for(25.0), BVRRangeBand::BVR);       // entry..envelope
    EXPECT_EQ(bvr.band_for(20.5), BVRRangeBand::BVR);
    EXPECT_EQ(bvr.band_for(20.0), BVRRangeBand::Employ);    // inside envelope
    EXPECT_EQ(bvr.band_for(12.0), BVRRangeBand::Employ);
    EXPECT_EQ(bvr.band_for(4.0),  BVRRangeBand::Employ);
    EXPECT_EQ(bvr.band_for(2.5),  BVRRangeBand::WVR);       // plan: 2.5 -> WVR
    EXPECT_EQ(bvr.band_for(2.0),  BVRRangeBand::Merge);     // plan: 2 -> merge
    EXPECT_EQ(bvr.band_for(1.0),  BVRRangeBand::Merge);
}

// ============================================================================
// State ladder
// ============================================================================

TEST(BVRModule, NoTargetOrFarTargetStaysNone) {
    BVRModule bvr;
    TestAircraftState own;

    // No target at all.
    auto out = bvr.update(DT, &own, nullptr);
    EXPECT_EQ(bvr.state(), BVRState::None);
    EXPECT_FALSE(bvr.wants_lock());
    // Empty output: pitch/roll/throttle all zero so the brain can detect
    // "no fight" and fly its mission module.
    EXPECT_DOUBLE_EQ(out.pitch_cmd, 0.0);
    EXPECT_DOUBLE_EQ(out.roll_cmd, 0.0);
    EXPECT_DOUBLE_EQ(out.throttle_cmd, 0.0);

    // Target far outside the entry ring (35 NM > 26 NM).
    const auto far = hostile_ahead(35.0);
    bvr.update(DT, &own, &far);
    EXPECT_EQ(bvr.state(), BVRState::None);
    EXPECT_FALSE(bvr.wants_lock());
}

TEST(BVRModule, InvisibleOrFriendlyTargetIsNotEngaged) {
    BVRModule bvr;
    TestAircraftState own;

    // Friendly fighter at 12 NM — the module must double-check the brain.
    auto friendly = hostile_ahead(12.0);
    friendly.is_hostile = false;
    bvr.update(DT, &own, &friendly);
    EXPECT_EQ(bvr.state(), BVRState::None);

    // Hostile but INVISIBLE (no detection flags — e.g. a corpse under the
    // radar-backed policy).
    auto unseen = hostile_ahead(12.0);
    unseen.detected_by_radar = false;
    bvr.update(DT, &own, &unseen);
    EXPECT_EQ(bvr.state(), BVRState::None);
}

TEST(BVRModule, EnteringThenEmployingThenLockIntent) {
    BVRModule bvr;
    TestAircraftState own;

    // 25 NM: inside the entry ring -> Entering, lock wanted immediately
    // (the track must be hot well before the envelope).
    auto t = hostile_ahead(25.0);
    bvr.update(DT, &own, &t);
    EXPECT_EQ(bvr.state(), BVRState::Entering);
    EXPECT_TRUE(bvr.wants_lock());
    EXPECT_EQ(bvr.lock_target_id(), 42u);
    EXPECT_EQ(bvr.tactic(), BVRTactic::Pursuit);

    // Closing to 12 NM: inside the envelope -> Employing (no shot yet:
    // Pk at 12 NM is ~0.62 but cooldown-free... see the fire tests below;
    // here we verify the STATE only by pre-spending the shots).
    bvr.fire().config().shoot_shoot_max_shots = 0;  // doctrine: never fire
    t = hostile_ahead(12.0);
    bvr.update(DT, &own, &t);
    EXPECT_EQ(bvr.state(), BVRState::Employing);
    EXPECT_TRUE(bvr.wants_lock());
}

TEST(BVRModule, DeadTargetReturnsToNoneWithEmptyOutput) {
    BVRModule bvr;
    TestAircraftState own;

    auto t = hostile_ahead(12.0);
    bvr.fire().config().shoot_shoot_max_shots = 0;
    bvr.update(DT, &own, &t);
    bvr.update(DT, &own, &t);
    ASSERT_EQ(bvr.state(), BVRState::Employing);

    // The host policy stops painting corpses -> brain passes nullptr.
    const auto out = bvr.update(DT, &own, nullptr);
    EXPECT_EQ(bvr.state(), BVRState::None);
    EXPECT_FALSE(bvr.wants_lock());
    EXPECT_FALSE(out.has_override);
    EXPECT_DOUBLE_EQ(out.throttle_cmd, 0.0);
}

// ============================================================================
// Fire control (plan validation: fire at MAR, cooldown, shoot-shoot)
// ============================================================================

TEST(BVRModule, FiresOnceThenCooldownBlocksTheSecondShot) {
    BVRModule bvr;
    TestAircraftState own;

    // 13 NM, tail aspect: Pk = 0.95 * (0.25+0.75*7/15) * 1.0 = 0.5735
    // >= 0.5 threshold -> fire. The SM steps one transition per tick:
    // tick 1 None->Entering, tick 2 Entering->Employing + the shot.
    auto t = hostile_ahead(13.0);
    (void)bvr.update(DT, &own, &t);
    const auto out = bvr.update(DT, &own, &t);
    EXPECT_EQ(bvr.state(), BVRState::Employing);
    EXPECT_TRUE(bvr.release_pulse());
    EXPECT_EQ(bvr.release_target_id(), 42u);
    EXPECT_TRUE(out.weapon_release);
    EXPECT_EQ(bvr.shots_fired(), 1);

    // The very next tick: no pulse (one-tick contract) even at a perfect
    // range — the 4 s cooldown is running.
    const auto out2 = bvr.update(DT, &own, &t);
    EXPECT_FALSE(bvr.release_pulse());
    EXPECT_FALSE(out2.weapon_release);
    EXPECT_EQ(bvr.shots_fired(), 1);
    EXPECT_GT(bvr.fire().cooldown_remaining_sec(), 0.0);

    // Shoot-shoot: through the 8 s crank window the cooldown (4 s)
    // expires at its midpoint, but the shot waits for the crank to drop
    // — exactly ONE more pulse, then the 2-round allotment is spent and
    // nothing further ever fires against this target.
    // The loop counts ONLY the second shot (the first was the pulse
    // asserted above): it drops when the 8 s crank window expires and the
    // expired cooldown lets the fire control go again.
    int pulses = 0;
    for (int i = 0; i < static_cast<int>(12.0 / DT); ++i) {
        if (bvr.update(DT, &own, &t).weapon_release) ++pulses;
    }
    EXPECT_EQ(pulses, 1);
    EXPECT_EQ(bvr.shots_fired(), 2);

    for (int i = 0; i < static_cast<int>(15.0 / DT); ++i) {
        bvr.update(DT, &own, &t);
    }
    EXPECT_EQ(bvr.shots_fired(), 2);
    EXPECT_FALSE(bvr.release_pulse());
}

TEST(BVRModule, NoShotOutsideEnvelopeOrBelowPkThreshold) {
    BVRModule bvr;
    TestAircraftState own;

    // 19 NM: Pk = 0.95 * (0.25 + 0.75 * 1/15) * 1.0 = 0.26 < 0.5 -> no
    // fire, but the state is still Employing (inside the envelope band).
    auto far = hostile_ahead(19.0);
    bvr.update(DT, &own, &far);
    bvr.update(DT, &own, &far);
    EXPECT_EQ(bvr.state(), BVRState::Employing);
    EXPECT_FALSE(bvr.release_pulse());

    // 4 NM (inside the 5 NM minimum): the envelope check refuses.
    BVRModule bvr2;
    auto close = hostile_ahead(4.0);
    bvr2.update(DT, &own, &close);
    bvr2.update(DT, &own, &close);
    EXPECT_FALSE(bvr2.release_pulse());
}

TEST(BVRModule, RwrOnlyPictureNeverFires) {
    BVRModule bvr;
    TestAircraftState own;

    // Visible ONLY through the RWR (a bearing warning, not a track):
    // the weapons-grade-picture rule must hold fire.
    auto t = hostile_ahead(13.0);
    t.detected_by_radar = false;
    t.detected_by_rwr = true;
    bvr.update(DT, &own, &t);
    EXPECT_FALSE(bvr.release_pulse());
}

// ============================================================================
// Crank (plan validation: 30-60 deg offset from target bearing)
// ============================================================================

TEST(BVRModule, CrankOffsetsFortyFiveDegreesOffTargetBearing) {
    BVRModule bvr;
    TestAircraftState own;

    // Fire once to arm the crank window (tick 2 = the shot; the crank
    // tactic shows from tick 3 on).
    auto t = hostile_ahead(13.0);
    bvr.update(DT, &own, &t);
    bvr.update(DT, &own, &t);
    ASSERT_EQ(bvr.shots_fired(), 1);
    bvr.update(DT, &own, &t);

    // Target dead north (bearing 0): crank heading = 0 + 45 deg.
    const auto hdg = bvr.desired_heading_rad();
    EXPECT_NEAR(hdg, kPi / 4.0, 0.001);
    EXPECT_EQ(bvr.tactic(), BVRTactic::Crank);
    // Plan band: 30-60 deg (0.52-1.05 rad).
    EXPECT_GE(hdg, 30.0 * kPi / 180.0 - 0.001);
    EXPECT_LE(hdg, 60.0 * kPi / 180.0 + 0.001);
}

TEST(BVRModule, SeparatingTurnsColdThenReopensRange) {
    BVRModule bvr;
    TestAircraftState own;

    // Spend the allotment instantly, then let the crank window expire.
    auto t = hostile_ahead(13.0);
    bvr.fire().config().shoot_shoot_max_shots = 1;
    bvr.update(DT, &own, &t);
    bvr.update(DT, &own, &t);
    ASSERT_EQ(bvr.shots_fired(), 1);

    // Crank window (8 s) + a tick.
    for (int i = 0; i < static_cast<int>(9.0 / DT); ++i) {
        bvr.update(DT, &own, &t);
    }
    // Allotment spent + crank done + target still hot -> BugOut.
    EXPECT_EQ(bvr.state(), BVRState::Separating);
    EXPECT_EQ(bvr.tactic(), BVRTactic::BugOut);
    EXPECT_FALSE(bvr.wants_lock());
    // Cold: 180 deg off the (dead north) target bearing.
    EXPECT_NEAR(wrap_2pi(bvr.desired_heading_rad()), kPi, 0.001);

    // The range reopens past 1.2 * entry (31.2 NM) + 5 s minimum: back to
    // None (disengaged; the brain returns to navigation).
    auto far = hostile_ahead(32.0);
    for (int i = 0; i < static_cast<int>(8.0 / DT); ++i) {
        bvr.update(DT, &own, &far);
    }
    EXPECT_EQ(bvr.state(), BVRState::None);
}

TEST(BVRModule, MergeTooCloseBugsOutImmediately) {
    BVRModule bvr;
    TestAircraftState own;

    // A target that closes to inside 2 NM: bug out even with shots left.
    // (Three SM steps: None -> Entering -> Employing -> BugOut.)
    auto t = hostile_ahead(1.5);
    bvr.fire().config().shoot_shoot_max_shots = 0;
    bvr.update(DT, &own, &t);
    bvr.update(DT, &own, &t);
    bvr.update(DT, &own, &t);
    EXPECT_EQ(bvr.state(), BVRState::Separating);
    EXPECT_EQ(bvr.tactic(), BVRTactic::BugOut);
}

// ============================================================================
// Steering sanity
// ============================================================================

TEST(BVRModule, PursuitSteersTowardTheTarget) {
    BVRModule bvr;
    TestAircraftState own;
    own.east_ft = 0.0;
    own.north_ft = 0.0;

    // Target northeast of us.
    auto t = hostile_ahead(12.0);
    t.position = geo::WorldPosition(12.0 * FT_PER_NM, 12.0 * FT_PER_NM,
                                    20000.0);
    t.range_nm = 17.0;
    bvr.fire().config().shoot_shoot_max_shots = 0;
    bvr.update(DT, &own, &t);

    // Direct bearing = 45 deg, but the target flies NORTH: lead pursuit
    // aims north of it (shallower than the direct bearing, well right of
    // our 0 deg heading) — the lead point, not the target.
    const double hdg = bvr.desired_heading_rad();
    EXPECT_GE(hdg, 0.3);
    EXPECT_LE(hdg, kPi / 4.0 + 0.02);
}

TEST(BVRModule, OutputIsSteeredNotRaw) {
    BVRModule bvr;
    TestAircraftState own;

    auto t = hostile_ahead(12.0);
    bvr.fire().config().shoot_shoot_max_shots = 0;
    const auto out = bvr.update(DT, &own, &t);
    // The AirSteering cascade produces bounded, non-trivial commands.
    EXPECT_GE(out.throttle_cmd, 0.0);
    EXPECT_LE(out.throttle_cmd, 1.5);
    EXPECT_GE(out.roll_cmd, -1.0);
    EXPECT_LE(out.roll_cmd, 1.0);
    // Combat never touches the gear/brakes.
    EXPECT_FALSE(out.gear_handle_down);
    EXPECT_FALSE(out.wheel_brakes);
    EXPECT_FALSE(out.parking_brake);
}
