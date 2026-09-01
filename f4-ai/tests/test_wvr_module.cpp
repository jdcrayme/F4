// test_wvr_module.cpp — unit tests for the WVRModule (AI_IMPLEMENTATION_
// PLAN.md §5 Step 9 validation table):
//   * entry: hostile inside the band -> Merge, lock intent on
//   * geometry classes: own advantage / target advantage / neutral
//   * state picks: Merge -> Offensive / Defensive (dwell-guarded)
//   * defensive jink: offset off the threat bearing, reversing
//   * overshoot control inside the OverB guard
//   * IR fire control: pulse, cooldown, shoot-shoot, RWR-blind gate
//   * out-of-band / target lost -> None, empty output
//   * bug-out: shots spent + sustained defense -> BugOut -> exit ring
//
// The module is engine-agnostic: every test drives it with a mock
// IAircraftState and hand-built TargetInfo snapshots — no EntityWorld.

#include <gtest/gtest.h>

#include <cmath>
#include <memory>

#include <f4/ai/modules/wvr_module.hpp>

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

/// A hostile, radar-tracked fighter `range_nm` NORTH of a northbound
/// ownship at 20000 ft. `ata` controls the target's nose: 0 = pointed
/// at us, pi = tail toward us. `ata_from` (angle between ownship
/// velocity and the line of sight) defaults to 0 = we point at it.
TargetInfo hostile(double range_nm, double ata, double ata_from = 0.0) {
    TargetInfo t;
    t.entity_id = 42;
    t.is_hostile = true;
    t.detected_by_radar = true;         // weapons-grade track
    t.range_nm = range_nm;
    t.range_ft = range_nm * FT_PER_NM;
    t.position = geo::WorldPosition(0.0, range_nm * FT_PER_NM, 20000.0);
    t.velocity = geo::WorldPosition(0.0, 420.0 * 1.68781, 0.0);  // north
    t.ata_rad = ata;
    t.ata_from_rad = ata_from;
    t.rangedot = 600.0;                 // closing
    t.combat_class = 4;
    return t;
}

/// Head-on merge geometry: both aircraft pointed at each other.
TargetInfo head_on(double range_nm) {
    return hostile(range_nm, /*ata=*/0.0, /*ata_from=*/0.0);
}

/// We are BEHIND the target: it points away (ata = pi) and we point
/// at it (ata_from = 0) — classic chase geometry.
TargetInfo running(double range_nm) {
    return hostile(range_nm, /*ata=*/kPi, /*ata_from=*/0.0);
}

/// The target is BEHIND US and pointed at us: ata = 0 (its nose on us),
/// ata_from = pi (line of sight is behind our nose).
TargetInfo on_our_six(double range_nm) {
    return hostile(range_nm, /*ata=*/0.0, /*ata_from=*/kPi);
}

double wrap_2pi(double a) {
    while (a < 0.0) a += 2.0 * kPi;
    while (a >= 2.0 * kPi) a -= 2.0 * kPi;
    return a;
}

/// Run `n` one-tick updates, returning the module output of the LAST.
AIControlOutput run(WVRModule& wvr, const IAircraftState* s,
                    const TargetInfo* t, int n) {
    AIControlOutput out{};
    for (int i = 0; i < n; ++i) out = wvr.update(DT, s, t);
    return out;
}

} // anonymous namespace

// ============================================================================
// Geometry classification (pure functions)
// ============================================================================

TEST(WVRModule, GeometryClasses) {
    // Neutral: head-on merge — both pointed at each other.
    EXPECT_FALSE(WVRModule::own_advantage(head_on(2.0)));
    EXPECT_FALSE(WVRModule::target_advantage(head_on(2.0)));
    // Our advantage: target running, we point at it.
    EXPECT_TRUE(WVRModule::own_advantage(running(1.5)));
    EXPECT_FALSE(WVRModule::target_advantage(running(1.5)));
    // Their advantage: on our six, pointed at us.
    EXPECT_FALSE(WVRModule::own_advantage(on_our_six(1.5)));
    EXPECT_TRUE(WVRModule::target_advantage(on_our_six(1.5)));
}

// ============================================================================
// Entry / None / empty output
// ============================================================================

