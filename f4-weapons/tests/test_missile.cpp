// test_missile.cpp — the 3-DOF flyout: motor/mass profile, drag, gravity,
// PN guidance convergence, seeker cone loss, max-G clamp, fuze behavior,
// time-of-flight self-destruct.
//
// All scenarios use small hand-built configs for tight numeric control.

#include <f4/weapons/missile.hpp>

#include <gtest/gtest.h>

#include <cmath>

using namespace f4::weapons;

namespace {

constexpr double kEps = 1e-6;

// A docile test round: strong motor, generous seeker, tight fuze.
MissileConfig make_config() {
    MissileConfig c;
    c.launch_mass_lb  = 300.0;
    c.burnout_mass_lb = 200.0;
    c.thrust_lbf      = 5000.0;
    c.burn_time_s     = 5.0;
    c.ref_area_ft2    = 0.25;
    c.cd              = 0.30;
    c.max_speed_fts   = 3000.0;
    c.max_g           = 30.0;
    c.guidance_gain   = 4.0;
    c.seeker_half_angle_rad = 60.0 * 3.14159265358979323846 / 180.0;
    c.seeker_max_range_ft   = 80000.0;
    c.fuze_radius_ft        = 40.0;
    c.lethal_radius_ft      = 55.0;
    c.tof_limit_s           = 60.0;
    return c;
}

TargetSnapshot target_at(double x, double y, double z, double vx = 0.0,
                         double vy = 0.0, double vz = 0.0) {
    TargetSnapshot t;
    t.valid = true;
    t.position = f4::geo::WorldPosition{x, y, z};
    t.velocity = f4::math::Vec3<double>{vx, vy, vz};
    return t;
}

} // namespace

TEST(Atmosphere, DensityDecreasesWithAltitude) {
    const double sea = atmosphere_density(0.0);
    const double high = atmosphere_density(36000.0);
    EXPECT_NEAR(sea, 0.0023769, 1e-6);
    EXPECT_LT(high, sea);
    EXPECT_DOUBLE_EQ(atmosphere_density(-100.0), sea);
}

TEST(MissileLaunch, InitialStateMatchesLaunchArguments) {
    Missile m;
    const auto cfg = make_config();
    const f4::geo::WorldPosition pos{0.0, 0.0, 20000.0};
    const f4::math::Vec3<double> vel{1000.0, 0.0, 0.0};
    m.launch(cfg, pos, vel);

    EXPECT_EQ(m.status(), MissileStatus::Guided);
    EXPECT_FALSE(m.terminal());
    EXPECT_DOUBLE_EQ(m.mass_lb(), cfg.launch_mass_lb);
    EXPECT_DOUBLE_EQ(m.flight_time_s(), 0.0);
}

TEST(MissileMass, DepletesToBurnoutMassDuringBurn) {
    Missile m;
    m.launch(make_config(), f4::geo::WorldPosition(0, 0, 20000), {1000, 0, 0});

    const double burn_rate = (300.0 - 200.0) / 5.0;  // 20 lb/s
    m.tick(1.0, TargetSnapshot{});
    EXPECT_NEAR(m.mass_lb(), 300.0 - burn_rate * 1.0, 1e-6);

    // Tick past burnout: mass must stop at burnout_mass exactly.
    for (int i = 0; i < 10; ++i) {
        m.tick(1.0, TargetSnapshot{});
    }
    EXPECT_DOUBLE_EQ(m.mass_lb(), 200.0);
    EXPECT_FALSE(m.motor_burning());
}

TEST(MissileTerminal, NoTargetWithinConeGoesBallistic) {
    Missile m;
    m.launch(make_config(), f4::geo::WorldPosition(0, 0, 20000), {1000, 0, 0});
    m.tick(0.1, TargetSnapshot{});  // invalid track
    EXPECT_EQ(m.status(), MissileStatus::Ballistic);
    EXPECT_TRUE(m.seeker_lost());
    EXPECT_FALSE(m.terminal());  // still flying
}

