// test_wingman_module.cpp — unit tests for the WingmanModule
// (AI_IMPLEMENTATION_PLAN.md §5 Step 11 validation table):
//   * formation station geometry per formation type (formdata.cpp)
//   * lead maneuvers, wingman follows: heading steered to the station,
//     speed law speeds up behind / slows down ahead, holds lead heading
//     once close (AiFollowLead)
//   * lead lost (dead / invalid picture) -> None, empty output
//   * rejoin: blown out past the rejoin ring -> Rejoining at rejoin
//     speed, converges back to Following
//   * formation command changes the station
//   * crude-kinematics convergence: a point-mass wingman joins station
//     within tolerance after a blowout
//
// The module is engine-agnostic: every test drives it with a mock
// IAircraftState and hand-built LeadPictures — no EntityWorld.

#include <gtest/gtest.h>

#include <cmath>
#include <memory>

#include <f4/ai/modules/wingman_module.hpp>

using namespace f4::ai;
using namespace f4::ai::modules;
namespace geo = f4::geo;
using namespace f4::flight;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double DT = 1.0 / 60.0;
constexpr double FT_PER_KT = 1.68781;

class TestAircraftState : public IAircraftState {
public:
    double east_ft{0.0};
    double north_ft{0.0};
    double alt_msl_ft{15000.0};
    double alt_agl_ft_{15000.0};
    double vcas_kts_{420.0};
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

/// A lead flying north at 420 kts from (0, 10000) at 15000 ft.
WingmanModule::LeadPicture lead_north() {
    WingmanModule::LeadPicture p;
    p.entity_id = 7;
    p.valid = true;
    p.position = geo::WorldPosition(0.0, 10000.0, 15000.0);
    p.velocity = geo::WorldPosition(0.0, 420.0 * FT_PER_KT, 0.0);
    p.heading_rad = 0.0;
    p.vcas_kts = 420.0;
    p.alt_msl_ft = 15000.0;
    return p;
}

double wrap_2pi(double a) {
    while (a < 0.0) a += 2.0 * kPi;
    while (a >= 2.0 * kPi) a -= 2.0 * kPi;
    return a;
}

} // anonymous namespace

// ============================================================================
// Station geometry (plan: "Formation type changes" / formdata.cpp)
// ============================================================================

TEST(WingmanModule, FightingWingStationIsAftRightOfLead) {
    WingmanModule wing;
    wing.set_lead_picture(lead_north());

    const auto st = wing.formation_position();
    // FightingWing: 2000 ft right (east), 2500 ft aft (south), same alt.
    EXPECT_NEAR(st.x, 2000.0, 1.0);
    EXPECT_NEAR(st.y, 10000.0 - 2500.0, 1.0);
    EXPECT_NEAR(st.z, 15000.0, 1.0);
}

TEST(WingmanModule, StationFollowsLeadHeading) {
    WingmanModule wing;
    auto p = lead_north();
    p.heading_rad = 0.5 * kPi;               // lead now faces EAST
    p.velocity = geo::WorldPosition(420.0 * FT_PER_KT, 0.0, 0.0);
    wing.set_lead_picture(p);

    const auto st = wing.formation_position();
    // Aft of an eastbound lead = west of it; right = south.
    EXPECT_NEAR(st.x, 10000.0 * 0.0 - 2500.0, 1.0);   // 2500 ft west
    EXPECT_NEAR(st.y, 10000.0 - 2000.0, 1.0);         // 2000 ft left of north
    EXPECT_NEAR(st.z, 15000.0, 1.0);
}