TEST(WVRModule, NoTargetIsEmpty) {
    WVRModule wvr;
    TestAircraftState s;
    const auto out = wvr.update(DT, &s, nullptr);
    EXPECT_EQ(wvr.state(), WVRState::None);
    EXPECT_TRUE(out.pitch_cmd == 0.0 && out.roll_cmd == 0.0 &&
                out.throttle_cmd == 0.0);
    EXPECT_FALSE(wvr.wants_lock());
    EXPECT_FALSE(wvr.release_pulse());
}

TEST(WVRModule, OutOfBandTargetIsEmpty) {
    // Past the exit ring (4.5 NM default) the module self-guards to
    // None — the fight belongs to BVRModule out there.
    WVRModule wvr;
    TestAircraftState s;
    const auto t = head_on(6.0);
    run(wvr, &s, &t, 10);
    EXPECT_EQ(wvr.state(), WVRState::None);
    EXPECT_FALSE(wvr.wants_lock());
}

TEST(WVRModule, EntryPutsModuleIntoMerge) {
    WVRModule wvr;
    TestAircraftState s;
    s.heading_rad_ = 0.5;  // 29 deg off the target bearing: steering
                            // must produce a roll command
    const auto t = head_on(2.5);
    const auto out = wvr.update(DT, &s, &t);
    EXPECT_EQ(wvr.state(), WVRState::Merge);
    EXPECT_EQ(wvr.state_name(), "Merge");
    EXPECT_TRUE(wvr.wants_lock());
    EXPECT_EQ(wvr.lock_target_id(), 42u);
    // Non-empty steering: the module flies (heading off-target => roll).
    EXPECT_NE(out.roll_cmd, 0.0);
    EXPECT_NE(out.throttle_cmd, 0.0);
    // The merge head-on with a weapons-grade track IS the IR
    // opportunity shot (all-aspect heater into the forward cone).
    EXPECT_TRUE(wvr.release_pulse());
    EXPECT_EQ(wvr.shots_fired(), 1);
}

// ============================================================================
// Geometry state picks (dwell-guarded)
// ============================================================================

TEST(WVRModule, AdvantageFlipsToOffensiveAfterDwell) {
    WVRModule wvr;
    wvr.config().tactic_dwell_sec = 0.0;  // accept flips immediately
    TestAircraftState s;
    auto t = head_on(2.5);
    run(wvr, &s, &t, 5);
    EXPECT_EQ(wvr.state(), WVRState::Merge);

    // The target starts running: we have the angle.
    t = running(1.8);
    run(wvr, &s, &t, 5);
    EXPECT_EQ(wvr.state(), WVRState::Offensive);
    EXPECT_EQ(wvr.state_name(), "Offensive");
    EXPECT_TRUE(wvr.wants_lock());
}

TEST(WVRModule, ThreatFlipsToDefensiveAfterDwell) {
    WVRModule wvr;
    wvr.config().tactic_dwell_sec = 0.0;
    TestAircraftState s;
    auto t = head_on(2.5);
    run(wvr, &s, &t, 5);

    // The target slides onto our six, pointed at us.
    t = on_our_six(1.8);
    run(wvr, &s, &t, 5);
    EXPECT_EQ(wvr.state(), WVRState::Defensive);
    EXPECT_EQ(wvr.state_name(), "Defensive");
    EXPECT_EQ(wvr.tactic_name(), "GunJink");
    EXPECT_TRUE(wvr.wants_lock());
}

TEST(WVRModule, DwellSuppressesGeometryChatter) {
    // A state flip must be held tactic_dwell_sec before it is accepted.
    WVRModule wvr;
    wvr.config().tactic_dwell_sec = 2.0;
    TestAircraftState s;
    auto t = head_on(2.5);
    wvr.update(DT, &s, &t);  // enter Merge, dwell starts

    t = running(1.8);
    // 1 s of advantage geometry (< 2 s dwell): still Merge.
    run(wvr, &s, &t, 60);
    EXPECT_EQ(wvr.state(), WVRState::Merge);
    // 2+ s: accepted.
    run(wvr, &s, &t, 70);
    EXPECT_EQ(wvr.state(), WVRState::Offensive);
}

// ============================================================================
// Steering: jink + reversal + overshoot
// ============================================================================

