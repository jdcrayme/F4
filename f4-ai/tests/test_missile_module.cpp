// test_missile_module.cpp — unit tests for the MissileModule
// (AI_IMPLEMENTATION_PLAN.md §5 Step 10 validation table):
//   * Pk model: monotonic in range, aspect-scaled, 0 out of envelope
//   * fire at (in-)range with Pk threshold + cooldown + shoot-shoot
//   * defeat: beam = 90 deg off the threat bearing, nearest side
//   * defeat: defensive override preempts (has_override)
//   * chaff (radar missile default) / flare (inside IR envelope)
//   * defeat-linger: the override persists briefly after the threat
//     disappears, then releases
//   * no threat: empty output, no override

#include <gtest/gtest.h>

#include <cmath>
#include <memory>

#include <f4/ai/modules/missile_module.hpp>

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
    double heading_rad_{0.0};
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

/// A hostile incoming missile `range_nm` NORTH of a northbound ownship.
TargetInfo incoming_missile(double range_nm) {
    TargetInfo t;
    t.entity_id = 77;
    t.is_hostile = true;
    t.is_missile = true;
    t.detected_by_rwr = true;    // the RWR launch warning paints it
    t.range_nm = range_nm;
    t.range_ft = range_nm * FT_PER_NM;
    t.position = geo::WorldPosition(0.0, range_nm * FT_PER_NM, 20000.0);
    t.velocity = geo::WorldPosition(0.0, -2600.0, 0.0);  // diving on us
    t.combat_class = 4;
    return t;
}

/// A hostile, radar-tracked fighter at `range_nm` with tail aspect.
TargetInfo hostile_target(double range_nm) {
    TargetInfo t;
    t.entity_id = 42;
    t.is_hostile = true;
    t.detected_by_radar = true;
    t.range_nm = range_nm;
    t.range_ft = range_nm * FT_PER_NM;
    t.position = geo::WorldPosition(0.0, range_nm * FT_PER_NM, 20000.0);
    t.velocity = geo::WorldPosition(0.0, 420.0 * 1.68781, 0.0);
    t.ata_rad = kPi;  // tail toward us
    t.rangedot = 100.0;
    t.combat_class = 4;
    return t;
}

} // anonymous namespace

// ============================================================================
// Fire control — Pk model (deterministic, monotonic)
// ============================================================================

TEST(MissileFireControl, PkIsMonotonicInRange) {
    MissileModule fc;
    double prev = 1.0e9;
    for (double r = 5.0; r <= 20.0; r += 0.5) {
        const auto t = hostile_target(r);
        const double pk = fc.compute_pk(t);
        EXPECT_LE(pk, prev + 1.0e-9) << "Pk must not increase with range";
        prev = pk;
    }
}

TEST(MissileFireControl, PkBoundsAndOutOfEnvelope) {
    MissileModule fc;
    EXPECT_NEAR(fc.compute_pk(hostile_target(5.0)), 0.95, 0.01);  // min range
    EXPECT_DOUBLE_EQ(fc.compute_pk(hostile_target(4.9)), 0.0);   // below min
    EXPECT_DOUBLE_EQ(fc.compute_pk(hostile_target(20.1)), 0.0);  // beyond max
    const double mid = fc.compute_pk(hostile_target(13.0));
    EXPECT_GT(mid, 0.5);
    EXPECT_LT(mid, 0.95);
}

TEST(MissileFireControl, TailAspectBeatsNoseAspect) {
    MissileModule fc;
    auto tail = hostile_target(13.0);   // ata = pi
    auto nose = hostile_target(13.0);
    nose.ata_rad = 0.0;                 // target pointing at us
    EXPECT_GT(fc.compute_pk(tail), fc.compute_pk(nose));
}