TEST(WingmanModule, FormationCommandMovesTheStation) {
    WingmanModule wing;
    wing.set_lead_picture(lead_north());

    wing.command_formation(FormationType::Trail);
    auto st = wing.formation_position();
    EXPECT_NEAR(st.x, 0.0, 1.0);                    // zero lateral
    EXPECT_NEAR(st.y, 10000.0 - 4000.0, 1.0);       // 4000 ft aft

    wing.command_formation(FormationType::EchelonLeft);
    st = wing.formation_position();
    EXPECT_NEAR(st.x, -3000.0, 1.0);                // LEFT side
    EXPECT_NEAR(st.y, 10000.0 - 1000.0, 1.0);       // 0.4 * 2500 aft

    wing.command_formation(FormationType::LineAbreast);
    st = wing.formation_position();
    EXPECT_NEAR(st.x, 5000.0, 1.0);                 // abeam
    EXPECT_NEAR(st.y, 10000.0, 1.0);                // level with the lead
}

TEST(WingmanModule, VerticalOffsetDropsStationBelowLead) {
    WingmanModule wing;
    wing.config().vertical_offset_ft = 500.0;
    wing.set_lead_picture(lead_north());

    const auto st = wing.formation_position();
    EXPECT_NEAR(st.z, 14500.0, 1.0);
}

// ============================================================================
// No lead -> None, empty output (the plan's LeadLost handling)
// ============================================================================

TEST(WingmanModule, NoPictureIsNoneAndEmpty) {
    WingmanModule wing;
    TestAircraftState own;

    const auto out = wing.update(DT, &own);
    EXPECT_EQ(wing.state(), WingState::None);
    EXPECT_EQ(out.pitch_cmd, 0.0);
    EXPECT_EQ(out.roll_cmd, 0.0);
    EXPECT_EQ(out.throttle_cmd, 0.0);
    EXPECT_FALSE(wing.has_live_picture());
}

TEST(WingmanModule, LeadLostDropsToNone) {
    WingmanModule wing;
    TestAircraftState own;
    own.east_ft = 2000.0;
    own.north_ft = 7500.0;   // at station

    wing.set_lead_picture(lead_north());
    wing.update(DT, &own);
    EXPECT_EQ(wing.state(), WingState::Following);

    // The lead dies: the host pushes an invalid picture.
    wing.set_lead_picture(WingmanModule::LeadPicture{});
    const auto out = wing.update(DT, &own);
    EXPECT_EQ(wing.state(), WingState::None);
    EXPECT_EQ(out.roll_cmd, 0.0);
}

// ============================================================================
// Following: the speed + heading laws (plan: "lead maneuvers, wingman
// follows" — asserted through the module's control decisions)
// ============================================================================

TEST(WingmanModule, WingmanBehindStationSpeedsUp) {
    WingmanModule wing;
    TestAircraftState own;
    wing.set_lead_picture(lead_north());

    // 5000 ft BEHIND the slot (station y = 7500).
    own.east_ft = 2000.0;
    own.north_ft = 2500.0;
    wing.update(DT, &own);

    EXPECT_GT(wing.desired_speed_kts(own), 420.0 + 50.0)
        << "a wingman 5000 ft behind the slot must run more than 50 kt hot";
}

TEST(WingmanModule, WingmanAheadOfStationSlowsDown) {
    WingmanModule wing;
    TestAircraftState own;
    wing.set_lead_picture(lead_north());

    // 5000 ft AHEAD of the slot (station y = 7500): slow down.
    own.east_ft = 2000.0;
    own.north_ft = 12500.0;
    wing.update(DT, &own);

    EXPECT_LT(wing.desired_speed_kts(own), 420.0 - 50.0)
        << "a wingman 5000 ft ahead of the slot must run more than 50 kt slow";
}

TEST(WingmanModule, OnStationHoldsLeadSpeedAndHeading) {
    WingmanModule wing;
    TestAircraftState own;
    wing.set_lead_picture(lead_north());

    // Exactly at station: match speed, hold the lead's heading.
    own.east_ft = 2000.0;
    own.north_ft = 7500.0;
    own.vcas_kts_ = 420.0;
    wing.update(DT, &own);

    EXPECT_NEAR(wing.desired_speed_kts(own), 420.0, 5.0);
    EXPECT_NEAR(wrap_2pi(wing.desired_heading_rad()), 0.0, 0.01);
}