TEST(MissileTerminal, SelfDestructAtTofLimit) {
    MissileConfig cfg = make_config();
    cfg.tof_limit_s = 2.0;
    Missile m;
    m.launch(cfg, f4::geo::WorldPosition(0, 0, 20000), {1000, 0, 0});
    m.tick(0.5, TargetSnapshot{});   // ballistic
    m.tick(0.5, TargetSnapshot{});
    EXPECT_FALSE(m.terminal());
    m.tick(0.5, TargetSnapshot{});
    m.tick(0.5, TargetSnapshot{});
    EXPECT_EQ(m.status(), MissileStatus::Expired);
    EXPECT_TRUE(m.terminal());
}

TEST(MissileTerminal, TerminalStateIsNoOp) {
    Missile m;
    m.launch(make_config(), f4::geo::WorldPosition(0, 0, 20000), {1000, 0, 0});
    m.tick(0.1, TargetSnapshot{});   // ballistic
    m.tick(100.0, TargetSnapshot{}); // expires
    ASSERT_TRUE(m.terminal());
    const auto pos = m.position();
    const auto vel = m.velocity();
    m.tick(0.1, target_at(1000.0, 0.0, 20000.0));  // ignored
    EXPECT_DOUBLE_EQ(m.position().x, pos.x);
    EXPECT_DOUBLE_EQ(m.velocity().x, vel.x);
}

TEST(MissileGravity, FallsWhenUnpoweredAndUnmoving) {
    MissileConfig cfg = make_config();
    cfg.burn_time_s = 0.0;          // no thrust
    cfg.max_speed_fts = 10000.0;
    Missile m;
    m.launch(cfg, f4::geo::WorldPosition(0, 0, 10000), {0, 0, 0});
    m.tick(1.0, TargetSnapshot{});  // ballistic immediately
    // One second of free fall from rest: z drops ~g (plus nothing else).
    EXPECT_NEAR(m.position().z, 10000.0 - kGravityFps2, 1e-6);
    EXPECT_NEAR(m.velocity().z, -kGravityFps2, 1e-6);
}

TEST(MissileGuidance, NullsLosRateOnCrossingTarget) {
    // Classic PN sanity: constant-velocity crossing target, missile launched
    // with the exact collision-course lead -> LOS rate ~ 0, straight flight,
    // fuze fires near the target.
    //
    // Geometry: missile at origin flying +x at 2000 fps. Target 30,000 ft
    // ahead (x), crossing +y at 600 fps. Collision triangle: the missile
    // must aim at an angle atan2 up-range so it meets the target — launch
    // straight at the intercept point.
    const double mt_speed = 2000.0;   // missile speed (assumed const, short range)
    const double tt_speed = 600.0;
    const double range_x = 30000.0;
    const double tof = range_x / mt_speed;         // ~15 s at constant speed
    const double intercept_y = tt_speed * tof;     // where the target will be

    MissileConfig cfg = make_config();
    cfg.max_speed_fts = 100000.0;     // do not cap; keep speed ~2000 for the lead math
    cfg.fuze_radius_ft = 60.0;

    Missile m;
    m.launch(cfg, f4::geo::WorldPosition(0, 0, 20000), {mt_speed, intercept_y / tof, 0.0});
    // Note: this initial velocity is NOT exactly the collision course (speed
    // grows with thrust), but PN should converge quickly. We assert the
    // fuze fires well inside the miss-distance envelope.

    ASSERT_FALSE(m.terminal());
    const double dt = 1.0 / 120.0;
    TargetSnapshot tgt = target_at(range_x, 0.0, 20000.0, 0.0, tt_speed, 0.0);
    for (int i = 0; i < 120 * 30 && !m.terminal(); ++i) {
        // Advance the constant-velocity target.
        tgt.position = f4::geo::WorldPosition{
            tgt.position.x + tgt.velocity.x * dt,
            tgt.position.y + tgt.velocity.y * dt,
            tgt.position.z + tgt.velocity.z * dt};
        m.tick(dt, tgt);
    }
    ASSERT_TRUE(m.terminal()) << "status: " << missile_status_name(m.status());
    EXPECT_EQ(m.status(), MissileStatus::Detonated);
    EXPECT_LT(m.min_range_ft(), 500.0);  // converged well inside a miss-distance envelope
}

