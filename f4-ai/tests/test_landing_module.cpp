// f4-ai/tests/test_landing_module.cpp
//
// Unit tests for LandingModule — ATC interaction, final-course geometry,
// the straight-in state chain through flare/rollout/taxi-in to parked,
// and the go-around safety valve.
//
// Airfield under test: Rwy 36 north along x=0, threshold at (0, 5000),
// 3-deg glide slope, pattern 2500 ft, DH 200 ft. The approach entry fix
// is 20,000 ft south of the threshold on the extended centerline.

#include <gtest/gtest.h>

#include <f4/ai/modules/landing_module.hpp>
#include <f4/ai/atc/stub_atc.hpp>
#include <f4/messaging/bus.hpp>
#include <f4/entities/entity.hpp>

#include <cmath>
#include <memory>

using namespace f4::ai::modules;
using namespace f4::ai::atc;
namespace messaging = f4::messaging;
namespace entities = f4::entities;
namespace geo = f4::geo;
namespace flight = f4::flight;

namespace {

constexpr double PI = 3.14159265358979323846;
constexpr double D2R = PI / 180.0;
constexpr double GS3 = 3.0 * D2R;

class TestAircraftState : public flight::IAircraftState {
public:
    double east_ft{0.0};
    double north_ft{0.0};
    double alt_msl_ft{0.0};
    double alt_agl_ft_{0.0};
    double vcas_kts_{250.0};
    double heading_rad_{0.0};
    double pitch_rad_{0.0};
    double roll_rad_{0.0};
    double vs_fpm_{0.0};
    bool on_ground_{false};

    double position_east_ft()  const override { return east_ft; }
    double position_north_ft() const override { return north_ft; }
    double altitude_msl_ft()   const override { return alt_msl_ft; }
    double altitude_agl_ft()   const override { return alt_agl_ft_; }
    double vcas_kts()          const override { return vcas_kts_; }
    double heading_rad()       const override { return heading_rad_; }
    double pitch_angle_rad()   const override { return pitch_rad_; }
    double roll_angle_rad()    const override { return roll_rad_; }
    double vertical_speed_fpm() const override { return vs_fpm_; }
    bool   on_ground()         const override { return on_ground_; }
};

// Position helpers on the final course. `dist_south` is feet before the
// threshold along the runway 36 approach course.
std::unique_ptr<TestAircraftState> on_final(double east, double dist_south,
                                            double alt_agl, double hdg = 0.0,
                                            double vcas = 160.0) {
    auto s = std::make_unique<TestAircraftState>();
    s->east_ft = east;
    s->north_ft = 5000.0 - dist_south;
    s->alt_agl_ft_ = alt_agl;
    s->alt_msl_ft = alt_agl;  // ground at 0
    s->heading_rad_ = hdg;
    s->vcas_kts_ = vcas;
    return s;
}

AirfieldConfig make_landing_config() {
    AirfieldConfig config;
    config.active_runway_id = 36;
    config.active_runway_name = "Rwy 36L";
    config.runway_heading_rad = 0.0;                       // north
    config.threshold_position = geo::WorldPosition(0.0, 5000.0, 0.0);
    config.threshold_altitude_ft = 0.0;
    config.pattern_altitude_ft = 2500.0;
    config.glide_slope_angle_rad = GS3;
    config.decision_height_ft = 200.0;
    config.runway_end_position = geo::WorldPosition(0.0, 10000.0, 0.0);
    return config;
}

struct LandingTestFixture : ::testing::Test {
    messaging::MessageBus bus;
    entities::EntityWorld world;
    StubATC atc{bus};
    LandingModule mod;

    // Entry fix 20,000 ft south of the threshold on the centerline;
    // taxi-in route: exit near mid-runway -> back to parking at origin.
    const geo::WorldPosition entry_fix{0.0, -15000.0, 0.0};

    void SetUp() override {
        atc.set_airfield(make_landing_config());
        std::vector<geo::WorldPosition> taxi_in = {
            geo::WorldPosition(100.0, 4800.0, 0.0),
            geo::WorldPosition(100.0, 1000.0, 0.0),
            geo::WorldPosition(0.0, 0.0, 0.0),
        };
        mod.configure(entry_fix, std::move(taxi_in));
        mod.initialize(1, world, bus);
    }

    // Drive ProceedToFix -> InterceptFinal -> OnFinal through the chain.
    void drive_to_on_final() {
        auto s = on_final(0.0, 19000.0, 2000.0);   // near fix
        mod.update(0.1, s.get());
        s = on_final(0.0, 15000.0, 1000.0, 0.0);   // established on centerline
        mod.update(0.1, s.get());
    }
};

} // anonymous namespace