TEST(WingmanModule, OffStationSteersTowardIt) {
    WingmanModule wing;
    TestAircraftState own;
    wing.set_lead_picture(lead_north());

    // Southwest of the station (station is NE): the desired heading is
    // the INTERCEPT, not the tail chase — the aim point advances along
    // the northbound lead's velocity by the time-to-go, so the heading
    // sits NORTH of the pure-pursuit bearing (45 deg for the
    // 6000-east/5000-north offset) and east of the lead's own track
    // (0 deg): the rejoin merges onto the formation's flight path.
    own.east_ft = -4000.0;
    own.north_ft = 2500.0;
    wing.update(DT, &own);

    const double h = wrap_2pi(wing.desired_heading_rad());
    EXPECT_GT(h, 0.05 * kPi);      // east of the lead's track...
    EXPECT_LT(h, 0.25 * kPi);      // ...north of the pure-pursuit bearing
}

// ============================================================================
// Rejoin: the blowout hysteresis
// ============================================================================

TEST(WingmanModule, BlowoutPastRejoinRingRejoins) {
    WingmanModule wing;
    TestAircraftState own;
    wing.set_lead_picture(lead_north());

    own.east_ft = 2000.0;
    own.north_ft = 7500.0;
    wing.update(DT, &own);
    EXPECT_EQ(wing.state(), WingState::Following);

    // Combat separation: 2 NM south of the slot blows the rejoin ring
    // (default 9000 ft).
    own.north_ft = 7500.0 - 12000.0;
    wing.update(DT, &own);
    EXPECT_EQ(wing.state(), WingState::Rejoining);

    // Rejoining always closes: the PD law's floor is 30 kt over the lead
    // (the P term saturates the +150 clamp this far out).
    EXPECT_GT(wing.desired_speed_kts(own), 420.0 + 25.0);

    // Converged back inside the station band: Following again.
    own.north_ft = 7500.0 - 1000.0;
    wing.update(DT, &own);
    EXPECT_EQ(wing.state(), WingState::Following);
}

// ============================================================================
// Convergence with a crude point-mass integrator (the "500 ft" goal of
// the plan, asserted at 1.5x the default tolerance for the crude model)
// ============================================================================

TEST(WingmanModule, PointMassWingmanConvergesToStation) {
    WingmanModule wing;
    TestAircraftState own;

    // Lead starts 6 kft north of a wingman that spawned well behind.
    own.east_ft = 2000.0;
    own.north_ft = 10000.0 - 2500.0 - 9000.0;   // 9 kft behind the slot
    own.vcas_kts_ = 420.0;
    own.heading_rad_ = 0.0;

    auto lead = lead_north();

    // Crude kinematics: first-order speed lag (tau 3 s) toward the
    // module's DESIRED speed + bank-to-turn (roll_cmd -> bank -> turn
    // rate at current speed).
    double speed_fps = 420.0 * FT_PER_KT;
    for (int i = 0; i < 60 * 120; ++i) {        // 120 s
        lead.position.y += lead.velocity.y * DT;
        wing.set_lead_picture(lead);

        const auto out = wing.update(DT, &own);
        ASSERT_NE(out.pitch_cmd + out.roll_cmd + out.throttle_cmd, 0.0);

        // Speed: first-order lag toward the module's desired speed.
        const double target_fps = wing.desired_speed_kts(own) * FT_PER_KT;
        const double accel = (target_fps - speed_fps) / 3.0;
        speed_fps += accel * DT;
        own.vcas_kts_ = speed_fps / FT_PER_KT;

        // Heading: crude bank-to-turn (roll command +/- 1 -> +/- 30 deg
        // bank -> turn rate at current speed).
        const double bank = 0.52 * out.roll_cmd;
        const double turn_rate = 32.2 * std::tan(bank) / std::max(speed_fps, 1.0);
        own.heading_rad_ = wrap_2pi(own.heading_rad_ + turn_rate * DT);

        own.east_ft += speed_fps * std::sin(own.heading_rad_) * DT;
        own.north_ft += speed_fps * std::cos(own.heading_rad_) * DT;
        own.alt_msl_ft = 15000.0;
    }

    const double dx = own.east_ft - 2000.0;
    const double dy = own.north_ft - (lead.position.y - 2500.0);
    const double dist = std::sqrt(dx * dx + dy * dy);
    EXPECT_LT(dist, 1.5 * wing.config().formation_tolerance_ft)
        << "wingman did not converge to station (dist = " << dist << " ft)";
    EXPECT_EQ(wing.state(), WingState::Following);
}