TEST(MissileFireControl, ShouldFireGates) {
    MissileModule fc;
    TestAircraftState own;

    // Legal shot: hostile, tracked, 13 NM, Pk 0.57 > 0.5.
    EXPECT_TRUE(fc.should_fire(hostile_target(13.0)));

    // Friendly.
    auto friendly = hostile_target(13.0);
    friendly.is_hostile = false;
    EXPECT_FALSE(fc.should_fire(friendly));

    // Invisible.
    auto unseen = hostile_target(13.0);
    unseen.detected_by_radar = false;
    EXPECT_FALSE(fc.should_fire(unseen));

    // RWR-only: not a weapons-grade picture.
    auto rwr_only = hostile_target(13.0);
    rwr_only.detected_by_radar = false;
    rwr_only.detected_by_rwr = true;
    EXPECT_FALSE(fc.should_fire(rwr_only));

    // A missile is never a fire-control target.
    EXPECT_FALSE(fc.should_fire(incoming_missile(13.0)));

    // Below the Pk threshold.
    EXPECT_FALSE(fc.should_fire(hostile_target(19.0)));

    // Out of envelope.
    EXPECT_FALSE(fc.should_fire(hostile_target(4.0)));

    // Cooldown.
    fc.note_fired();
    EXPECT_FALSE(fc.should_fire(hostile_target(13.0)));
    EXPECT_EQ(fc.shots_fired(), 1);

    // Shoot-shoot allotment.
    fc.tick_cooldown(5.0);
    fc.note_fired();
    EXPECT_FALSE(fc.should_fire(hostile_target(13.0)));
    EXPECT_EQ(fc.shots_fired(), 2);
}

TEST(MissileFireControl, CooldownTicksDownAndEngagementResets) {
    MissileModule fc;
    fc.note_fired();
    fc.tick_cooldown(1.0);
    EXPECT_NEAR(fc.cooldown_remaining_sec(), 3.0, 1.0e-9);
    for (int i = 0; i < 200; ++i) fc.tick_cooldown(DT);
    EXPECT_DOUBLE_EQ(fc.cooldown_remaining_sec(), 0.0);

    fc.reset_engagement();
    EXPECT_EQ(fc.shots_fired(), 0);
    // The cooldown SURVIVES the engagement reset (rail cadence, not
    // per-target state).
    fc.note_fired();
    fc.reset_engagement();
    EXPECT_EQ(fc.shots_fired(), 0);
    EXPECT_GT(fc.cooldown_remaining_sec(), 0.0);
}

// ============================================================================
// Missile defeat — beam maneuver + intents
// ============================================================================

TEST(MissileDefeat, NoThreatYieldsEmptyNonOverrideOutput) {
    MissileModule defense;
    TestAircraftState own;

    const auto out = defense.update(DT, &own, nullptr);
    EXPECT_FALSE(defense.is_defeating());
    EXPECT_FALSE(out.has_override);
    EXPECT_DOUBLE_EQ(out.pitch_cmd, 0.0);
    EXPECT_DOUBLE_EQ(out.roll_cmd, 0.0);
    EXPECT_DOUBLE_EQ(out.throttle_cmd, 0.0);
    EXPECT_FALSE(defense.should_chaff());
    EXPECT_FALSE(defense.should_flare());
}

TEST(MissileDefeat, BeamPutsThreatOnTheThreeNineLine) {
    MissileModule defense;
    TestAircraftState own;

    // Threat dead ahead (north, we fly north): beam = +/- 90 deg. The
    // NEARER beam from heading 0 is ambiguous (both 90 deg away) — either
    // is correct; assert |error to beam| == 90 deg.
    auto t = incoming_missile(8.0);
    defense.update(DT, &own, &t);
    ASSERT_TRUE(defense.is_defeating());
    double err = defense.beam_heading_rad() - own.heading_rad_;
    while (err > kPi) err -= 2.0 * kPi;
    while (err < -kPi) err += 2.0 * kPi;
    EXPECT_NEAR(std::fabs(err), kPi / 2.0, 0.001);

    // Threat from the east (bearing 90 deg): nearest beam for a
    // northbound jet is 90+90=180 (or 0); both |90 deg| off the THREAT
    // BEARING — assert the heading-to-bearing angle is 90 deg.
    MissileModule defense2;
    auto east = incoming_missile(8.0);
    east.position = geo::WorldPosition(8.0 * FT_PER_NM, 0.0, 20000.0);
    defense2.update(DT, &own, &east);
    double bearing_err = defense2.beam_heading_rad() - (kPi / 2.0);
    while (bearing_err > kPi) bearing_err -= 2.0 * kPi;
    while (bearing_err < -kPi) bearing_err += 2.0 * kPi;
    EXPECT_NEAR(std::fabs(bearing_err), kPi / 2.0, 0.001);
}