TEST(WVRModule, DefensiveJinkOffsetsAndReverses) {
    WVRModule wvr;
    wvr.config().tactic_dwell_sec = 0.0;
    wvr.config().jink_period_sec = 3.0;
    TestAircraftState s;
    auto t = on_our_six(1.8);
    run(wvr, &s, &t, 5);
    ASSERT_EQ(wvr.state(), WVRState::Defensive);

    // Threat bearing: dead north (0 rad) from ownship. The jink offsets
    // +60 deg (jink_side starts +1) off the bearing.
    const double h_plus = wvr.desired_heading_rad();
    EXPECT_NEAR(wrap_2pi(h_plus), 60.0 * kPi / 180.0, 1e-6);

    // Hold until the reversal (3 s at 60 Hz = 180 ticks; 5 already spent).
    run(wvr, &s, &t, 180);
    const double h_minus = wvr.desired_heading_rad();
    EXPECT_NEAR(wrap_2pi(h_minus), 300.0 * kPi / 180.0, 1e-6);
    // The reversal is a 120-deg swing in heading space (minimum angular
    // distance — the raw wrap of -120 deg reads as +240).
    double swing = std::fabs(wrap_2pi(h_minus - h_plus));
    if (swing > kPi) swing = 2.0 * kPi - swing;
    EXPECT_NEAR(swing, 120.0 * kPi / 180.0, 1e-6);
}

TEST(WVRModule, OvershootControlInsideGuard) {
    WVRModule wvr;
    wvr.config().tactic_dwell_sec = 0.0;
    TestAircraftState s;
    // Hard closure (rangedot 1200 ft/s) inside the 0.35 NM overshoot
    // guard, target running — Offensive geometry.
    auto t = running(0.30);
    t.rangedot = 1200.0;
    run(wvr, &s, &t, 5);
    ASSERT_EQ(wvr.state(), WVRState::Offensive);
    EXPECT_EQ(wvr.tactic_name(), "OverB");
    // The offset jinks the pursuit heading off the direct bearing.
    const double bearing = 0.0;  // target due north
    EXPECT_GT(std::fabs(wvr.desired_heading_rad() - bearing), 0.1);
}

// ============================================================================
// IR fire control
// ============================================================================

TEST(WVRModule, MergeFiresOnceThenCooldown) {
    WVRModule wvr;
    wvr.config().tactic_dwell_sec = 0.0;
    TestAircraftState s;
    const auto t = head_on(2.5);

    // First tick in Merge with a weapons-grade track inside the IR
    // envelope (default [0.5, 8] NM): one release pulse.
    wvr.update(DT, &s, &t);
    EXPECT_EQ(wvr.state(), WVRState::Merge);
    EXPECT_TRUE(wvr.release_pulse());
    EXPECT_EQ(wvr.release_target_id(), 42u);
    EXPECT_EQ(wvr.shots_fired(), 1);

    // Cooldown: no second pulse for the next 3 s.
    for (int i = 0; i < 180; ++i) {
        const auto o = wvr.update(DT, &s, &t);
        EXPECT_FALSE(o.weapon_release) << "tick " << i;
    }
    EXPECT_EQ(wvr.shots_fired(), 1);
}

TEST(WVRModule, ShootShootLimitIsTwo) {
    WVRModule wvr;
    wvr.config().tactic_dwell_sec = 0.0;
    TestAircraftState s;
    const auto t = head_on(2.5);

    // Burn the full allotment with cooldown gaps (4 s > 3 s cooldown).
    run(wvr, &s, &t, 1);
    run(wvr, &s, &t, 240);  // 4 s
    run(wvr, &s, &t, 240);  // another 4 s
    EXPECT_EQ(wvr.shots_fired(), 2);

    // No further pulses, ever, within this engagement.
    for (int i = 0; i < 600; ++i) {
        wvr.update(DT, &s, &t);
    }
    EXPECT_EQ(wvr.shots_fired(), 2);
}

TEST(WVRModule, RwrBlindTargetNeverFires) {
    // The weapons-grade-picture gate: an RWR-only contact is a bearing
    // warning, not a track you can put a heater on.
    WVRModule wvr;
    wvr.config().tactic_dwell_sec = 0.0;
    TestAircraftState s;
    auto t = head_on(2.5);
    t.detected_by_radar = false;
    t.detected_by_gci = false;
    t.detected_by_rwr = true;
    run(wvr, &s, &t, 120);
    EXPECT_EQ(wvr.state(), WVRState::Merge);
    EXPECT_EQ(wvr.shots_fired(), 0);
    EXPECT_FALSE(wvr.release_pulse());
}