// ============================================================================
// Names + config surface
// ============================================================================

TEST(WingmanModule, StateAndFormationNames) {
    WingmanModule wing;
    EXPECT_EQ(wing.state_name(), "None");
    EXPECT_EQ(wing.formation_name(), "FightingWing");

    TestAircraftState own;
    own.east_ft = 2000.0;
    own.north_ft = 7500.0;
    wing.set_lead_picture(lead_north());
    wing.update(DT, &own);
    EXPECT_EQ(wing.state_name(), "Following");

    wing.command_formation(FormationType::EchelonRight);
    EXPECT_EQ(wing.formation_name(), "EchelonRight");
}

TEST(WingmanModule, ResetClearsEverything) {
    WingmanModule wing;
    TestAircraftState own;
    wing.set_lead_picture(lead_north());
    wing.update(DT, &own);
    EXPECT_NE(wing.state(), WingState::None);

    wing.reset();
    EXPECT_EQ(wing.state(), WingState::None);
    EXPECT_FALSE(wing.has_live_picture());
}

// ============================================================================
// Data-driven formations (FORMDAT.FIL via f4-data — command_formation_slot)
//
// The station math is FreeFalcon's own, ported line-for-line from
// bvrengage.cpp:3367-3379 (ENU form; see wingman_module.hpp's header
// note). These tests pin the geometry to the shipped FORMDAT.FIL values
// (spread: relAz -90 deg / 0.5 NM; trail: 180 deg / 2 NM; ladder:
// 180 deg / 45 deg el / 1.05 NM) and the radio-command modifiers
// (WMToggleSide, WMKickout, WMCloseup — wingai.cpp).
// ============================================================================

namespace {

/// The shipped "spread" formation (FORMDAT.FIL row 0): 4-ship slots at
/// -90/+90/+90 deg; the 2-ship slot inherits slot[0] (num2Slots == 0).
f4::data::Formation spread_formation() {
    f4::data::Formation f;
    f.name = "spread";
    f.form_num = 0;
    f.two_ship = f4::data::FormationSlot{-90.0, 0.0, 0.5, 0};
    f.two_ship_explicit = false;
    f.slots = {
        {-90.0, 0.0, 0.5, 0},
        {90.0, 0.0, 1.0, 0},
        {90.0, 0.0, 1.5, 0},
    };
    return f;
}

/// The shipped "trail" (row 2): the one 2-ship slot with a DEDICATED
/// triple (180 deg, 2 NM) — exercises the explicit two-ship path.
f4::data::Formation trail_formation() {
    f4::data::Formation f;
    f.name = "trail";
    f.form_num = 2;
    f.two_ship = f4::data::FormationSlot{180.0, 0.0, 2.0, 2};
    f.two_ship_explicit = true;
    f.slots = {{180.0, 0.0, 1.0, 2}, {180.0, 0.0, 2.0, 2},
               {180.0, 0.0, 3.0, 2}};
    return f;
}

/// The shipped "ladder" (row 3): relEl 45 deg — exercises the elevated
/// branch (trackZ += rangeFactor * sin(-relEl)).
f4::data::Formation ladder_formation() {
    f4::data::Formation f;
    f.name = "ladder";
    f.form_num = 3;
    f.two_ship = f4::data::FormationSlot{180.0, 45.0, 1.05, 3};
    f.two_ship_explicit = false;
    f.slots = {{180.0, 0.0, 1.0, 3}, {180.0, 30.0, 1.0, 3},
               {180.0, 60.0, 1.0, 3}};
    return f;
}

} // anonymous namespace