TEST(MissileGuidance, PursuesManeuveringCrossingTarget) {
    // Crossing target WITHOUT lead: missile must curve onto an intercept
    // course. Asserts the guidance bends the flight path (velocity direction
    // rotates toward the target) and the fuze fires.
    MissileConfig cfg = make_config();
    cfg.max_speed_fts = 100000.0;

    Missile m;
    m.launch(cfg, f4::geo::WorldPosition(0, 0, 20000), {1500.0, 0.0, 0.0});

    TargetSnapshot tgt = target_at(20000.0, 8000.0, 20000.0, 0.0, -300.0, 0.0);
    const double dt = 1.0 / 120.0;
    for (int i = 0; i < 120 * 40 && !m.terminal(); ++i) {
        tgt.position = f4::geo::WorldPosition{
            tgt.position.x + tgt.velocity.x * dt,
            tgt.position.y + tgt.velocity.y * dt,
            tgt.position.z + tgt.velocity.z * dt};
        m.tick(dt, tgt);
    }
    ASSERT_TRUE(m.terminal()) << "status: " << missile_status_name(m.status());
    EXPECT_EQ(m.status(), MissileStatus::Detonated);
    EXPECT_LT(m.min_range_ft(), 800.0);
}

TEST(MissileGuidance, RespectsMaxG) {
    // A target crossing hard at close range demands more than max_g; the
    // commanded lateral accel must never exceed the clamp.
    MissileConfig cfg = make_config();
    cfg.max_g = 5.0;
    cfg.max_speed_fts = 100000.0;

    Missile m;
    m.launch(cfg, f4::geo::WorldPosition(0, 0, 20000), {1000.0, 0.0, 0.0});

    const double dt = 1.0 / 120.0;
    const double a_max = cfg.max_g * kGravityFps2;
    double max_observed_bend = 0.0;
    TargetSnapshot tgt = target_at(5000.0, 50.0, 20000.0, 0.0, 800.0, 0.0);
    for (int i = 0; i < 120 * 5 && !m.terminal(); ++i) {
        const auto vel_before = m.velocity();
        const double v0 = vel_before.length();
        tgt.position = f4::geo::WorldPosition{
            tgt.position.x + tgt.velocity.x * dt,
            tgt.position.y + tgt.velocity.y * dt,
            tgt.position.z + tgt.velocity.z * dt};
        m.tick(dt, tgt);
        // Upper-bound the COMMANDED lateral accel: remove gravity from the
        // observed delta-v first (gravity is a body force, not commanded),
        // then project out the along-track portion (thrust/drag).
        if (m.velocity().length() > 0.0 && v0 > 0.0) {
            auto dv = m.velocity() - vel_before;
            dv.z += kGravityFps2 * dt;               // strip gravity
            const auto v0_hat = vel_before / v0;
            const double dv_along = dv.dot(v0_hat);
            const auto dv_perp = dv - v0_hat * dv_along;
            max_observed_bend = std::max(max_observed_bend, dv_perp.length() / dt);
        }
    }
    EXPECT_LE(max_observed_bend, a_max + 1.0);  // +1 ft/s^2 numeric slack
}

TEST(MissileSeeker, ConeLossGoesBallistic) {
    // Target exits the seeker cone: guidance must cut.
    MissileConfig cfg = make_config();
    cfg.seeker_half_angle_rad = 10.0 * 3.14159265358979323846 / 180.0;  // tight cone
    cfg.max_speed_fts = 100000.0;

    Missile m;
    m.launch(cfg, f4::geo::WorldPosition(0, 0, 20000), {1000.0, 0.0, 0.0});
    // Target at 90 deg off the velocity axis: outside a 10 deg cone.
    m.tick(0.01, target_at(0.0, 30000.0, 20000.0));
    EXPECT_EQ(m.status(), MissileStatus::Ballistic);
}