TEST(WVRModule, DefensiveStateDoesNotFire) {
    // Even with a weapons-grade track, the defensive geometry is not an
    // IR opportunity: no pulses while the target holds the angle.
    WVRModule wvr;
    wvr.config().tactic_dwell_sec = 0.0;
    TestAircraftState s;
    const auto t = on_our_six(1.8);
    run(wvr, &s, &t, 120);
    ASSERT_EQ(wvr.state(), WVRState::Defensive);
    EXPECT_EQ(wvr.shots_fired(), 0);
    EXPECT_FALSE(wvr.release_pulse());
}

// ============================================================================
// Target loss / reset
// ============================================================================

TEST(WVRModule, TargetLostReturnsToNone) {
    WVRModule wvr;
    wvr.config().tactic_dwell_sec = 0.0;
    TestAircraftState s;
    const auto t = head_on(2.5);
    run(wvr, &s, &t, 10);
    ASSERT_EQ(wvr.state(), WVRState::Merge);

    const auto out = wvr.update(DT, &s, nullptr);
    EXPECT_EQ(wvr.state(), WVRState::None);
    EXPECT_TRUE(out.pitch_cmd == 0.0 && out.throttle_cmd == 0.0);
    EXPECT_FALSE(wvr.wants_lock());
    EXPECT_EQ(wvr.lock_target_id(), 0u);
}

TEST(WVRModule, ResetClearsEngagement) {
    WVRModule wvr;
    wvr.config().tactic_dwell_sec = 0.0;
    TestAircraftState s;
    const auto t = head_on(2.5);
    run(wvr, &s, &t, 1);  // one shot away
    ASSERT_EQ(wvr.shots_fired(), 1);

    wvr.reset();
    EXPECT_EQ(wvr.state(), WVRState::None);
    EXPECT_EQ(wvr.shots_fired(), 0);
    EXPECT_EQ(wvr.lock_target_id(), 0u);

    // The shot COUNT re-arms on re-engagement, but the launch cooldown
    // is the shooter's rail cadence and survives the reset (the same
    // contract BVRModule documents). Burn it, then re-engage.
    run(wvr, &s, &t, 200);  // 3.3 s > 3 s cooldown
    EXPECT_EQ(wvr.state(), WVRState::Merge);
    EXPECT_EQ(wvr.shots_fired(), 1);
}

// ============================================================================
// Bug-out doctrine
// ============================================================================

TEST(WVRModule, SpentAndSustainedDefenseBugsOut) {
    WVRModule wvr;
    wvr.config().tactic_dwell_sec = 0.0;
    wvr.config().defensive_grace_sec = 8.0;
    TestAircraftState s;

    // Spend the IR allotment in Merge geometry first.
    const auto merge = head_on(2.5);
    run(wvr, &s, &merge, 1);
    run(wvr, &s, &merge, 240);
    run(wvr, &s, &merge, 240);
    ASSERT_EQ(wvr.shots_fired(), 2);

    // Sustained defense (grace 8 s = 480 ticks).
    const auto six = on_our_six(1.8);
    run(wvr, &s, &six, 5);
    ASSERT_EQ(wvr.state(), WVRState::Defensive);
    run(wvr, &s, &six, 480);
    EXPECT_EQ(wvr.state(), WVRState::BugOut);
    EXPECT_EQ(wvr.state_name(), "BugOut");
    EXPECT_EQ(wvr.tactic_name(), "BugOut");
    EXPECT_FALSE(wvr.wants_lock());

    // Bug-out heading: cold (180 deg off the threat bearing due north).
    EXPECT_NEAR(wvr.desired_heading_rad(), kPi, 1e-6);

    // The range reopens past the exit ring: separation complete.
    const auto far = on_our_six(6.0);
    run(wvr, &s, &far, 5);
    EXPECT_EQ(wvr.state(), WVRState::None);
}