TEST(MissileDefeat, OverridePreemptsAndThrottleHitsAfterburner) {
    MissileModule defense;
    TestAircraftState own;

    auto t = incoming_missile(8.0);
    const auto out = defense.update(DT, &own, &t);
    EXPECT_TRUE(defense.is_defeating());
    EXPECT_TRUE(out.has_override);
    // Outrun: the speed PI (target 550 kt from 450 kt) commands well past
    // MIL immediately and walks to its clamp as the integral spins up.
    EXPECT_GE(out.throttle_cmd, 1.0);
    EXPECT_LE(out.throttle_cmd, 1.5);
    // Never gear/brakes in a fight.
    EXPECT_FALSE(out.gear_handle_down);
    EXPECT_FALSE(out.wheel_brakes);
}

TEST(MissileDefeat, ChaffAndFlareIntentConditions) {
    MissileModule defense;
    TestAircraftState own;

    // Far radar missile: chaff yes, flare no.
    const auto far = incoming_missile(8.0);
    defense.update(DT, &own, &far);
    EXPECT_TRUE(defense.should_chaff());
    EXPECT_FALSE(defense.should_flare());

    // Inside the IR envelope (3 NM): flare yes; chaff still on above the
    // 1 NM floor.
    MissileModule defense2;
    const auto ir_band = incoming_missile(2.0);
    defense2.update(DT, &own, &ir_band);
    EXPECT_TRUE(defense2.should_chaff());
    EXPECT_TRUE(defense2.should_flare());

    // Inside 1 NM: chaff stops (nothing beats kinematics that late).
    MissileModule defense3;
    const auto knifefight = incoming_missile(0.5);
    defense3.update(DT, &own, &knifefight);
    EXPECT_FALSE(defense3.should_chaff());
    EXPECT_TRUE(defense3.should_flare());
}

TEST(MissileDefeat, LingerFinishesTheJinkAfterTheThreatDisappears) {
    MissileModule defense;
    TestAircraftState own;
    defense.config().defeat_linger_sec = 2.0;

    // Threat visible for a while...
    auto t = incoming_missile(8.0);
    for (int i = 0; i < 60; ++i) defense.update(DT, &own, &t);
    ASSERT_TRUE(defense.is_defeating());

    // ...then the fuze fires and the sweep destroys the missile: the
    // picture goes empty. The override must persist ~2 s...
    const auto during = defense.update(DT, &own, nullptr);
    EXPECT_TRUE(defense.is_defeating());
    EXPECT_TRUE(during.has_override);

    int ticks = 0;
    while (defense.is_defeating() && ticks < 600) {
        defense.update(DT, &own, nullptr);
        ++ticks;
    }
    // Roughly the linger window (2 s), not zero and not forever.
    EXPECT_GT(ticks, static_cast<int>(1.0 / DT));
    EXPECT_LT(ticks, static_cast<int>(4.0 / DT));

    // ...then the override releases and the brain falls through.
    const auto after = defense.update(DT, &own, nullptr);
    EXPECT_FALSE(after.has_override);
    EXPECT_FALSE(defense.is_defeating());
}

TEST(MissileDefeat, NeverDefendsAgainstOwnTeamMissile) {
    MissileModule defense;
    TestAircraftState own;

    // A blue missile (ours) — hostile-only missile_threat() filters it
    // upstream; the module double-checks so a wiring change cannot make
    // the brain defend against its own weapon.
    auto own_shot = incoming_missile(8.0);
    own_shot.is_hostile = false;
    const auto out = defense.update(DT, &own, &own_shot);
    EXPECT_FALSE(defense.is_defeating());
    EXPECT_FALSE(out.has_override);
}

TEST(MissileDefeat, FreshThreatKeepsDefeatingAcrossTicks) {
    MissileModule defense;
    TestAircraftState own;

    auto t = incoming_missile(8.0);
    for (int i = 0; i < 300; ++i) {  // 5 s of sustained threat
        const auto out = defense.update(DT, &own, &t);
        EXPECT_TRUE(out.has_override);
    }
    EXPECT_TRUE(defense.is_defeating());
    EXPECT_EQ(defense.incoming_target_id(), 77u);
}