// ============================================================================
// ATC interaction
// ============================================================================

TEST_F(LandingTestFixture, PublishesLandingRequestOnInitialize) {
    bool received = false;
    std::uint64_t id = 0;
    bus.subscribe<LandingRequest>([&](const LandingRequest& msg) {
        received = true;
        id = msg.aircraft_id;
    });
    mod.initialize(2, world, bus);  // re-init re-fires the entry action
    EXPECT_TRUE(received);
    EXPECT_EQ(id, 2u);
}

TEST_F(LandingTestFixture, ClearanceMovesToProceedToFix) {
    EXPECT_EQ(mod.state(), LandingState::ProceedToFix);
}

TEST_F(LandingTestFixture, EstablishedOnFinalRequestsLanding) {
    bool cleared = false;
    bus.subscribe<ClearedToLand>([&](const ClearedToLand&) { cleared = true; });

    // One tick near the fix while already on the centerline heading north:
    // the chained transitions carry the module FixReached -> InterceptFinal
    // -> Established -> OnFinal in a single update.
    auto s = on_final(0.0, 19000.0, 2000.0);
    mod.update(0.1, s.get());
    EXPECT_EQ(mod.state(), LandingState::OnFinal);
    EXPECT_TRUE(cleared);            // StubATC auto-granted via ApproachClearance
    EXPECT_TRUE(mod.cleared_to_land());
}

// ============================================================================
// Final-course geometry
// ============================================================================

TEST_F(LandingTestFixture, GlideSlopeTrackingCommands) {
    drive_to_on_final();
    ASSERT_EQ(mod.state(), LandingState::OnFinal);

    // The final law feeds the level-trim attitude forward and trims
    // toward the beam: 10,000 ft out the beam is ~524 ft. A big error
    // above the beam (1500 ft) commands nose-down; well below it (200 ft)
    // commands nose-up. Small errors ride the feedforward.
    auto s = on_final(0.0, 10000.0, 1500.0, 0.0);
    EXPECT_LT(mod.update(0.1, s.get()).pitch_cmd, 0.0);
    s = on_final(0.0, 10000.0, 200.0, 0.0);
    EXPECT_GT(mod.update(0.1, s.get()).pitch_cmd, 0.0);
}

TEST_F(LandingTestFixture, LocalizerCorrectionSteersLeftWhenRightOfCourse) {
    drive_to_on_final();
    ASSERT_EQ(mod.state(), LandingState::OnFinal);

    // 1000 ft right of the centerline: desired heading < runway heading
    // (correction to the left -> negative roll command from the cascade).
    auto s = on_final(1000.0, 14000.0, 800.0, 0.0);
    const auto out = mod.update(0.1, s.get());
    EXPECT_LT(out.roll_cmd, 0.0) << "right of centerline must steer left";
    EXPECT_TRUE(out.gear_handle_down);
}

// ============================================================================
// Flare / touchdown / rollout / taxi-in / parked
// ============================================================================

TEST_F(LandingTestFixture, FlareBelowFlareHeight) {
    drive_to_on_final();
    auto s = on_final(0.0, 500.0, 800.0, 0.0);  // (unused first state assumed OnFinal)
    mod.update(0.1, s.get());
    ASSERT_EQ(mod.state(), LandingState::OnFinal);

    s = on_final(0.0, 200.0, 25.0, 0.0);   // below 30 ft AGL
    const auto out = mod.update(0.1, s.get());
    EXPECT_EQ(mod.state(), LandingState::Flare);
    EXPECT_NEAR(out.throttle_cmd, 0.0, 1e-9);
    EXPECT_GT(out.pitch_cmd, 0.0);          // pitch up to the flare attitude
}

TEST_F(LandingTestFixture, TouchdownTransitionsToRolloutWithBrakes) {
    drive_to_on_final();
    auto s = on_final(0.0, 500.0, 800.0, 0.0);  // (unused first state assumed OnFinal)
    mod.update(0.1, s.get());
    s = on_final(0.0, 200.0, 25.0, 0.0);
    mod.update(0.1, s.get());
    ASSERT_EQ(mod.state(), LandingState::Flare);

    s = on_final(0.0, 100.0, 6.0, 0.0, 120.0);
    s->on_ground_ = true;
    const auto out = mod.update(0.1, s.get());
    EXPECT_EQ(mod.state(), LandingState::Rollout);
    EXPECT_TRUE(out.wheel_brakes);
    EXPECT_NEAR(out.throttle_cmd, 0.0, 1e-9);
    EXPECT_TRUE(out.gear_handle_down);
}

