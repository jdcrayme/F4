// test_gun_module.cpp — GunModule: the lead predictor (TOF, lead point,
// solution error), the employment envelope, the trigger state machine
// (burst → cooldown → idle), ammo budget, ROE, reset semantics.
//
// The predictor is the whole point of the guns cut: the tests pin the
// MATH (a head-on solution is tight, a beam target needs real lead, a
// stationary ownship has no solution) and the TRIGGER DISCIPLINE (one
// pulse per burst, cooldown between, budget spent, guns-tight holds
// everything).

#include <f4/ai/modules/gun_module.hpp>

#include <gtest/gtest.h>

#include <cmath>

using namespace f4::ai::modules;
using f4::ai::TargetInfo;

namespace {

constexpr double FT_PER_NM = 6076.11548;
constexpr double DT = 1.0 / 60.0;

/// A hostile, radar-tracked fighter `range_nm` NORTH of the ownship at
/// 20000 ft, moving with `vel_north_fps` (negative = closing head-on).
/// ata/ata_from mirror the WVR test fixture's geometry knobs.
TargetInfo hostile(double range_nm, double vel_north_fps,
                   double ata = 0.0, double ata_from = 0.0) {
    TargetInfo t;
    t.entity_id = 42;
    t.is_hostile = true;
    t.detected_by_radar = true;
    t.range_nm = range_nm;
    t.range_ft = range_nm * FT_PER_NM;
    t.position = f4::geo::WorldPosition(0.0, range_nm * FT_PER_NM, 20000.0);
    t.velocity = f4::geo::WorldPosition(0.0, vel_north_fps, 0.0);
    t.ata_rad = ata;
    t.ata_from_rad = ata_from;
    t.rangedot = -vel_north_fps;   // closure (positive = closing)
    t.combat_class = 4;
    return t;
}

f4::geo::WorldPosition ownship_at(double east, double north, double alt) {
    return f4::geo::WorldPosition(east, north, alt);
}

/// Ownship velocity: due north at `fps` (level flight).
f4::math::Vec3<double> north_vel(double fps) {
    return f4::math::Vec3<double>{0.0, fps, 0.0};
}

} // namespace

// ============================================================================
// The predictor — pure math
// ============================================================================

TEST(GunPredictor, BulletTofClosesWithRangeAndClosure) {
    GunModule gun;
    const auto own = ownship_at(0.0, 0.0, 20000.0);
    // 1800 ft head-on, 900 ft/s closure, 3400 ft/s muzzle:
    // 1800 / (3400 + 900) = 0.419 s.
    const auto t = hostile(1800.0 / FT_PER_NM, -900.0);
    EXPECT_NEAR(gun.bullet_tof_s(t, own), 1800.0 / 4300.0, 1e-6);

    // Zero closure: 1800 / 3400.
    const auto t2 = hostile(1800.0 / FT_PER_NM, 0.0);
    EXPECT_NEAR(gun.bullet_tof_s(t2, own), 1800.0 / 3400.0, 1e-6);

    // Opening target never yields a negative TOF.
    const auto t3 = hostile(1800.0 / FT_PER_NM, 600.0);
    EXPECT_GT(gun.bullet_tof_s(t3, own), 0.0);
    // ... and the clamp holds for an absurdly far target.
    const auto t4 = hostile(20.0, -900.0);
    EXPECT_DOUBLE_EQ(gun.bullet_tof_s(t4, own), 2.5);
}

TEST(GunPredictor, LeadPointIsWhereTargetWillBe) {
    GunModule gun;
    const auto own = ownship_at(0.0, 0.0, 20000.0);
    // Target 1800 ft north closing at 780 ft/s; TOF = 1800/4180 = 0.43 s.
    const auto t = hostile(1800.0 / FT_PER_NM, -780.0);
    const auto lead = gun.lead_point(t, own);
    EXPECT_NEAR(lead.x, 0.0, 1e-9);
    EXPECT_NEAR(lead.y, 1800.0 - 780.0 * gun.bullet_tof_s(t, own), 1e-6);
    // z: the target's level altitude PLUS the superelevation (the
    // gravity-drop compensation over the flight time).
    EXPECT_NEAR(lead.z,
                20000.0 + 0.5 * 32.174 * gun.bullet_tof_s(t, own) *
                              gun.bullet_tof_s(t, own), 1e-6);
}