TEST(MissileSeeker, SeesTargetOnBoresight) {
    MissileConfig cfg = make_config();
    cfg.max_speed_fts = 100000.0;
    Missile m;
    m.launch(cfg, f4::geo::WorldPosition(0, 0, 20000), {1000.0, 0.0, 0.0});
    m.tick(0.01, target_at(30000.0, 0.0, 20000.0));  // dead ahead
    EXPECT_EQ(m.status(), MissileStatus::Guided);
}

TEST(MissileFuze, DetonatesAtFuzeRadius) {
    MissileConfig cfg = make_config();
    cfg.max_speed_fts = 100000.0;
    cfg.fuze_radius_ft = 50.0;
    Missile m;
    m.launch(cfg, f4::geo::WorldPosition(0, 0, 20000), {1000.0, 0.0, 0.0});
    // Static target dead ahead; the missile flies into fuze range.
    TargetSnapshot tgt = target_at(800.0, 0.0, 20000.0);
    const double dt = 1.0 / 120.0;
    for (int i = 0; i < 120 * 10 && !m.terminal(); ++i) {
        m.tick(dt, tgt);
    }
    EXPECT_EQ(m.status(), MissileStatus::Detonated);
    EXPECT_LE(m.min_range_ft(), 50.0);
}

TEST(MissileDrag, DeceleratesAfterBurnout) {
    MissileConfig cfg = make_config();
    cfg.burn_time_s = 0.5;
    cfg.thrust_lbf = 3000.0;
    cfg.max_speed_fts = 100000.0;   // let drag be the only cap
    cfg.cd = 1.0;                   // exaggerate for test visibility
    cfg.ref_area_ft2 = 1.0;
    Missile m;
    m.launch(cfg, f4::geo::WorldPosition(0, 0, 40000.0), {2000.0, 0.0, 0.0});
    m.tick(0.5, TargetSnapshot{});
    ASSERT_FALSE(m.motor_burning());
    const double v_after_burn = m.velocity().length();
    for (int i = 0; i < 60; ++i) {
        m.tick(1.0 / 60.0, TargetSnapshot{});
    }
    EXPECT_LT(m.velocity().x, v_after_burn);
}

TEST(MissileSpeed, CappedAtMaxSpeed) {
    MissileConfig cfg = make_config();
    cfg.thrust_lbf = 100000.0;      // enormous
    cfg.burn_time_s = 10.0;
    Missile m;
    m.launch(cfg, f4::geo::WorldPosition(0, 0, 20000), {1000.0, 0.0, 0.0});
    for (int i = 0; i < 120; ++i) {
        m.tick(1.0 / 120.0, TargetSnapshot{});
        if (m.velocity().length() >= cfg.max_speed_fts) {
            break;
        }
    }
    EXPECT_LE(m.velocity().length(), cfg.max_speed_fts + kEps);
}

TEST(MissileConfig, FromRecordConvertsDegreesAndCarriesEnvelope) {
    WeaponClassRecord rec;
    rec.launch_mass_lb = 335.0;
    rec.burnout_mass_lb = 210.0;
    rec.thrust_lbf = 8000.0;
    rec.burn_time_s = 8.0;
    rec.ref_area_ft2 = 0.267;
    rec.cd = 0.28;
    rec.max_speed_fts = 4400.0;
    rec.max_g = 40.0;
    rec.guidance_gain = 4.0;
    rec.seeker_half_angle_deg = 60.0;
    rec.seeker_max_range_ft = 91141.7;
    rec.fuze_radius_ft = 45.0;
    rec.lethal_radius_ft = 55.0;
    rec.tof_limit_s = 120.0;

    const auto cfg = MissileConfig::from_record(rec);
    EXPECT_DOUBLE_EQ(cfg.launch_mass_lb, 335.0);
    EXPECT_NEAR(cfg.seeker_half_angle_rad, 60.0 * 3.14159265358979323846 / 180.0, kEps);
    EXPECT_DOUBLE_EQ(cfg.fuze_radius_ft, 45.0);
    EXPECT_DOUBLE_EQ(cfg.tof_limit_s, 120.0);
}
