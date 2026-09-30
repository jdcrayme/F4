// test_fcr_page.cpp — the AVIONICS-2 FCR page pins (unit tier).
//
// The page is a MODE MACHINE + a LOCK STATE + a pure snapshot recompute
// over f4-sensors' RadarSimComponent. This file pins, in the
// test_ins.cpp discipline:
//
//   1. the mode table — every transition AND every refusal that
//      matters (power on/off, the RWS/TWS/VS mesh, self-select no-ops,
//      Off-page refusals)
//   2. the lock rules — refused with the page Off, refused for
//      untracked/Dropped targets, the lock lands through the radar's
//      own command_track (page lock == radar lock, the no-divergence
//      rule), break_lock parks the radar into Search, mode switches
//      keep the lock, PowerOff clears it
//   3. the track-death mirror — the locked track going Dropped drops
//      the page lock and parks the radar (update()'s one write)
//   4. the snapshot — page-relative azimuth wrap, slant range, signed
//      closure (positive = closing), IFF hostile, the
//      Established-or-Coasting "good track" flag, the designated flag,
//      the VS velocity-only filter, and byte-identical determinism

#include <gtest/gtest.h>

#include "f4/avionics/fcr_page.hpp"

#include <f4/geo/position.hpp>
#include <f4/math/vec3.hpp>
#include <f4/sensors/radar_component.hpp>

#include <cstdint>

using namespace f4::avionics;
namespace sensors = f4::sensors;

namespace {

// A bare radar component (no world): the page reads its published
// state — scan volume, track store, mode — and drives it through
// command_track/command_search only. The default track store owns
// team "blue", so "red" tracks read hostile and "blue" friendly.
struct RadarRig {
    sensors::RadarSimComponent radar{};

    void detect(std::uint64_t id, double x, double y, double z,
                double vx, double vy, double vz, double t,
                const char* team) {
        radar.tracks().on_detection(
            id, f4::geo::WorldPosition{x, y, z},
            f4::math::Vec3<double>{vx, vy, vz}, t, team, "");
    }

    // One scan cycle (the end-of-scan bookkeeping decay_untracked does)
    // so a second detection lands Established (2 x 0.34 >= 0.6).
    void scan_cycle(double t) { radar.tracks().decay_untracked(t); }
};

constexpr double kNm = 6076.115485;

f4::geo::WorldPosition kOwn{0.0, 0.0, 10000.0};
f4::math::Vec3<double> kOwnVel{0.0, 500.0, 0.0};   // north, 500 fps

} // namespace

// ============================================================================
// 1. The mode table
// ============================================================================

TEST(FcrPage, PowerOnFromOffSelectsRws) {
    FcrPageModel page;
    EXPECT_EQ(page.mode(), FcrState::Off);
    EXPECT_TRUE(page.power_on());
    EXPECT_EQ(page.mode(), FcrState::Rws);
}

TEST(FcrPage, PowerOnOutsideOffIsRefused) {
    FcrPageModel page;
    ASSERT_TRUE(page.power_on());
    EXPECT_FALSE(page.power_on()) << "double power-on moved the page";
    EXPECT_EQ(page.mode(), FcrState::Rws);
}

TEST(FcrPage, ModeMeshPinsEverySwitch) {
    FcrPageModel page;
    ASSERT_TRUE(page.power_on());
    EXPECT_TRUE(page.select_tws());
    EXPECT_EQ(page.mode(), FcrState::Tws);
    EXPECT_TRUE(page.select_vs());
    EXPECT_EQ(page.mode(), FcrState::Vs);
    EXPECT_TRUE(page.select_rws());
    EXPECT_EQ(page.mode(), FcrState::Rws);
    EXPECT_TRUE(page.select_vs());
    EXPECT_EQ(page.mode(), FcrState::Vs);
    EXPECT_TRUE(page.select_tws());
    EXPECT_EQ(page.mode(), FcrState::Tws);
    EXPECT_TRUE(page.select_rws());
    EXPECT_EQ(page.mode(), FcrState::Rws);
}