TEST_F(LandingTestFixture, RolloutExitsToTaxiInAndParks) {
    // Fast-forward to Rollout.
    drive_to_on_final();
    auto s = on_final(0.0, 500.0, 800.0, 0.0);  // (unused first state assumed OnFinal)
    mod.update(0.1, s.get());
    s = on_final(0.0, 200.0, 25.0, 0.0);
    mod.update(0.1, s.get());
    s = on_final(0.0, 100.0, 6.0, 0.0, 120.0);
    s->on_ground_ = true;
    mod.update(0.1, s.get());
    ASSERT_EQ(mod.state(), LandingState::Rollout);

    // Slow below the exit speed.
    s = on_final(0.0, 200.0, 6.0, 0.0, 8.0);
    s->on_ground_ = true;
    const auto out = mod.update(0.1, s.get());
    EXPECT_EQ(mod.state(), LandingState::TaxiIn);
    EXPECT_GT(out.throttle_cmd, 0.0);       // taxiing again

    // Follow the taxi-in waypoints: (100,4800) -> (100,1000) -> parking (0,0).
    // on_final(east, dist_south) maps to north = 5000 - dist_south.
    s = on_final(100.0, 210.0, 6.0, 0.0, 8.0);    // ~10 ft from wp0 (100,4800)
    s->on_ground_ = true;
    mod.update(0.1, s.get());
    EXPECT_EQ(mod.state(), LandingState::TaxiIn);

    s = on_final(100.0, 3970.0, 6.0, 0.0, 8.0);   // ~30 ft from wp1 (100,1000)
    s->on_ground_ = true;
    mod.update(0.1, s.get());
    EXPECT_EQ(mod.state(), LandingState::TaxiIn);

    s = on_final(20.0, 4980.0, 6.0, 0.0, 8.0);    // ~28 ft from parking (0,0)
    s->on_ground_ = true;
    const auto parked_out = mod.update(0.1, s.get());
    EXPECT_EQ(mod.state(), LandingState::Parked);
    EXPECT_TRUE(mod.is_complete());
    EXPECT_TRUE(parked_out.parking_brake);
    EXPECT_TRUE(parked_out.wheel_brakes);
}

// ============================================================================
// Go-around safety valve
// ============================================================================

namespace {

// Bus wiring that grants the APPROACH but never ClearedToLand.
struct NoClearedToLandFixture : ::testing::Test {
    messaging::MessageBus bus;
    entities::EntityWorld world;
    LandingModule mod;
    const geo::WorldPosition entry_fix{0.0, -15000.0, 0.0};

    void SetUp() override {
        auto cfg = make_landing_config();
        bus.subscribe<LandingRequest>([this, cfg](const LandingRequest& msg) {
            LandingClearance c;
            c.aircraft_id = msg.aircraft_id;
            c.runway_id = cfg.active_runway_id;
            c.runway_name = cfg.active_runway_name;
            c.runway_heading_rad = cfg.runway_heading_rad;
            c.threshold_position = cfg.threshold_position;
            c.threshold_altitude_ft = cfg.threshold_altitude_ft;
            c.glide_slope_angle_rad = cfg.glide_slope_angle_rad;
            c.pattern_altitude_ft = cfg.pattern_altitude_ft;
            c.decision_height_ft = cfg.decision_height_ft;
            bus.publish(c);
            // Intentionally NO response to ApproachClearance.
        });
        mod.configure(entry_fix, {});
        mod.initialize(1, world, bus);
    }
};

} // namespace

TEST_F(NoClearedToLandFixture, DescendingThroughDHUnclearedGoesAround) {
    bool went_around = false;
    bus.subscribe<GoAroundMessage>([&](const GoAroundMessage& msg) {
        if (msg.aircraft_id == 1u) went_around = true;
    });

    // Drive to OnFinal.
    auto s = on_final(0.0, 19000.0, 2000.0);
    mod.update(0.1, s.get());
    s = on_final(0.0, 15000.0, 1000.0, 0.0);
    mod.update(0.1, s.get());
    ASSERT_EQ(mod.state(), LandingState::OnFinal);
    EXPECT_FALSE(mod.cleared_to_land());

    // Descend below DH (200 ft) without clearance.
    s = on_final(0.0, 3000.0, 150.0, 0.0);
    const auto out = mod.update(0.1, s.get());
    EXPECT_EQ(mod.state(), LandingState::GoAround);
    EXPECT_TRUE(went_around);
    EXPECT_NEAR(out.throttle_cmd, 1.0, 1e-9);   // MIL power climb-away
}