TEST(GunPredictor, TrackFilePredictionFliesTheSnapshotForward) {
    GunModule gun;
    const auto own = ownship_at(0.0, 0.0, 20000.0);

    // A snapshot taken 3 s ago: the target was 6000 ft north flying
    // SOUTH (closing) at 780 ft/s. Its dead-reckoned NOW position is
    // 6000 - 780*3 = 3660 ft north — that is the geometry every
    // predictor below consumes, not the stale 6000.
    auto t = hostile(6000.0 / FT_PER_NM, -780.0);
    t.age_s = 3.0;

    const auto now = GunModule::predicted_position(t);
    EXPECT_NEAR(now.x, 0.0, 1e-9);
    EXPECT_NEAR(now.y, 6000.0 - 780.0 * 3.0, 1e-6);
    EXPECT_NEAR(GunModule::range_now_ft(t, own), 3660.0, 1e-6);

    // The envelope gate follows the PREDICTED range, not the stale
    // snapshot range (the whole point of the track-file model).
    gun.set_envelope_nm(0.5, 0.9);
    EXPECT_TRUE(gun.in_envelope(t, own));   // 3660 ft = 0.60 NM — in
    EXPECT_FALSE(gun.in_envelope(t,
        ownship_at(0.0, 3660.0 - 0.4 * FT_PER_NM, 20000.0)));  // 0.4 NM

    // And the lead point compounds on the prediction: the bullet's TOF
    // is measured from NOW.
    const auto lead = gun.lead_point(t, own);
    EXPECT_NEAR(lead.y, 3660.0 - 780.0 * gun.bullet_tof_s(t, own), 1e-6);
}

TEST(GunPredictor, HeadOnSolutionIsTight) {
    GunModule gun;
    // Ownship at the origin flying north; target dead ahead closing
    // head-on: the lead point sits on the north axis — the boresight
    // and the lead direction coincide. Error ~ 0.
    const auto t = hostile(1800.0 / FT_PER_NM, -900.0);
    const double err = gun.solution_error_rad(
        t, ownship_at(0.0, 0.0, 20000.0), north_vel(500.0));
    // Not exactly zero: the superelevation (gravity-drop compensation)
    // tilts the lead a fraction of a degree ABOVE the boresight line —
    // that is the fire computer aiming correctly.
    EXPECT_LT(err, 0.003);
}

TEST(GunPredictor, BeamTargetNeedsRealLead) {
    GunModule gun;
    // Target 3000 ft EAST (abeam), flying north at 500 ft/s (a beam
    // crossing target). The lead point pulls north of the target; a
    // north-pointing boresight is ~45 deg off the lead direction.
    auto t = hostile(1800.0 / FT_PER_NM, -900.0);
    t.position = f4::geo::WorldPosition(1800.0, 0.0, 20000.0);
    const double err = gun.solution_error_rad(
        t, ownship_at(0.0, 0.0, 20000.0), north_vel(500.0));
    // The lead point is at (3000, +lead): atan2(3000, lead) off north.
    EXPECT_GT(err, 0.5);   // tens of degrees — no trigger
}

TEST(GunPredictor, StationaryOwnshipHasNoSolution) {
    GunModule gun;
    const auto t = hostile(1800.0 / FT_PER_NM, -900.0);
    // Zero velocity = no boresight to measure against: maximum error.
    const double err = gun.solution_error_rad(
        t, ownship_at(0.0, 0.0, 20000.0), f4::math::Vec3<double>{});
    EXPECT_NEAR(err, 3.14159265358979, 1e-9);
}

// ============================================================================
// should_fire — the full gate stack
// ============================================================================

TEST(GunFireControl, HeadOnMergeSnapshotIsLegal) {
    GunModule gun;
    const auto t = hostile(1800.0 / FT_PER_NM, -900.0);   // ~0.49 NM
    EXPECT_TRUE(gun.should_fire(t, ownship_at(0.0, 0.0, 20000.0),
                                 north_vel(500.0)));
}

TEST(GunFireControl, OutOfEnvelopeNeverFires) {
    GunModule gun;
    // Beyond max range (0.9 NM default = 5468 ft).
    const auto far = hostile(1.5, -900.0);
    EXPECT_FALSE(gun.should_fire(far, ownship_at(0.0, 0.0, 20000.0),
                                 north_vel(500.0)));
    // Inside min range (0.08 NM = 486 ft) — don't shoot through.
    const auto close = hostile(486.0 / FT_PER_NM, -900.0);
    EXPECT_FALSE(gun.should_fire(close, ownship_at(0.0, 0.0, 20000.0),
                                 north_vel(500.0)));
}

TEST(GunFireControl, VisibleHostileFighterRequired) {
    GunModule gun;
    auto t = hostile(1800.0 / FT_PER_NM, -900.0);

    auto invisible = t;
    invisible.detected_by_radar = false;
    EXPECT_FALSE(gun.should_fire(invisible, ownship_at(0.0, 0.0, 20000.0),
                                 north_vel(500.0)));

    auto friendly = t;
    friendly.is_hostile = false;
    EXPECT_FALSE(gun.should_fire(friendly, ownship_at(0.0, 0.0, 20000.0),
                                 north_vel(500.0)));

    auto missile = t;
    missile.is_missile = true;   // never gun down an incoming missile
    EXPECT_FALSE(gun.should_fire(missile, ownship_at(0.0, 0.0, 20000.0),
                                 north_vel(500.0)));
}

TEST(GunFireControl, HoldFireSuppressesEverything) {
    GunModule gun;
    gun.config().hold_fire = true;
    const auto t = hostile(1800.0 / FT_PER_NM, -900.0);
    EXPECT_FALSE(gun.should_fire(t, ownship_at(0.0, 0.0, 20000.0),
                                 north_vel(500.0)));
}