TEST(WingmanModule, FormdatSpreadStationIsHalfMileLeft) {
    // bvrengage.cpp:3367-3370 with the lead north (sigma = 0),
    // mFormSide = +1, spread two-ship slot relAz = -90 deg:
    //   ENU east  = range * sin(0 + 1*(-90)) = -range (LEFT of the lead)
    //   ENU north = range * cos(-90)         = 0 (abeam)
    // 0.5 NM = 3038.1055 ft (formdata.cpp's NM_TO_FT 6076.211).
    // The flat slot stacks the #2 wingman 100 ft DOWN (flightIdx 1 *
    // -100.0F, bvrengage.cpp:3378).
    WingmanModule wing;
    const auto f = spread_formation();
    wing.command_formation_slot(f);
    EXPECT_TRUE(wing.formation_slot_active());

    wing.set_lead_picture(lead_north());
    const auto st = wing.formation_position();

    const double range_ft = 0.5 * f4::data::kNmToFt;
    EXPECT_NEAR(st.x, 0.0 - range_ft, 0.5);
    EXPECT_NEAR(st.y, 10000.0, 0.5);
    EXPECT_NEAR(st.z, 15000.0 - 100.0, 0.5);
}

TEST(WingmanModule, FormdatTrailStationIsTwoMileStern) {
    // relAz 180 deg: directly behind the north-flying lead; the explicit
    // 2-ship triple (2 NM = 12152.422 ft) — twoposData, not slot[0].
    WingmanModule wing;
    const auto f = trail_formation();
    wing.command_formation_slot(f);

    wing.set_lead_picture(lead_north());
    const auto st = wing.formation_position();

    const double range_ft = 2.0 * f4::data::kNmToFt;
    EXPECT_NEAR(st.x, 0.0, 0.5);
    EXPECT_NEAR(st.y, 10000.0 - range_ft, 0.5);
    EXPECT_NEAR(st.z, 15000.0 - 100.0, 0.5);
}

TEST(WingmanModule, FormdatLadderStationIsElevatedStern) {
    // relEl 45 deg: trackZ += rangeFactor * sin(45 deg) UP (the ENU flip
    // of the reference's sin(-relEl) under Z-down). 1.05 NM = 6380.0216 ft.
    WingmanModule wing;
    const auto f = ladder_formation();
    wing.command_formation_slot(f);

    wing.set_lead_picture(lead_north());
    const auto st = wing.formation_position();

    const double range_ft = 1.05 * f4::data::kNmToFt;
    EXPECT_NEAR(st.x, 0.0, 0.5);
    EXPECT_NEAR(st.y, 10000.0 - range_ft, 0.5);
    EXPECT_NEAR(st.z, 15000.0 + range_ft * std::sin(45.0 * kPi / 180.0),
                0.5);
}

TEST(WingmanModule, KickoutDoublesFormdatRangeNotStack) {
    // WMKickout (wingai.cpp:1757): mFormLateralSpaceFactor *= 2 — the
    // lateral range doubles. The -100 ft stack does NOT scale (the
    // reference applies flightIdx * -100.0F raw — bvrengage.cpp:3378,
    // wingai.cpp:2923), which is exactly the kind of asymmetry a
    // faithful port must preserve.
    WingmanModule wing;
    const auto f = spread_formation();
    wing.command_formation_slot(f);
    wing.kickout();
    EXPECT_DOUBLE_EQ(wing.formation_space_factor(), 2.0);

    wing.set_lead_picture(lead_north());
    const auto st = wing.formation_position();

    const double range_ft = 2.0 * 0.5 * f4::data::kNmToFt;
    EXPECT_NEAR(st.x, 0.0 - range_ft, 0.5);
    EXPECT_NEAR(st.y, 10000.0, 0.5);
    EXPECT_NEAR(st.z, 15000.0 - 100.0, 0.5);
}

