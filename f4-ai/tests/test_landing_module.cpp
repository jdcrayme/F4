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
    // STAB-E23/E45: the establish gate now requires being within 300 ft
    // of the glide BEAM and settled (|vs| < 900) — spawn ON the beam:
    // at 19,000 ft out the aim-point beam is (19000+1500)*tan(3 deg)
    // ≈ 1,077 ft; at 15,000 it is ≈ 866 ft.
    void drive_to_on_final() {
        auto s = on_final(0.0, 19000.0, 1100.0);   // near fix, on the beam
        mod.update(0.1, s.get());
        s = on_final(0.0, 15000.0, 900.0, 0.0);    // established on centerline+beam
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
    // -> Established -> OnFinal in a single update. STAB-E23/E45: the state
    // must be ON the beam (~1,077 ft at 19,000 ft out) and settled — the
    // gate refuses high/unsettled handoffs.
    auto s = on_final(0.0, 19000.0, 1100.0);
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

    // The final law feeds the beam rate (STAB-E6) and trims toward the
    // beam: 10,000 ft out the aim-point beam is (10000+1500)*tan(3 deg)
    // ≈ 603 ft. A big error above the beam (1500 ft) commands nose-down;
    // well below it (200 ft) commands nose-up. The VS command SLEWS
    // (STAB-E29), so give each state ~2 s (120 ticks) for the command
    // to reflect the error before asserting the stick direction.
    auto s = on_final(0.0, 10000.0, 1500.0, 0.0);
    double high_cmd = 0.0;
    for (int i = 0; i < 120; ++i) high_cmd = mod.update(0.1, s.get()).pitch_cmd;
    EXPECT_LT(high_cmd, 0.0) << "high on the beam must command nose-down";
    s = on_final(0.0, 10000.0, 200.0, 0.0);
    double low_cmd = 0.0;
    for (int i = 0; i < 120; ++i) low_cmd = mod.update(0.1, s.get()).pitch_cmd;
    EXPECT_GT(low_cmd, 0.0) << "low on the beam must command nose-up";
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

TEST_F(LandingTestFixture, LocalizerInterceptLeadForLargeOffset) {
    // Phase B2 (FLIGHT_CONTROL_NEXT_STEPS.md §4 Phase B2): when far from the
    // centerline (|xtrack| > intercept_offset_ft, default 1000 ft), the
    // localizer uses a direct intercept heading (aiming at a point
    // intercept_lead_ft ahead on the centerline) instead of the saturated
    // proportional correction. The aircraft should still steer toward the
    // centerline, but the heading command is computed differently.
    drive_to_on_final();
    ASSERT_EQ(mod.state(), LandingState::OnFinal);

    // 3000 ft right of the centerline — well into the intercept-lead regime.
    auto s = on_final(3000.0, 30000.0, 800.0, 0.0);
    const auto out = mod.update(0.1, s.get());
    EXPECT_LT(out.roll_cmd, 0.0)
        << "right of centerline must steer left, even at large offset";
}

TEST_F(LandingTestFixture, LocalizerInterceptSignFlipsWithOffsetSide) {
    // The intercept lead angle sign must match the offset side: right of
    // centerline -> steer left (negative roll); left of centerline -> steer
    // right (positive roll). Both at 3000 ft offset (well into the intercept
    // regime).
    drive_to_on_final();
    ASSERT_EQ(mod.state(), LandingState::OnFinal);

    auto s_right = on_final(3000.0, 30000.0, 800.0, 0.0);
    const auto out_right = mod.update(0.1, s_right.get());
    EXPECT_LT(out_right.roll_cmd, 0.0) << "right of centerline -> steer left";

    auto s_left = on_final(-3000.0, 30000.0, 800.0, 0.0);
    const auto out_left = mod.update(0.1, s_left.get());
    EXPECT_GT(out_left.roll_cmd, 0.0) << "left of centerline -> steer right";
}

// ============================================================================
// Flare / touchdown / rollout / taxi-in / parked
// ============================================================================

TEST_F(LandingTestFixture, FlareBelowFlareHeight) {
    drive_to_on_final();
    auto s = on_final(0.0, 500.0, 800.0, 0.0);  // (unused first state assumed OnFinal)
    mod.update(0.1, s.get());
    ASSERT_EQ(mod.state(), LandingState::OnFinal);

    // Phase C4: the flare law now predicts the touchdown point. Use a
    // realistic approach sink rate (-700 fpm) so the predicted touchdown
    // lands within the runway bounds (otherwise the new energy-managed
    // flare correctly triggers a go-around). At 25 ft AGL, 200 ft before
    // threshold, 160 kts, -700 fpm: time_to_ground=2.14s, td_distance=577 ft,
    // td_along=-200+577=377 ft, well within missed_along_ft (2500).
    s = on_final(0.0, 200.0, 25.0, 0.0);
    s->vs_fpm_ = -700.0;
    const auto out = mod.update(0.1, s.get());
    EXPECT_EQ(mod.state(), LandingState::Flare);
    EXPECT_NEAR(out.throttle_cmd, 0.0, 1e-9);
    EXPECT_GT(out.pitch_cmd, 0.0);          // pitch up to the flare attitude
}

TEST_F(LandingTestFixture, OnFinalExtendsFlaps) {
    // Phase C2 (FLIGHT_CONTROL_NEXT_STEPS.md §4 Phase C2): OnFinal commands
    // landing-flap configuration (TEF + LEF) every tick. The FM actuates
    // the actual surfaces from PilotInput.tefCmd/lefCmd.
    drive_to_on_final();
    ASSERT_EQ(mod.state(), LandingState::OnFinal);

    auto s = on_final(0.0, 5000.0, 1500.0, 0.0);
    const auto out = mod.update(0.1, s.get());
    EXPECT_GT(out.tef_cmd, 0.0) << "OnFinal should command TEF extension";
    EXPECT_GT(out.lef_cmd, 0.0) << "OnFinal should command LEF extension";
}

TEST_F(LandingTestFixture, FlareHoldsFlapsExtended) {
    // Phase C2: flaps stay extended through the flare (don't retract until
    // rollout slows the aircraft).
    drive_to_on_final();
    auto s = on_final(0.0, 500.0, 800.0, 0.0);
    mod.update(0.1, s.get());
    s = on_final(0.0, 200.0, 25.0, 0.0);  // below flare height
    const auto out = mod.update(0.1, s.get());
    ASSERT_EQ(mod.state(), LandingState::Flare);
    EXPECT_GT(out.tef_cmd, 0.0) << "Flare should keep TEF extended";
    EXPECT_GT(out.lef_cmd, 0.0) << "Flare should keep LEF extended";
}

TEST_F(LandingTestFixture, EnergyManagedFlareModulatesPitchOnLongPrediction) {
    // Phase C4 (FLIGHT_CONTROL_NEXT_STEPS.md §4 Phase C4): the flare law
    // predicts the touchdown point and modulates pitch to manage energy.
    // When the aircraft is fast (will land long), the flare should command
    // MORE pitch (to bleed energy); when slow (will land short), LESS pitch.
    //
    // Test setup: aircraft approaches the threshold, descends through the
    // flare height (60 ft AGL) at two different approach speeds. The
    // high-energy case (250 kts) commands more pitch than the baseline
    // (160 kts) because the predicted touchdown is farther past the aim.

    // Step 1: drive to OnFinal. The on_enter(OnFinal) action publishes
    // ApproachClearance; the StubATC responds with ClearedToLand. Match
    // the existing FlareBelowFlareHeight test pattern: 1 tick above DH
    // (200 ft) is sufficient for the clearance to propagate.
    drive_to_on_final();
    auto s_high_cruise = on_final(0.0, 800.0, 500.0, 0.0, 250.0);
    s_high_cruise->vs_fpm_ = -500.0;
    mod.update(0.1, s_high_cruise.get());

    // Step 2: descend to flare height (50 ft AGL). The flare transition fires.
    // on_final signature: on_final(east, dist_south, alt_agl, hdg, vcas)
    auto s_high = on_final(0.0, 500.0, 50.0, 0.0, 250.0);
    s_high->vs_fpm_ = -500.0;
    const auto out_high = mod.update(0.1, s_high.get());
    ASSERT_EQ(mod.state(), LandingState::Flare)
        << "expected flare transition at 50 ft AGL; got state="
        << static_cast<int>(mod.state());

    // Reset and run the baseline case at 160 kts.
    SetUp();
    drive_to_on_final();
    auto s_base_cruise = on_final(0.0, 800.0, 500.0, 0.0, 160.0);
    s_base_cruise->vs_fpm_ = -500.0;
    mod.update(0.1, s_base_cruise.get());

    auto s_base = on_final(0.0, 500.0, 50.0, 0.0, 160.0);
    s_base->vs_fpm_ = -500.0;
    const auto out_base = mod.update(0.1, s_base.get());
    ASSERT_EQ(mod.state(), LandingState::Flare);

    EXPECT_GT(out_high.pitch_cmd, out_base.pitch_cmd)
        << "high-energy flare should command MORE pitch to bleed energy "
        << "(high=" << out_high.pitch_cmd << ", base=" << out_base.pitch_cmd << ")";
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

// ============================================================================
// ============================================================================
// Traffic pattern (visual approach: upwind join -> crosswind -> downwind
// -> base -> final)
// ============================================================================
//
// Geometry under test (Rwy 36, threshold (0,5000), left traffic) —
// STAB-E49/E52 constants: pattern_offset 12000, upwind_along 16000,
// base_turn_along 28000, base_capture 9000. Leg captures are plane
// crossings (lead 1500 ft):
//   leg 0 (upwind overfly):  along > 14500  -> north > 19500
//   leg 1 (crosswind widen): east  < -10500
//   leg 2 (downwind):        along < -26500 -> north < -21500
// base capture |east| < 9000 (plus the E42 health gate: settled + agl
// > 700).

namespace {

/// Position helper: absolute ENU placement (not final-course relative).
std::unique_ptr<TestAircraftState> at_pos(double east, double north,
                                          double alt_ft, double hdg = 0.0,
                                          double vcas = 250.0) {
    auto s = std::make_unique<TestAircraftState>();
    s->east_ft = east;
    s->north_ft = north;
    s->alt_agl_ft_ = alt_ft;
    s->alt_msl_ft = alt_ft;
    s->heading_rad_ = hdg;
    s->vcas_kts_ = vcas;
    return s;
}

struct PatternTestFixture : ::testing::Test {
    messaging::MessageBus bus;
    entities::EntityWorld world;
    StubATC atc{bus};
    LandingModule mod;

    void SetUp() override {
        atc.set_airfield(make_landing_config());
        std::vector<geo::WorldPosition> taxi_in = {
            geo::WorldPosition(100.0, 4800.0, 0.0),
            geo::WorldPosition(0.0, 0.0, 0.0),
        };
        mod.configure(geo::WorldPosition(0.0, -15000.0, 0.0),
                      std::move(taxi_in));
        mod.fly_traffic_pattern = true;
        mod.initialize(1, world, bus);
    }
};

} // anonymous namespace

TEST_F(PatternTestFixture, FixReachedEntersPatternNotIntercept) {
    // Near the entry fix, pattern mode: expect PatternDownwind (not the
    // straight-in InterceptFinal).
    auto s = at_pos(0.0, -14500.0, 2500.0);   // ~500 ft from the fix
    mod.update(0.1, s.get());
    EXPECT_EQ(mod.state(), LandingState::PatternDownwind);
}

TEST_F(PatternTestFixture, PatternWalksUpwindCrosswindDownwindBaseFinal) {
    bool cleared = false;
    bus.subscribe<ClearedToLand>([&](const ClearedToLand&) { cleared = true; });

    // Enter the pattern from the fix (south, inbound heading).
    auto s = at_pos(0.0, -14500.0, 2500.0);
    mod.update(0.1, s.get());
    ASSERT_EQ(mod.state(), LandingState::PatternDownwind);

    // Leg 0 (upwind overfly): before the far-corner plane -> holds.
    s = at_pos(-3000.0, 12000.0, 2500.0);   // along = 7000 < 14500
    mod.update(0.1, s.get());
    ASSERT_EQ(mod.state(), LandingState::PatternDownwind);

    // Past the far-corner plane: leg 0 -> 1 (crosswind).
    s = at_pos(-3000.0, 20000.0, 2500.0);   // along = 15000 > 14500
    mod.update(0.1, s.get());
    ASSERT_EQ(mod.state(), LandingState::PatternDownwind);

    // Leg 1 (crosswind widen): WEST of -10500 ft captures (left traffic).
    s = at_pos(-11000.0, 20000.0, 2500.0);
    mod.update(0.1, s.get());
    ASSERT_EQ(mod.state(), LandingState::PatternDownwind);

    // Leg 2 (downwind): before the base plane -> holds.
    s = at_pos(-12000.0, 0.0, 2500.0);      // along = -5000 > -26500
    mod.update(0.1, s.get());
    ASSERT_EQ(mod.state(), LandingState::PatternDownwind);

    // Past the base-turn plane: turn base.
    s = at_pos(-12000.0, -22000.0, 2000.0, PI);   // along = -27000, south
    mod.update(0.1, s.get());
    ASSERT_EQ(mod.state(), LandingState::PatternBase);

    // Still outside the base-capture window (12000 ft lateral): hold base.
    s = at_pos(-12000.0, -24000.0, 1200.0, PI / 2);   // heading east (base)
    mod.update(0.1, s.get());
    ASSERT_EQ(mod.state(), LandingState::PatternBase);

    // Within 9000 ft of the centerline (and healthy per E42): base -> final.
    s = at_pos(-8000.0, -26000.0, 1000.0, PI / 2);
    mod.update(0.1, s.get());
    ASSERT_EQ(mod.state(), LandingState::InterceptFinal);

    // Established inbound on the centerline, ON the beam and settled
    // (STAB-E23/E45: 4000 ft out the beam is ~288 ft): OnFinal + request.
    s = at_pos(-300.0, 1000.0, 300.0, 0.2);   // along = -4000, beam ~288
    mod.update(0.1, s.get());
    EXPECT_EQ(mod.state(), LandingState::OnFinal);
    EXPECT_TRUE(cleared);
}

TEST_F(PatternTestFixture, BasePastThresholdGoesAround) {
    // Enter + walk to base quickly via direct placements.
    auto s = at_pos(0.0, -14500.0, 2500.0);
    mod.update(0.1, s.get());
    s = at_pos(-3000.0, 20000.0, 2500.0);
    mod.update(0.1, s.get());
    s = at_pos(-11000.0, 20000.0, 2500.0);
    mod.update(0.1, s.get());
    s = at_pos(-12000.0, -22000.0, 2000.0, PI);
    mod.update(0.1, s.get());
    ASSERT_EQ(mod.state(), LandingState::PatternBase);

    // On base but already past the threshold (north of it): missed.
    bool went_around = false;
    bus.subscribe<GoAroundMessage>([&](const GoAroundMessage& msg) {
        if (msg.aircraft_id == 1u) went_around = true;
    });
    s = at_pos(-4000.0, 6000.0, 1500.0, PI / 2);   // along = +1000 ft
    mod.update(0.1, s.get());
    EXPECT_EQ(mod.state(), LandingState::GoAround);
    EXPECT_TRUE(went_around);
}

TEST_F(PatternTestFixture, RightTrafficMirrorsToTheEastSide) {
    mod.pattern_left_traffic = false;

    auto s = at_pos(0.0, -14500.0, 2500.0);
    mod.update(0.1, s.get());
    ASSERT_EQ(mod.state(), LandingState::PatternDownwind);

    // Far-corner plane is side-independent.
    s = at_pos(3000.0, 20000.0, 2500.0);
    mod.update(0.1, s.get());
    ASSERT_EQ(mod.state(), LandingState::PatternDownwind);   // leg 0 -> 1

    // Crosswind capture mirrors: EAST of +10500 ft.
    s = at_pos(11000.0, 20000.0, 2500.0);
    mod.update(0.1, s.get());
    ASSERT_EQ(mod.state(), LandingState::PatternDownwind);   // leg 1 -> 2

    // Downwind plane: same along axis, mirrored side.
    s = at_pos(12000.0, -22000.0, 2000.0, 0.0);
    mod.update(0.1, s.get());
    EXPECT_EQ(mod.state(), LandingState::PatternBase);
}

TEST_F(PatternTestFixture, PatternBaseCommandsGearDown) {
    auto s = at_pos(0.0, -14500.0, 2500.0);
    mod.update(0.1, s.get());
    s = at_pos(-3000.0, 20000.0, 2500.0);
    mod.update(0.1, s.get());
    s = at_pos(-11000.0, 20000.0, 2500.0);
    mod.update(0.1, s.get());
    s = at_pos(-12000.0, -22000.0, 2000.0, PI);
    const auto out = mod.update(0.1, s.get());
    ASSERT_EQ(mod.state(), LandingState::PatternBase);
    EXPECT_TRUE(out.gear_handle_down);
}