TEST(FcrPage, SelfSelectIsANoOpRefusal) {
    FcrPageModel page;
    ASSERT_TRUE(page.power_on());
    EXPECT_FALSE(page.select_rws()) << "RWS -> RWS should not move";
    ASSERT_TRUE(page.select_tws());
    EXPECT_FALSE(page.select_tws());
    ASSERT_TRUE(page.select_vs());
    EXPECT_FALSE(page.select_vs());
}

TEST(FcrPage, EverythingFromOffIsRefused) {
    FcrPageModel page;
    EXPECT_FALSE(page.select_rws());
    EXPECT_FALSE(page.select_tws());
    EXPECT_FALSE(page.select_vs());
    EXPECT_FALSE(page.power_off()) << "power-off while Off should not move";
    EXPECT_EQ(page.mode(), FcrState::Off);
}

TEST(FcrPage, PowerOffLandsFromEveryLiveStateAndClearsTheLock) {
    for (int i = 0; i < 3; ++i) {
        FcrPageModel page;
        RadarRig rig;
        ASSERT_TRUE(page.power_on());
        const auto id = static_cast<std::uint64_t>(101 + i);
        rig.detect(id, 0.0, 6.0 * kNm, 10000.0, 0.0, 300.0, 0.0,
                   1.0, "red");
        ASSERT_TRUE(page.designate(id, rig.radar));
        ASSERT_TRUE(page.locked());

        const bool moved = (i == 0)   ? page.select_tws()
                           : (i == 1) ? page.select_vs()
                                      : true;
        (void)moved;   // Rws keeps the lock in place for i == 2
        EXPECT_TRUE(page.power_off());
        EXPECT_EQ(page.mode(), FcrState::Off);
        EXPECT_FALSE(page.locked()) << "power-off must clear the lock";
    }
}

TEST(FcrPage, ModeSwitchKeepsTheLock) {
    FcrPageModel page;
    RadarRig rig;
    ASSERT_TRUE(page.power_on());
    rig.detect(77, 0.0, 6.0 * kNm, 10000.0, 0.0, 300.0, 0.0, 1.0, "red");
    ASSERT_TRUE(page.designate(77, rig.radar));
    ASSERT_TRUE(page.select_tws());
    ASSERT_TRUE(page.select_vs());
    ASSERT_TRUE(page.select_rws());
    EXPECT_TRUE(page.locked());
    EXPECT_EQ(page.locked_target_id(), 77u)
        << "the lock is orthogonal to the search display";
}

// ============================================================================
// 2. The lock rules (the f4-sensors hand-off)
// ============================================================================

TEST(FcrPage, DesignateIsRefusedWithThePageOff) {
    FcrPageModel page;
    RadarRig rig;
    rig.detect(9, 0.0, 6.0 * kNm, 10000.0, 0.0, 300.0, 0.0, 1.0, "red");
    EXPECT_FALSE(page.designate(9, rig.radar));
    EXPECT_FALSE(page.locked());
    EXPECT_EQ(rig.radar.mode(), sensors::RadarMode::Search)
        << "an unpowered page must not drive the radar";
}

TEST(FcrPage, DesignateIsRefusedForUntrackedOrDroppedTargets) {
    FcrPageModel page;
    RadarRig rig;
    ASSERT_TRUE(page.power_on());

    // Never detected: nothing to lock.
    EXPECT_FALSE(page.designate(1, rig.radar));

    // Detected, then Dropped: the radar's own rule refuses too — the
    // page takes that answer.
    rig.detect(2, 0.0, 6.0 * kNm, 10000.0, 0.0, 300.0, 0.0, 1.0, "red");
    rig.scan_cycle(1.0e6);   // force the track to collapse
    ASSERT_EQ(rig.radar.tracks().find(2)->state,
              sensors::TrackState::Dropped);
    EXPECT_FALSE(page.designate(2, rig.radar));
    EXPECT_FALSE(page.locked());
    EXPECT_EQ(rig.radar.mode(), sensors::RadarMode::Search);
}