TEST(WingmanModule, CloseupHalvesFormdatRange) {
    // WMCloseup (wingai.cpp:1794): mFormLateralSpaceFactor *= 0.5.
    WingmanModule wing;
    const auto f = trail_formation();
    wing.command_formation_slot(f);
    wing.closeup();
    EXPECT_DOUBLE_EQ(wing.formation_space_factor(), 0.5);

    wing.set_lead_picture(lead_north());
    const auto st = wing.formation_position();

    const double range_ft = 0.5 * 2.0 * f4::data::kNmToFt;
    EXPECT_NEAR(st.x, 0.0, 0.5);
    EXPECT_NEAR(st.y, 10000.0 - range_ft, 0.5);
}

TEST(WingmanModule, ToggleSideMirrorsFormdatStation) {
    // WMToggleSide (wingai.cpp:1842-1846): mFormSide -> -1 mirrors the
    // station to the lead's other side. spread's -90 deg slot flips from
    // LEFT to RIGHT (+90).
    WingmanModule wing;
    const auto f = spread_formation();
    wing.command_formation_slot(f);
    wing.set_formation_side(false);   // mirror
    EXPECT_FALSE(wing.formation_side_right());

    wing.set_lead_picture(lead_north());
    const auto st = wing.formation_position();

    const double range_ft = 0.5 * f4::data::kNmToFt;
    EXPECT_NEAR(st.x, 0.0 + range_ft, 0.5);
    EXPECT_NEAR(st.y, 10000.0, 0.5);
}

TEST(WingmanModule, FormdatSlotFollowsLeadHeading) {
    // The station rides the LEAD's heading frame: lead heading east
    // (atan2(east, north) = +90 deg) puts the -90 deg slot NORTH of the
    // lead (90 - 90 = 0 deg bearing).
    WingmanModule wing;
    const auto f = spread_formation();
    wing.command_formation_slot(f);

    auto p = lead_north();
    p.heading_rad = kPi / 2.0;                 // east
    p.velocity = geo::WorldPosition(420.0 * FT_PER_KT, 0.0, 0.0);
    wing.set_lead_picture(p);

    const auto st = wing.formation_position();
    const double range_ft = 0.5 * f4::data::kNmToFt;
    EXPECT_NEAR(st.x, 0.0, 0.5);
    EXPECT_NEAR(st.y, 10000.0 + range_ft, 0.5);
    EXPECT_NEAR(st.z, 15000.0 - 100.0, 0.5);
}

TEST(WingmanModule, CommandFormationRevertsFormdatSlot) {
    // A formation radio command (the built-in WingManCmd path) replaces
    // the FORMDAT slot — one source of station geometry at a time.
    WingmanModule wing;
    const auto f = spread_formation();
    wing.command_formation_slot(f);
    EXPECT_TRUE(wing.formation_slot_active());
    EXPECT_EQ(wing.formation_name(), "spread");

    wing.command_formation(FormationType::FightingWing);
    EXPECT_FALSE(wing.formation_slot_active());
    EXPECT_EQ(wing.formation_name(), "FightingWing");
}

TEST(WingmanModule, FormdatSlotWithNoPictureIsEmpty) {
    // formation_position() with no live picture returns the zero
    // position regardless of the commanded slot (same contract as the
    // built-in path — the module never invents a lead).
    WingmanModule wing;
    const auto f = spread_formation();
    wing.command_formation_slot(f);
    const auto st = wing.formation_position();
    EXPECT_DOUBLE_EQ(st.x, 0.0);
    EXPECT_DOUBLE_EQ(st.y, 0.0);
    EXPECT_DOUBLE_EQ(st.z, 0.0);
}