TEST(WVRModule, NoBugoutWhileHeatersRemain) {
    // Defensive with the allotment UNspent: the grace timer runs, but
    // the module stays Defensive — there is always a reason to stay.
    WVRModule wvr;
    wvr.config().tactic_dwell_sec = 0.0;
    wvr.config().defensive_grace_sec = 8.0;
    TestAircraftState s;
    const auto six = on_our_six(1.8);
    run(wvr, &s, &six, 600);  // 10 s — well past the grace
    EXPECT_EQ(wvr.state(), WVRState::Defensive);
    EXPECT_EQ(wvr.shots_fired(), 0);
}

// ============================================================================
// Names
// ============================================================================

TEST(WVRModule, NamesMatchFreeFalconVocabulary) {
    WVRModule wvr;
    wvr.config().tactic_dwell_sec = 0.0;
    TestAircraftState s;
    const auto t = head_on(2.5);
    run(wvr, &s, &t, 5);
    EXPECT_EQ(wvr.state_name(), "Merge");
    EXPECT_EQ(wvr.tactic_name(), "RandP");

    const auto six = on_our_six(1.8);
    run(wvr, &s, &six, 5);
    EXPECT_EQ(wvr.state_name(), "Defensive");
    EXPECT_EQ(wvr.tactic_name(), "GunJink");

    // None-state reporting (the brain's HUD strings come from these).
    wvr.reset();
    EXPECT_EQ(wvr.state_name(), "None");
    EXPECT_EQ(wvr.tactic_name(), "None");
}

// ============================================================================
// GUNS (Steps 11-12) — the trigger intents through the WVR module
// ============================================================================
//
// The gun fire control needs a MOVING ownship (the boresight estimate is
// consecutive positions / dt — a static test state has no boresight, so
// the guns can never fire: safe by construction). The gun tests below
// advance the ownship every tick.

namespace {

/// Advance the ownship one tick north at `fps` (and return the output of
/// that update) — gives the WVR module a real velocity estimate.
AIControlOutput advance_and_update(WVRModule& wvr, TestAircraftState* s,
                                   const TargetInfo* t, double fps) {
    s->north_ft += fps * DT;
    return wvr.update(DT, s, t);
}

} // namespace

TEST(WVRModuleGuns, MergeSnapshotFiresOneBurstPerCycle) {
    WVRModule wvr;
    wvr.config().tactic_dwell_sec = 0.0;
    TestAircraftState s;

    // Head-on target inside the gun envelope (~0.5 NM), closing.
    auto t = head_on(0.3);
    t.rangedot = 900.0;

    // TWO-tick boresight warmup: the entry tick's engage() drops the
    // velocity history (it may be stale from before the fight), so the
    // estimate exists from the third tick — the first the guns can fire.
    advance_and_update(wvr, &s, &t, 500.0);   // tick 1: entry
    EXPECT_EQ(wvr.state(), WVRState::Merge);
    EXPECT_FALSE(wvr.gun_pulse());
    advance_and_update(wvr, &s, &t, 500.0);   // tick 2: history builds
    EXPECT_FALSE(wvr.gun_pulse());

    // Tick 3: the boresight is measured, the solution is dead-on.
    const auto out = advance_and_update(wvr, &s, &t, 500.0);
    EXPECT_TRUE(wvr.gun_pulse()) << "the merge snapshot never fired";
    EXPECT_EQ(wvr.gun_target_id(), 42u);
    EXPECT_TRUE(out.trigger_down) << "the trigger must read held during "
                                     "the burst";

    // The gun then cycles: no pulse for burst + cooldown (~1.4 s)...
    int pulses = 1;
    for (int i = 0; i < static_cast<int>(1.2 / DT); ++i) {
        advance_and_update(wvr, &s, &t, 500.0);
        if (wvr.gun_pulse()) ++pulses;
    }
    EXPECT_EQ(pulses, 1) << "gun fired during its own cooldown";
    EXPECT_EQ(wvr.guns().bursts_fired(), 1);

    // ...and the trigger is ready again after the cycle (burst 2 within
    // ~3.5 s — trigger discipline, not a one-shot).
    for (int i = 0; i < static_cast<int>(2.5 / DT) && pulses < 2; ++i) {
        advance_and_update(wvr, &s, &t, 500.0);
        if (wvr.gun_pulse()) ++pulses;
    }
    EXPECT_EQ(pulses, 2);
    EXPECT_EQ(wvr.guns().rounds_remaining(), 511 - 200);
}