TEST(FcrPage, DesignateLocksThroughTheRadarNoDivergence) {
    FcrPageModel page;
    RadarRig rig;
    ASSERT_TRUE(page.power_on());
    rig.detect(42, 0.0, 6.0 * kNm, 10000.0, 0.0, 300.0, 0.0, 1.0, "red");

    EXPECT_TRUE(page.designate(42, rig.radar));
    EXPECT_TRUE(page.locked());
    EXPECT_EQ(page.locked_target_id(), 42u);
    // The hand-off: the RADAR is in Track mode on the same id — the
    // fire-control gate's radar leg now lives on this page's decision.
    EXPECT_EQ(rig.radar.mode(), sensors::RadarMode::Track);
    EXPECT_EQ(rig.radar.locked_target(), 42u);
}

TEST(FcrPage, BreakLockParksTheRadarIntoSearch) {
    FcrPageModel page;
    RadarRig rig;
    ASSERT_TRUE(page.power_on());
    rig.detect(42, 0.0, 6.0 * kNm, 10000.0, 0.0, 300.0, 0.0, 1.0, "red");
    ASSERT_TRUE(page.designate(42, rig.radar));

    page.break_lock(rig.radar);
    EXPECT_FALSE(page.locked());
    EXPECT_EQ(page.locked_target_id(), 0u);
    EXPECT_EQ(rig.radar.mode(), sensors::RadarMode::Search);
    EXPECT_EQ(rig.radar.locked_target(), 0u);

    // Break with no lock is a no-op (and stays Search).
    page.break_lock(rig.radar);
    EXPECT_EQ(rig.radar.mode(), sensors::RadarMode::Search);
}

TEST(FcrPage, LockedTrackDeathDropsThePageLockAndParksTheRadar) {
    FcrPageModel page;
    RadarRig rig;
    ASSERT_TRUE(page.power_on());
    rig.detect(42, 0.0, 6.0 * kNm, 10000.0, 0.0, 300.0, 0.0, 1.0, "red");
    ASSERT_TRUE(page.designate(42, rig.radar));
    ASSERT_EQ(rig.radar.mode(), sensors::RadarMode::Track);

    // The track dies (the target splashed / gone): the page observes
    // the Dropped track on the next update and mirrors the radar's own
    // "lock cannot outlive its track" rule.
    rig.scan_cycle(1.0e6);
    const auto snap = page.update(rig.radar, kOwn, kOwnVel);
    EXPECT_FALSE(page.locked());
    EXPECT_EQ(page.locked_target_id(), 0u);
    EXPECT_EQ(rig.radar.mode(), sensors::RadarMode::Search);
    EXPECT_EQ(snap.mode, FcrState::Rws);
    EXPECT_FALSE(snap.locked);
}

// ============================================================================
// 3. The snapshot: page geometry, IFF, track quality, VS, determinism
// ============================================================================

