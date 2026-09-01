// test_ground_avoid_module.cpp — unit tests for the terrain-avoidance
// pull-up (the DigitalBrain priority ladder's rung 1; FreeFalcon
// MIN_ALTT / g_fGALookAheadTime / g_fPullupTime constants).
//
// The module is a pure function of (ownship state, host terrain picture):
// no world, no bus — the mock state + a hand-built TerrainPicture drive
// every case. The mock is the shared IAircraftState double every other
// module test uses (fuel_lbs included — the interface grew a fuel gauge).

#include <gtest/gtest.h>

#include "f4/ai/modules/ground_avoid_module.hpp"

#include <cmath>

using f4::ai::modules::GroundAvoidModule;
using Picture = GroundAvoidModule::TerrainPicture;

namespace {

constexpr double kDt = 1.0 / 60.0;

class MockState final : public f4::flight::IAircraftState {
public:
    double east_ft{0.0}, north_ft{0.0}, alt_msl_ft{10000.0};
    double alt_agl_ft_{10000.0};
    double vcas_kts_{400.0};
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

Picture flat_sea() {
    Picture p;
    p.valid = true;
    p.terrain_here_ft = 0.0;
    p.terrain_ahead_ft = 0.0;
    return p;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Detection
// ---------------------------------------------------------------------------

TEST(GroundAvoid, LevelAboveFlatTerrainNeverPulls) {
    GroundAvoidModule ga;
    MockState s;                       // 10,000 ft over flat 0
    const auto out = ga.update(kDt, &s, flat_sea());
    EXPECT_FALSE(ga.pulling_up());
    EXPECT_FALSE(out.has_override);
    EXPECT_DOUBLE_EQ(ga.clearance_ft(), 10000.0);
}

TEST(GroundAvoid, LowAltitudePullsUp) {
    // MIN_ALTT = 1500: 1,200 ft over flat ground is inside the floor.
    GroundAvoidModule ga;
    MockState s;
    s.alt_msl_ft = 1200.0;
    s.alt_agl_ft_ = 1200.0;
    const auto out = ga.update(kDt, &s, flat_sea());
    EXPECT_TRUE(ga.pulling_up());
    EXPECT_TRUE(ga.ground_avoid_needed());
    EXPECT_TRUE(out.has_override);
    EXPECT_DOUBLE_EQ(out.throttle_cmd, 1.5);   // the escape is max power
}

TEST(GroundAvoid, RisingTerrainAheadPullsEarly) {
    // The look-ahead is the point: 5,000 ft AGL NOW, but the ridge ahead
    // (in the picture's look-ahead cone) reaches 4,200 ft — the predicted
    // clearance (5,000 - 4,200 = 800) is inside the floor. A module that
    // only looked DOWN would fly into the ridge.
    GroundAvoidModule ga;
    MockState s;
    s.alt_msl_ft = 5000.0;
    s.alt_agl_ft_ = 5000.0;
    Picture p = flat_sea();
    p.terrain_ahead_ft = 4200.0;
    ga.update(kDt, &s, p);
    EXPECT_TRUE(ga.pulling_up());
}

TEST(GroundAvoid, SinkProjectionTriggersEarly) {
    // A 6,000 fpm dive from 2,000 ft over flat ground: the predicted
    // altitude at the 6-s horizon is 2,000 - 600 = 1,400 < 1,500. The
    // pull-up fires while there is still sky to trade.
    GroundAvoidModule ga;
    MockState s;
    s.alt_msl_ft = 2000.0;
    s.alt_agl_ft_ = 2000.0;
    s.vs_fpm_ = -6000.0;
    ga.update(kDt, &s, flat_sea());
    EXPECT_TRUE(ga.pulling_up());
}

TEST(GroundAvoid, ClimbNeverTriggersThePredictor) {
    // A climb from 1,600 ft: the NOW clearance is 1,600 (above floor? no
    // — 1,600 > 1,500 barely) with terrain_ahead 0 and climbing VS: no.
    GroundAvoidModule ga;
    MockState s;
    s.alt_msl_ft = 1600.0;
    s.alt_agl_ft_ = 1600.0;
    s.vs_fpm_ = +4000.0;
    ga.update(kDt, &s, flat_sea());
    EXPECT_FALSE(ga.pulling_up());
}

// ---------------------------------------------------------------------------
// Hysteresis + the pullupTimer
// ---------------------------------------------------------------------------

TEST(GroundAvoid, HoldContinuesAfterTheThreatClears) {
    // g_fPullupTime: the recovery CONTINUES after the picture goes clean
    // — a real pull-up is flown, not pulsed for one tick. (The exact
    // release tick carries a tick or two of float drift; the window is
    // what is asserted.)
    GroundAvoidModule ga;
    MockState s;
    s.alt_msl_ft = 1200.0;
    s.alt_agl_ft_ = 1200.0;

    ga.update(kDt, &s, flat_sea());
    ASSERT_TRUE(ga.pulling_up());

    // Picture clears (the host pushed 10,000 ft of flat terrain now).
    s.alt_msl_ft = 10000.0;
    s.alt_agl_ft_ = 10000.0;
    Picture clean = flat_sea();

    bool released = false;
    int hold_ticks = 0;
    for (int i = 0; i < 3 * 60 + 30; ++i) {
        const auto out = ga.update(kDt, &s, clean);
        if (!ga.pulling_up()) {
            EXPECT_FALSE(out.has_override);
            released = true;
            break;
        }
        EXPECT_TRUE(out.has_override);   // the override holds THROUGH the window
        ++hold_ticks;
    }
    EXPECT_TRUE(released);
    EXPECT_GE(hold_ticks, 3 * 60 - 2);  // held the window...
    EXPECT_LE(hold_ticks, 3 * 60 + 2);  // ...and not much longer
}

TEST(GroundAvoid, OscillatingRidgeDoesNotFlap) {
    // Terrain bobbing in and out of the margin band: the hold keeps the
    // recovery continuous — the module never drops the override for a
    // single clean tick inside the window.
    GroundAvoidModule ga;
    MockState s;
    s.alt_msl_ft = 1000.0;
    s.alt_agl_ft_ = 1000.0;

    Picture p = flat_sea();
    ga.update(kDt, &s, p);
    ASSERT_TRUE(ga.pulling_up());

    // Now the jet has climbed to 1,700 (inside the margin band — above
    // the floor, below floor+margin) and the terrain oscillates 0/500.
    s.alt_msl_ft = 1700.0;
    s.alt_agl_ft_ = 1700.0;
    for (int i = 0; i < 120; ++i) {
        p.terrain_here_ft = (i % 2 == 0) ? 0.0 : 500.0;
        p.terrain_ahead_ft = p.terrain_here_ft;
        const auto out = ga.update(kDt, &s, p);
        EXPECT_TRUE(ga.pulling_up());       // never releases mid-band
        EXPECT_TRUE(out.has_override);
    }
}

// ---------------------------------------------------------------------------
// Gating
// ---------------------------------------------------------------------------

TEST(GroundAvoid, OnGroundNeverPulls) {
    // A parked aircraft sits at AGL 0 by definition — the module must not
    // fight the takeoff/landing modules for it.
    GroundAvoidModule ga;
    MockState s;
    s.alt_msl_ft = 50.0;
    s.alt_agl_ft_ = 0.0;
    s.on_ground_ = true;
    ga.update(kDt, &s, flat_sea());
    EXPECT_FALSE(ga.pulling_up());
}

TEST(GroundAvoid, NoPictureMeansNoPull) {
    // A host that never pushed a terrain picture: the module idles (the
    // standalone-brain case — brains in other libraries' tests).
    GroundAvoidModule ga;
    MockState s;
    s.alt_msl_ft = 100.0;         // would be a hard pull if it could see
    s.alt_agl_ft_ = 100.0;
    Picture p;                    // valid = false
    ga.update(kDt, &s, p);
    EXPECT_FALSE(ga.pulling_up());
}

TEST(GroundAvoid, DisabledConfigNeverPulls) {
    GroundAvoidModule ga;
    auto cfg = ga.config();
    cfg.enabled = false;
    ga.set_config(cfg);
    MockState s;
    s.alt_msl_ft = 100.0;
    s.alt_agl_ft_ = 100.0;
    ga.update(kDt, &s, flat_sea());
    EXPECT_FALSE(ga.pulling_up());
}

TEST(GroundAvoid, NullStateIdles) {
    GroundAvoidModule ga;
    const auto out = ga.update(kDt, nullptr, flat_sea());
    EXPECT_FALSE(ga.pulling_up());
    EXPECT_FALSE(out.has_override);
}

// ---------------------------------------------------------------------------
// The recovery's shape
// ---------------------------------------------------------------------------

TEST(GroundAvoid, RecoveryCommandsClimb) {
    GroundAvoidModule ga;
    MockState s;
    s.alt_msl_ft = 1200.0;
    s.alt_agl_ft_ = 1200.0;
    const auto out = ga.update(kDt, &s, flat_sea());
    // Full power and a positive pitch command (the AirSteering VS
    // cascade driving a climb toward terrain + 2x floor).
    EXPECT_DOUBLE_EQ(out.throttle_cmd, 1.5);
    EXPECT_GT(out.pitch_cmd, 0.0);
    EXPECT_TRUE(out.has_override);
}

TEST(GroundAvoid, ReactTimeTuneIsConfigurable) {
    // The FreeFalcon constants are the DEFAULTS, not hardcoded: the host
    // can re-tune (a scenario's mountains, a drone's floor).
    GroundAvoidModule ga;
    auto cfg = ga.config();
    cfg.min_clearance_ft = 500.0;
    ga.set_config(cfg);
    MockState s;
    s.alt_msl_ft = 1000.0;
    s.alt_agl_ft_ = 1000.0;
    ga.update(kDt, &s, flat_sea());
    EXPECT_FALSE(ga.pulling_up());   // 1,000 > the relaxed 500 floor

    s.alt_msl_ft = 400.0;
    s.alt_agl_ft_ = 400.0;
    ga.update(kDt, &s, flat_sea());
    EXPECT_TRUE(ga.pulling_up());
}