TEST(WVRModuleGuns, GunsTightHoldsTheTrigger) {
    WVRModule wvr;
    wvr.config().tactic_dwell_sec = 0.0;
    wvr.guns().config().hold_fire = true;   // the scenario's guns_hold
    TestAircraftState s;
    auto t = head_on(0.3);
    t.rangedot = 900.0;

    for (int i = 0; i < 120; ++i) {
        advance_and_update(wvr, &s, &t, 500.0);
        EXPECT_FALSE(wvr.gun_pulse()) << "tick " << i;
    }
    EXPECT_EQ(wvr.guns().bursts_fired(), 0);
    EXPECT_EQ(wvr.guns().rounds_remaining(), 511);
}

TEST(WVRModuleGuns, OutOfGunEnvelopeNoTrigger) {
    WVRModule wvr;
    wvr.config().tactic_dwell_sec = 0.0;
    TestAircraftState s;

    // Head-on at 2.5 NM: inside the WVR band and the IR envelope, but
    // far outside the 0.9 NM gun envelope.
    auto t = head_on(2.5);
    for (int i = 0; i < 120; ++i) {
        advance_and_update(wvr, &s, &t, 500.0);
        EXPECT_FALSE(wvr.gun_pulse()) << "tick " << i;
    }
}

TEST(WVRModuleGuns, EmptyDrumNoTrigger) {
    WVRModule wvr;
    wvr.config().tactic_dwell_sec = 0.0;
    wvr.guns().set_rounds_budget(0);
    TestAircraftState s;
    auto t = head_on(0.3);
    t.rangedot = 900.0;

    for (int i = 0; i < 120; ++i) {
        advance_and_update(wvr, &s, &t, 500.0);
        EXPECT_FALSE(wvr.gun_pulse()) << "tick " << i;
    }
}

TEST(WVRModuleGuns, OffensiveSteeringTracksTheGunSolution) {
    WVRModule wvr;
    wvr.config().tactic_dwell_sec = 0.0;
    TestAircraftState s;

    // A RUNNING target (we hold the angle) at an offset bearing,
    // inside the gun envelope (~0.3 NM). SLOW closure (rangedot 200 —
    // under the 300 ft/s overshoot-guard trigger): a tracking gunnery
    // pass, not a hard-closure overshoot (OverB would rightly own the
    // steering there).
    auto t = running(0.3);
    t.rangedot = 200.0;
    t.position = f4::geo::WorldPosition(1200.0, 1200.0, 20000.0);
    t.velocity = f4::geo::WorldPosition(0.0, 420.0 * 1.68781, 0.0);

    // Fly into Offensive with a moving ownship.
    for (int i = 0; i < 30; ++i) advance_and_update(wvr, &s, &t, 500.0);
    ASSERT_EQ(wvr.state(), WVRState::Offensive);

    // Inside the gun envelope the steering swaps the missile-grade
    // pursuit for the gun lead bearing.
    const double expected = wvr.guns().lead_heading_rad(
        t, f4::geo::WorldPosition(0.0, s.north_ft, 20000.0));
    const double pursuit = std::atan2(t.position.x, t.position.y -
                                                        s.north_ft);
    EXPECT_NEAR(wvr.desired_heading_rad(), expected, 1e-9);
    // And the swap is REAL: the pursuit and the gun solution differ at
    // this geometry (the gun's TOF is a fraction of the pursuit's).
    EXPECT_GT(std::fabs(expected - pursuit), 0.05)
        << "test geometry failed to separate the two lead laws";
}

TEST(WVRModuleGuns, ResetClearsTheGunEngagement) {
    WVRModule wvr;
    wvr.config().tactic_dwell_sec = 0.0;
    TestAircraftState s;
    auto t = head_on(0.3);
    t.rangedot = 900.0;

    // The two-tick boresight warmup, then the burst.
    advance_and_update(wvr, &s, &t, 500.0);
    advance_and_update(wvr, &s, &t, 500.0);
    advance_and_update(wvr, &s, &t, 500.0);
    ASSERT_TRUE(wvr.gun_pulse());

    wvr.reset();
    EXPECT_FALSE(wvr.gun_pulse());
    EXPECT_FALSE(wvr.guns().trigger_down());
    // Ammo and the doctrine counter survive the reset.
    EXPECT_EQ(wvr.guns().rounds_remaining(), 511 - 100);
    EXPECT_EQ(wvr.guns().bursts_fired(), 1);
}