TEST(FcrPage, SymbolsPinThePageGeometry) {
    FcrPageModel page;
    RadarRig rig;
    ASSERT_TRUE(page.power_on());

    // Closing contact 6 NM due north (same direction, slower): bearing
    // offset 0 from the north-centered antenna, closure +200.
    rig.detect(1, 0.0, 6.0 * kNm, 10000.0, 0.0, 300.0, 0.0, 1.0, "red");
    // Opening contact 6 NM due east (crossing): bearing offset +90 deg,
    // closure -400 (own north pull minus east flyer).
    rig.detect(2, 6.0 * kNm, 0.0, 10000.0, 500.0, 0.0, 0.0, 1.0, "blue");
    // Friendly due north: IFF friendly, same geometry as #1.
    rig.detect(3, 0.0, 4.0 * kNm, 10000.0, 0.0, 300.0, 0.0, 1.0, "blue");

    ASSERT_TRUE(page.designate(1, rig.radar));
    const auto snap = page.update(rig.radar, kOwn, kOwnVel);

    EXPECT_EQ(snap.mode, FcrState::Rws);
    EXPECT_FALSE(snap.velocity_only);
    EXPECT_EQ(snap.symbols.size(), 3u);   // ascending entity_id
    ASSERT_EQ(snap.symbols[0].entity_id, 1u);
    ASSERT_EQ(snap.symbols[1].entity_id, 2u);
    ASSERT_EQ(snap.symbols[2].entity_id, 3u);

    const auto& a = snap.symbols[0];
    const auto& b = snap.symbols[1];
    const auto& c = snap.symbols[2];

    // #1: dead ahead of the antenna, 6 NM, closing, hostile, designated.
    EXPECT_NEAR(a.azimuth_rad, 0.0, 1e-9);
    EXPECT_NEAR(a.elevation_rad, 0.0, 1e-9);   // same altitude
    EXPECT_NEAR(a.range_nm, 6.0, 1e-9);
    EXPECT_NEAR(a.closure_fps, 200.0, 1e-6);   // 500 - 300
    EXPECT_TRUE(a.hostile);
    EXPECT_TRUE(a.closing);
    EXPECT_TRUE(a.designated);

    // #2: +90 deg off the antenna, crossing east while own pulls north
    // => opening; friendly.
    EXPECT_NEAR(b.azimuth_rad, M_PI / 2.0, 1e-9);
    EXPECT_NEAR(b.closure_fps, -500.0, 1e-6);  // LOS east; own v is north
    EXPECT_FALSE(b.hostile);
    EXPECT_FALSE(b.closing);
    EXPECT_FALSE(b.designated);

    // #3: friendly, not designated.
    EXPECT_FALSE(c.hostile);
    EXPECT_FALSE(c.designated);
    EXPECT_NEAR(c.range_nm, 4.0, 1e-9);

    // The lock rides the radar: Track mode on #1.
    EXPECT_EQ(rig.radar.mode(), sensors::RadarMode::Track);
}

TEST(FcrPage, AzimuthWrapsTheShortestWay) {
    FcrPageModel page;
    RadarRig rig;
    ASSERT_TRUE(page.power_on());

    // Steer the antenna east (pi/2): a target 20 deg north of east reads
    // +20 deg (NOT -340), and one 10 deg past the reciprocal reads ~170
    // deg, not -190.
    rig.radar.scan.azimuth_center_rad = M_PI / 2.0;
    rig.detect(1, 8.0 * kNm, 2.0 * kNm, 10000.0, 0.0, 300.0, 0.0, 1.0,
               "red");
    rig.detect(2, -2.0 * kNm, 6.0 * kNm, 10000.0, 0.0, 300.0, 0.0, 1.0,
               "red");

    const auto snap = page.update(rig.radar, kOwn, kOwnVel);
    ASSERT_EQ(snap.symbols.size(), 2u);
    const double bearing1 = std::atan2(8.0, 2.0);    // ~76 deg
    const double bearing2 = std::atan2(-2.0, 6.0);   // ~-18 deg
    EXPECT_NEAR(snap.symbols[0].azimuth_rad,
                bearing1 - M_PI / 2.0, 1e-9);
    EXPECT_NEAR(snap.symbols[1].azimuth_rad,
                bearing2 - M_PI / 2.0, 1e-9);
    // #1 sits 14 deg SHORT of the east-boresighted antenna (negative =
    // counterclockwise on the page); #2 wraps the shortest way to
    // ~-108 deg (NOT +252).
    EXPECT_TRUE(snap.symbols[0].azimuth_rad < 0.0 &&
                snap.symbols[0].azimuth_rad > -M_PI / 2.0);
    EXPECT_TRUE(snap.symbols[1].azimuth_rad < -M_PI / 2.0 &&
                snap.symbols[1].azimuth_rad > -M_PI);
}

