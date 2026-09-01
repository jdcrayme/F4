// test_collision_avoid_module.cpp — unit tests for the mid-air
// collision-avoidance rung (the DigitalBrain priority ladder's rung 2;
// FreeFalcon digi_cavoid.cpp's CollisionCheck + CollisionAvoid ported
// 1:1 — hRange 200 ft, reactFact 0.55, GS_LIMIT 9.0 G, the escape point
// 45 deg azimuth / 45 deg elevation opposite the target's roll).
//
// Pure module test: mock state + hand-built traffic lists. Geometry is
// chosen so every detection branch (range gate, timeToImpact gate,
// extrapolation hit/miss, diverging early-out) is exercised exactly.

#include <gtest/gtest.h>

#include "f4/ai/modules/collision_avoid_module.hpp"

#include <cmath>
#include <vector>

using f4::ai::modules::CollisionAvoidModule;
using Intruder = CollisionAvoidModule::Intruder;

namespace {

constexpr double kDt = 1.0 / 60.0;
constexpr double PI = 3.14159265358979323846;

class MockState final : public f4::flight::IAircraftState {
public:
    double east_ft{0.0}, north_ft{0.0}, alt_msl_ft{10000.0};
    double alt_agl_ft_{10000.0};
    double vcas_kts_{450.0};
    double heading_rad_{0.0};     // north
    double pitch_rad_{0.0}, roll_rad_{0.0};
    double roll_rate_{0.0}, pitch_rate_{0.0}, yaw_rate_{0.0};
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
    double roll_rate_radps()   const override { return roll_rate_; }
    double pitch_rate_radps()  const override { return pitch_rate_; }
    double yaw_rate_radps()    const override { return yaw_rate_; }
    double vertical_speed_fpm() const override { return vs_fpm_; }
    bool   on_ground()         const override { return on_ground_; }
    double fuel_lbs()          const override { return fuel_lbs_; }
};

/// An intruder dead ahead at `range_ft`, closing at `closure_fps` along
/// the north axis (the ownship flies north at 750 ft/s; the intruder
/// flies south when closure > 750).
Intruder head_on(double range_ft, double closure_fps) {
    Intruder intr;
    intr.entity_id = 42;
    intr.position = f4::geo::WorldPosition(0.0, range_ft, 10000.0);
    intr.velocity = f4::geo::WorldPosition(0.0, 750.0 - closure_fps, 0.0);
    return intr;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Detection — the CollisionCheck gates, in the reference's order
// ---------------------------------------------------------------------------

TEST(CollisionAvoid, HeadOnMergePredictsCollision) {
    // 1,000 ft out, 1,500 ft/s closure: timeToImpact = 800/1500 = 0.53 s
    // < reactTime (0.707 s at 7 G) — the extrapolation runs and finds the
    // sub-200-ft point.
    CollisionAvoidModule ca;
    MockState s;
    ca.set_traffic({head_on(1000.0, 1500.0)}, std::nullopt);
    const auto out = ca.update(kDt, &s);
    EXPECT_TRUE(ca.is_avoiding());
    EXPECT_EQ(ca.intruder_id(), 42u);
    EXPECT_TRUE(out.has_override);
}

TEST(CollisionAvoid, DistantTrafficIsIgnored) {
    // 5,000 ft out at the same closure: timeToImpact = 4,800/1500 = 3.2 s
    // >> reactTime — the reference's first gate returns "no collision"
    // without ever extrapolating.
    CollisionAvoidModule ca;
    MockState s;
    ca.set_traffic({head_on(5000.0, 1500.0)}, std::nullopt);
    ca.update(kDt, &s);
    EXPECT_FALSE(ca.is_avoiding());
}

TEST(CollisionAvoid, DivergingTrafficIsIgnored) {
    // Opening geometry: the range rate is positive — no collision to find
    // (the reference's extrapolation breaks at the first diverging step).
    CollisionAvoidModule ca;
    MockState s;
    Intruder intr = head_on(1000.0, 1500.0);
    intr.velocity.y = 1000.0;   // both northbound, intruder faster: opening
    ca.set_traffic({intr}, std::nullopt);
    ca.update(kDt, &s);
    EXPECT_FALSE(ca.is_avoiding());
}

TEST(CollisionAvoid, PassingMissIsNotAThreat) {
    // Crossing traffic whose closest approach is ~360 ft (> hRange): the
    // timeToImpact gate PASSES (0.58 s < 0.707 — this is a near merge)
    // and the extrapolation runs — but no sub-200-ft point exists. This
    // is the case that separates a real cavoid from a proximity alarm.
    // Ownship north at ~760 ft/s at the origin; the intruder crosses
    // eastbound 800 ft ahead and 800 ft left at 750 ft/s.
    CollisionAvoidModule ca;
    MockState s;
    Intruder intr;
    intr.entity_id = 7;
    intr.position = f4::geo::WorldPosition(-800.0, 800.0, 10000.0);
    intr.velocity = f4::geo::WorldPosition(750.0, -750.0, 0.0);
    ca.set_traffic({intr}, std::nullopt);
    ca.update(kDt, &s);
    EXPECT_FALSE(ca.is_avoiding());
}

TEST(CollisionAvoid, InsideTheBubbleIsImmediate) {
    // Range 150 ft (< hRange): collision NOW — the timeToImpact gate is
    // skipped entirely (the reference's "range > hRange" guard).
    CollisionAvoidModule ca;
    MockState s;
    ca.set_traffic({head_on(150.0, 1500.0)}, std::nullopt);
    ca.update(kDt, &s);
    EXPECT_TRUE(ca.is_avoiding());
}

TEST(CollisionAvoid, ReactTimeScalesWithOwnMaxG) {
    // reactTime = (GS_LIMIT / maxGs) * reactFact. A 4-G airframe buys
    // 1.24 s of warning instead of 0.707 s — a threat 1,400 ft out at
    // 1,500 ft/s closure (timeToImpact 0.8 s) is ignored by the 7-G
    // jet but detected by the 4-G one. The sluggish airframe sees MORE.
    CollisionAvoidModule ca7;
    MockState s;
    ca7.set_traffic({head_on(1400.0, 1500.0)}, std::nullopt);
    ca7.update(kDt, &s);
    EXPECT_FALSE(ca7.is_avoiding());
    EXPECT_NEAR(ca7.react_time_sec(), 0.707, 0.01);

    CollisionAvoidModule ca4;
    auto cfg = ca4.config();
    cfg.own_max_g = 4.0;
    ca4.set_config(cfg);
    ca4.set_traffic({head_on(1400.0, 1500.0)}, std::nullopt);
    ca4.update(kDt, &s);
    EXPECT_TRUE(ca4.is_avoiding());
    EXPECT_NEAR(ca4.react_time_sec(), 1.238, 0.01);
}

// ---------------------------------------------------------------------------
// The escape — direction doctrine
// ---------------------------------------------------------------------------

TEST(CollisionAvoid, LevelIntruderBreaksRight) {
    // droll ~ 0: the tiebreak is the aviation head-on convention — the
    // escape azimuth sits +45 deg (right) of the bearing to the intruder.
    CollisionAvoidModule ca;
    MockState s;
    ca.set_traffic({head_on(1000.0, 1500.0)}, std::nullopt);
    ca.update(kDt, &s);
    // Intruder dead ahead (bearing 0): escape azimuth = +45 deg.
    EXPECT_NEAR(ca.escape_az_rad(), 45.0 * PI / 180.0, 1e-6);
}

TEST(CollisionAvoid, TargetRollingRightEscapesLeft) {
    // "45 deg azimuth ... opposite to target roll": a target rolling
    // RIGHT places the escape on the LEFT (-45 deg of the bearing).
    CollisionAvoidModule ca;
    MockState s;
    Intruder intr = head_on(1000.0, 1500.0);
    intr.roll_rate_radps = +0.5;   // rolling right
    ca.set_traffic({intr}, std::nullopt);
    ca.update(kDt, &s);
    EXPECT_NEAR(ca.escape_az_rad(), -45.0 * PI / 180.0, 1e-6);
}

TEST(CollisionAvoid, TargetRollingLeftEscapesRight) {
    CollisionAvoidModule ca;
    MockState s;
    Intruder intr = head_on(1000.0, 1500.0);
    intr.roll_rate_radps = -0.5;   // rolling left
    ca.set_traffic({intr}, std::nullopt);
    ca.update(kDt, &s);
    EXPECT_NEAR(ca.escape_az_rad(), +45.0 * PI / 180.0, 1e-6);
}

TEST(CollisionAvoid, EscapeGoesUpAndFullPower) {
    // The escape point is 10,000 ft at +45 deg elevation: the steering
    // targets own alt + 7,071 ft, max bank/VS caps lifted, AB throttle.
    CollisionAvoidModule ca;
    MockState s;
    ca.set_traffic({head_on(1000.0, 1500.0)}, std::nullopt);
    const auto out = ca.update(kDt, &s);
    EXPECT_DOUBLE_EQ(out.throttle_cmd, 1.5);
    EXPECT_TRUE(out.has_override);
}

// ---------------------------------------------------------------------------
// The linger + gating
// ---------------------------------------------------------------------------

TEST(CollisionAvoid, BreakLingersAfterTheThreatClears) {
    // The break is a maneuver: with the sky clean (empty traffic), the
    // module keeps flying the escape for avoid_hold_sec, then releases.
    // (Exact release tick carries a tick of float drift; the window is
    // what is asserted.)
    CollisionAvoidModule ca;
    MockState s;
    ca.set_traffic({head_on(1000.0, 1500.0)}, std::nullopt);
    ca.update(kDt, &s);
    ASSERT_TRUE(ca.is_avoiding());
    const double escape = ca.escape_az_rad();

    ca.set_traffic({}, std::nullopt);   // the sky goes clean
    bool released = false;
    int hold_ticks = 0;
    for (int i = 0; i < static_cast<int>(1.5 * 60) + 30; ++i) {
        const auto out = ca.update(kDt, &s);
        if (!ca.is_avoiding()) {
            EXPECT_FALSE(out.has_override);
            released = true;
            break;
        }
        EXPECT_TRUE(out.has_override);
        EXPECT_NEAR(ca.escape_az_rad(), escape, 1e-9);  // holds the LAST break
        ++hold_ticks;
    }
    EXPECT_TRUE(released);
    EXPECT_GE(hold_ticks, static_cast<int>(1.5 * 60) - 2);
    EXPECT_LE(hold_ticks, static_cast<int>(1.5 * 60) + 2);
}

TEST(CollisionAvoid, EmptySkyNeverArms) {
    CollisionAvoidModule ca;
    MockState s;
    ca.set_traffic({}, std::nullopt);
    const auto out = ca.update(kDt, &s);
    EXPECT_FALSE(ca.is_avoiding());
    EXPECT_FALSE(out.has_override);
}

TEST(CollisionAvoid, DisabledConfigNeverAvoids) {
    CollisionAvoidModule ca;
    auto cfg = ca.config();
    cfg.enabled = false;
    ca.set_config(cfg);
    MockState s;
    ca.set_traffic({head_on(200.0, 1500.0)}, std::nullopt);
    ca.update(kDt, &s);
    EXPECT_FALSE(ca.is_avoiding());
}

TEST(CollisionAvoid, NullStateIdles) {
    CollisionAvoidModule ca;
    const auto out = ca.update(kDt, nullptr);
    EXPECT_FALSE(ca.is_avoiding());
    EXPECT_FALSE(out.has_override);
}

// ---------------------------------------------------------------------------
// Host-fed own velocity (the same-frame relative geometry)
// ---------------------------------------------------------------------------

TEST(CollisionAvoid, HostVelocityFeedMatchesDerivedGeometry) {
    // The same head-on geometry, expressed two ways: derived from
    // heading+CAS (nullopt feed) vs pushed by the host — both must
    // detect (the derivation is CAS-based, the host feed exact).
    CollisionAvoidModule caDerived;
    MockState s;   // 450 KCAS north: derived speed ~763 ft/s
    caDerived.set_traffic({head_on(1000.0, 1500.0)}, std::nullopt);
    caDerived.update(kDt, &s);
    EXPECT_TRUE(caDerived.is_avoiding());

    CollisionAvoidModule caFed;
    caFed.set_traffic({head_on(1000.0, 1500.0)},
                      f4::geo::WorldPosition{0.0, 750.0, 0.0});
    caFed.update(kDt, &s);
    EXPECT_TRUE(caFed.is_avoiding());
    // Same picture in, same escape out.
    EXPECT_NEAR(caFed.escape_az_rad(), caDerived.escape_az_rad(), 1e-9);
}