TEST(LandingModuleMissed, OverflyingRunwayAirborneGoesAround) {
    messaging::MessageBus bus;
    entities::EntityWorld world;
    StubATC atc(bus);
    atc.set_airfield(make_landing_config());

    LandingModule mod;
    mod.configure(geo::WorldPosition(0.0, -15000.0, 0.0), {});
    mod.initialize(1, world, bus);

    // Drive to OnFinal, then overfly deep into the runway airborne. (The
    // missed-approach window is 4000 ft past the threshold — a normal high
    // crossing descends to the flare inside the runway; well past that
    // window still airborne is a missed approach.)
    auto s = on_final(0.0, 19000.0, 2000.0);
    mod.update(0.1, s.get());
    s = on_final(0.0, 15000.0, 1000.0, 0.0);
    mod.update(0.1, s.get());
    ASSERT_EQ(mod.state(), LandingState::OnFinal);

    s = on_final(0.0, -5000.0, 50.0, 0.0);  // 5000 ft past the threshold, airborne
    mod.update(0.1, s.get());
    EXPECT_EQ(mod.state(), LandingState::GoAround);
    EXPECT_TRUE(mod.going_around());
}

TEST(LandingModuleMissed, HighThresholdCrossingStillFlares) {
    // Crossing the threshold high but on a descent must NOT immediately
    // go around — the flare runs inside the runway.
    messaging::MessageBus bus;
    entities::EntityWorld world;
    StubATC atc(bus);
    atc.set_airfield(make_landing_config());

    LandingModule mod;
    mod.configure(geo::WorldPosition(0.0, -15000.0, 0.0), {});
    mod.initialize(1, world, bus);

    auto s = on_final(0.0, 19000.0, 2000.0);
    mod.update(0.1, s.get());
    s = on_final(0.0, 15000.0, 1000.0, 0.0);
    mod.update(0.1, s.get());
    ASSERT_EQ(mod.state(), LandingState::OnFinal);

    // 1500 ft past the threshold at 250 ft — descending, cleared: keep flying.
    s = on_final(0.0, -1500.0, 250.0, 0.0);
    mod.update(0.1, s.get());
    EXPECT_EQ(mod.state(), LandingState::OnFinal);
}

// ============================================================================
// Null state safety + names
// ============================================================================

TEST(LandingModuleMisc, NullStateIsSafe) {
    messaging::MessageBus bus;
    entities::EntityWorld world;
    LandingModule mod;
    mod.configure(geo::WorldPosition(0.0, -15000.0, 0.0), {});
    mod.initialize(1, world, bus);
    const auto out = mod.update(0.1, nullptr);
    EXPECT_TRUE(std::isfinite(out.pitch_cmd));
}

TEST(LandingModuleMisc, Names) {
    LandingModule mod;
    EXPECT_EQ(mod.mode_name(), "LandingMode");
    EXPECT_EQ(mod.state_name(), "RequestApproach");
}

TEST(LandingModuleMisc, EmptyTaxiInRouteParksOnRunway) {
    // With no taxi-in route, Rollout slows and goes straight to Parked.
    messaging::MessageBus bus;
    entities::EntityWorld world;
    StubATC atc(bus);
    atc.set_airfield(make_landing_config());

    LandingModule mod;
    mod.configure(geo::WorldPosition(0.0, -15000.0, 0.0), {});
    mod.initialize(1, world, bus);

    // Drive ProceedToFix -> InterceptFinal -> OnFinal manually.
    auto s = on_final(0.0, 19000.0, 2000.0);
    mod.update(0.1, s.get());
    s = on_final(0.0, 15000.0, 1000.0, 0.0);
    mod.update(0.1, s.get());
    ASSERT_EQ(mod.state(), LandingState::OnFinal);

    s = on_final(0.0, 200.0, 25.0, 0.0);
    mod.update(0.1, s.get());
    s = on_final(0.0, 100.0, 6.0, 0.0, 120.0);
    s->on_ground_ = true;
    mod.update(0.1, s.get());
    ASSERT_EQ(mod.state(), LandingState::Rollout);

    s = on_final(0.0, 200.0, 6.0, 0.0, 8.0);
    s->on_ground_ = true;
    mod.update(0.1, s.get());
    EXPECT_EQ(mod.state(), LandingState::Parked);
    EXPECT_TRUE(mod.is_complete());
}