TEST(FcrPage, EstablishedFlagFollowsTheTrackQualityLadder) {
    FcrPageModel page;
    RadarRig rig;
    ASSERT_TRUE(page.power_on());
    rig.detect(5, 0.0, 6.0 * kNm, 10000.0, 0.0, 300.0, 0.0, 1.0, "red");

    // One detection: Tentative — hollow on the page.
    auto snap = page.update(rig.radar, kOwn, kOwnVel);
    ASSERT_EQ(snap.symbols.size(), 1u);
    EXPECT_FALSE(snap.symbols[0].established);

    // Two scan cycles with a fresh detection each: Established.
    rig.scan_cycle(2.0);
    rig.detect(5, 0.0, 6.0 * kNm, 10000.0, 0.0, 300.0, 0.0, 2.0, "red");
    snap = page.update(rig.radar, kOwn, kOwnVel);
    ASSERT_EQ(snap.symbols.size(), 1u);
    EXPECT_TRUE(snap.symbols[0].established);
}

TEST(FcrPage, VelocitySearchListsOnlyClosingContacts) {
    FcrPageModel page;
    RadarRig rig;
    ASSERT_TRUE(page.power_on());
    rig.detect(1, 0.0, 6.0 * kNm, 10000.0, 0.0, 300.0, 0.0, 1.0, "red");
    rig.detect(2, 6.0 * kNm, 0.0, 10000.0, 500.0, 0.0, 0.0, 1.0, "red");

    ASSERT_TRUE(page.select_vs());
    const auto snap = page.update(rig.radar, kOwn, kOwnVel);
    EXPECT_TRUE(snap.velocity_only);
    ASSERT_EQ(snap.symbols.size(), 1u)
        << "VS shows the closing contact only";
    EXPECT_EQ(snap.symbols[0].entity_id, 1u);
    EXPECT_TRUE(snap.symbols[0].closing);

    // Back to RWS: the full picture returns (no sensor physics changed).
    ASSERT_TRUE(page.select_rws());
    const auto rws = page.update(rig.radar, kOwn, kOwnVel);
    EXPECT_FALSE(rws.velocity_only);
    EXPECT_EQ(rws.symbols.size(), 2u);
}

TEST(FcrPage, SnapshotIsDeterministic) {
    auto build = []() {
        FcrPageModel page;
        RadarRig rig;
        page.power_on();
        page.select_tws();
        rig.detect(1, 0.0, 6.0 * kNm, 10000.0, 0.0, 300.0, 0.0, 1.0,
                   "red");
        rig.detect(2, 6.0 * kNm, 0.0, 10000.0, 500.0, 0.0, 0.0, 1.0,
                   "red");
        page.designate(1, rig.radar);
        return page.update(rig.radar, kOwn, kOwnVel);
    };
    const auto s1 = build();
    const auto s2 = build();
    EXPECT_EQ(s1, s2) << "two renderers must agree frame for frame";
}

TEST(FcrPage, SnapshotCarriesTheLiveScanFrame) {
    FcrPageModel page;
    RadarRig rig;
    rig.radar.scan.azimuth_half_width_rad = 0.7;
    rig.radar.scan.elevation_min_rad = -0.4;
    rig.radar.scan.elevation_max_rad = 0.4;
    rig.radar.scan.range_scale_nm = 80.0;
    ASSERT_TRUE(page.power_on());

    const auto snap = page.update(rig.radar, kOwn, kOwnVel);
    EXPECT_NEAR(snap.az_half_width_rad, 0.7, 1e-9);
    EXPECT_NEAR(snap.el_min_rad, -0.4, 1e-9);
    EXPECT_NEAR(snap.el_max_rad, 0.4, 1e-9);
    EXPECT_NEAR(snap.range_scale_nm, 80.0, 1e-9);
}