// ============================================================================
// The trigger state machine
// ============================================================================

TEST(GunTrigger, BurstPulseIsOneTickThenCooldown) {
    GunModule gun;
    const auto t = hostile(1800.0 / FT_PER_NM, -900.0);
    const auto pos = ownship_at(0.0, 0.0, 20000.0);
    const auto vel = north_vel(500.0);

    ASSERT_TRUE(gun.should_fire(t, pos, vel));
    gun.note_burst();
    EXPECT_TRUE(gun.gun_pulse());           // the edge
    EXPECT_TRUE(gun.trigger_down());        // through the burst

    gun.tick(DT);                           // pulse consumed by tick
    EXPECT_FALSE(gun.gun_pulse());
    EXPECT_TRUE(gun.trigger_down());        // still bursting (~0.4 s)

    // Burst flight time (100 rounds @ 6000 rpm = 1.0 s) + 1 s cooldown.
    for (int i = 0; i < static_cast<int>(2.2 / DT); ++i) {
        gun.tick(DT);
    }
    EXPECT_FALSE(gun.trigger_down());
    EXPECT_NEAR(gun.cooldown_remaining_sec(), 0.0, 1e-6);  // ready again
}

TEST(GunTrigger, CannotDoubleFire) {
    GunModule gun;
    const auto t = hostile(1800.0 / FT_PER_NM, -900.0);
    const auto pos = ownship_at(0.0, 0.0, 20000.0);
    const auto vel = north_vel(500.0);

    gun.note_burst();
    gun.note_burst();   // second pull while bursting: no-op
    gun.tick(DT);
    EXPECT_EQ(gun.bursts_fired(), 1);

    // should_fire agrees: the trigger state is part of the gate.
    EXPECT_FALSE(gun.should_fire(t, pos, vel));
}

TEST(GunTrigger, BudgetSpendsAndStops) {
    GunModule gun;
    gun.set_rounds_budget(115);  // just over one 100-round burst
    const auto t = hostile(1800.0 / FT_PER_NM, -900.0);
    const auto pos = ownship_at(0.0, 0.0, 20000.0);
    const auto vel = north_vel(500.0);

    gun.note_burst();   // 100 of the 115
    EXPECT_EQ(gun.rounds_remaining(), 115 - 100);

    // Burn through the cooldown (1.0 s burst + 1.0 s pause).
    for (int i = 0; i < static_cast<int>(2.2 / DT); ++i) gun.tick(DT);

    // Second burst is clipped to the 15 remaining.
    ASSERT_TRUE(gun.should_fire(t, pos, vel));
    gun.note_burst();
    EXPECT_EQ(gun.rounds_remaining(), 0);
    EXPECT_EQ(gun.bursts_fired(), 2);

    // Dry drum: no more firing.
    EXPECT_FALSE(gun.should_fire(t, pos, vel));
    gun.note_burst();   // no-op on an empty budget
    EXPECT_EQ(gun.bursts_fired(), 2);
}

TEST(GunTrigger, ResetEngagementClearsTriggerNotAmmoOrCooldown) {
    GunModule gun;
    const auto t = hostile(1800.0 / FT_PER_NM, -900.0);
    const auto pos = ownship_at(0.0, 0.0, 20000.0);
    const auto vel = north_vel(500.0);

    gun.note_burst();
    gun.tick(DT);
    ASSERT_TRUE(gun.trigger_down());
    ASSERT_EQ(gun.rounds_remaining(), 511 - 100);

    gun.reset_engagement();
    EXPECT_FALSE(gun.trigger_down());
    EXPECT_EQ(gun.rounds_remaining(), 511 - 100);  // ammo survives
    EXPECT_EQ(gun.bursts_fired(), 1);              // the counter too

    // The cooldown still burns: mid-burst reset drops to cooldown, and
    // should_fire stays no until the cycle completes.
    EXPECT_FALSE(gun.should_fire(t, pos, vel));
    for (int i = 0; i < static_cast<int>(1.2 / DT); ++i) gun.tick(DT);
    EXPECT_TRUE(gun.should_fire(t, pos, vel));
}

// ============================================================================
// Configuration surface (host wiring)
// ============================================================================

TEST(GunConfig, EnvelopeAndBudgetSetters) {
    GunModule gun;
    gun.set_envelope_nm(0.1, 0.85);
    EXPECT_NEAR(gun.config().min_range_nm, 0.1, 1e-9);
    EXPECT_NEAR(gun.config().max_range_nm, 0.85, 1e-9);

    gun.set_rounds_budget(300);
    EXPECT_EQ(gun.rounds_remaining(), 300);
    EXPECT_EQ(gun.config().rounds_budget, 300);

    // A budget of 0 (no gun station) disarms the trigger.
    gun.set_rounds_budget(0);
    const auto t = hostile(1800.0 / FT_PER_NM, -900.0);
    EXPECT_FALSE(gun.should_fire(t, ownship_at(0.0, 0.0, 20000.0),
                                 north_vel(500.0)));
}
