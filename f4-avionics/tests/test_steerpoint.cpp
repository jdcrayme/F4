// f4-avionics/tests/test_steerpoint.cpp
//
// Steerpoint navigation pins (AVIONICS-1): the plan container, the
// to_bra-backed solution, and the HSI steering cue — including the
// through-the-INS reads the plan's done-when names ("steerpoint
// ranges/bearings reading through the INS").

#include <f4/avionics/f4_avionics.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <stdexcept>
#include <string>

using namespace f4::avionics;
using f4::geo::WorldPosition;

namespace {

// ---------------------------------------------------------------------------
// Truth mock (same shape as test_ins.cpp's, slimmed for the nav reads).
// ---------------------------------------------------------------------------

class TruthState final : public f4::flight::IAircraftState {
public:
    double east{0.0};
    double north{0.0};
    double alt_msl{10000.0};
    double hdg_rad{0.0};
    bool on_ground_{false};

    [[nodiscard]] double position_east_ft() const override { return east; }
    [[nodiscard]] double position_north_ft() const override { return north; }
    [[nodiscard]] double altitude_msl_ft() const override { return alt_msl; }
    [[nodiscard]] double altitude_agl_ft() const override { return alt_msl - 100.0; }
    [[nodiscard]] double vcas_kts() const override { return 400.0; }
    [[nodiscard]] double heading_rad() const override { return hdg_rad; }
    [[nodiscard]] double pitch_angle_rad() const override { return 0.0; }
    [[nodiscard]] double roll_angle_rad() const override { return 0.0; }
    [[nodiscard]] double roll_rate_radps() const override { return 0.0; }
    [[nodiscard]] double pitch_rate_radps() const override { return 0.0; }
    [[nodiscard]] double yaw_rate_radps() const override { return 0.0; }
    [[nodiscard]] double vertical_speed_fpm() const override { return 0.0; }
    [[nodiscard]] bool on_ground() const override { return on_ground_; }
    [[nodiscard]] double fuel_lbs() const override { return 5000.0; }
};

/// A drifting INS: aligned after 1 s on the ground, then a hot walk.
InsUnit drifting_ins(std::uint64_t seed) {
    InsConfig cfg;
    cfg.align_time_s = 1.0;
    cfg.nav_data_key = "steer-test-db";
    cfg.max_pos_drift_rate_fps = 50.0;
    cfg.max_heading_drift_rate_radps = 1.0e-3;
    cfg.seed = seed;
    InsUnit ins{cfg};
    TruthState t;
    t.on_ground_ = true;
    ins.power_on();
    ins.update(1.0, t);   // align completes on the first ground tick
    return ins;           // C++17 guaranteed elision (moves are deleted)
}

SteerpointSequence three_point_plan() {
    SteerpointSequence seq;
    seq.push_back(Steerpoint{"IP", WorldPosition{0.0, 60761.15, 12000.0}});
    seq.push_back(Steerpoint{"TARGET", WorldPosition{60761.15, 121522.31, 8000.0}});
    seq.push_back(Steerpoint{"HOME", WorldPosition{0.0, 0.0, 0.0}});
    return seq;
}

// ---------------------------------------------------------------------------
// The plan container
// ---------------------------------------------------------------------------

TEST(Steerpoint, SequenceCurrentNextSelect) {
    SteerpointSequence seq = three_point_plan();
    ASSERT_EQ(seq.size(), std::size_t{3});

    EXPECT_STREQ(seq.current().name.c_str(), "IP");
    EXPECT_EQ(seq.index(), std::size_t{0});

    EXPECT_TRUE(seq.next());
    EXPECT_STREQ(seq.current().name.c_str(), "TARGET");
    EXPECT_EQ(seq.index(), std::size_t{1});

    seq.select(2);
    EXPECT_STREQ(seq.current().name.c_str(), "HOME");

    // The wall: the last point holds (overflight is the host's decision).
    EXPECT_FALSE(seq.next());
    EXPECT_EQ(seq.index(), std::size_t{2});
    EXPECT_STREQ(seq.current().name.c_str(), "HOME");
}

TEST(Steerpoint, EmptyAndOutOfRangeAreLoud) {
    SteerpointSequence empty;
    EXPECT_THROW((void)empty.current(), std::out_of_range);

    SteerpointSequence seq = three_point_plan();
    EXPECT_THROW(seq.select(3), std::out_of_range);
    EXPECT_THROW(seq.select(99), std::out_of_range);

    // The loud failures leave the selection untouched.
    EXPECT_EQ(seq.index(), std::size_t{0});
    EXPECT_STREQ(seq.current().name.c_str(), "IP");
}

// ---------------------------------------------------------------------------
// The solution: to_bra over the supplied (believed) position
// ---------------------------------------------------------------------------

TEST(Steerpoint, ToSteerIsBearingRangeAltitude) {
    // 10 nm due north, 2,000 ft below the believed position.
    const WorldPosition believed{0.0, 0.0, 10000.0};
    const Steerpoint steer{"STP", WorldPosition{0.0, 60761.1546, 8000.0}};

    const f4::geo::BRA br = to_steer(believed, steer);
    EXPECT_NEAR(br.bearing_rad, 0.0, 1e-12);
    EXPECT_NEAR(br.range_ft, std::sqrt(60761.1546 * 60761.1546 + 2000.0 * 2000.0), 1e-6);
    EXPECT_DOUBLE_EQ(br.altitude_ft, 8000.0);

    // The read is f4-geo's to_bra — exactly (the wrapper adds no geometry).
    EXPECT_EQ(br, f4::geo::to_bra(believed, steer.position));
}

TEST(Steerpoint, BearingSemanticsPinTheCompass) {
    const WorldPosition believed{0.0, 0.0, 0.0};
    const double nm = 60761.1546;

    EXPECT_NEAR(to_steer(believed, Steerpoint{"N", WorldPosition{0.0, nm, 0.0}}).bearing_rad,
                0.0, 1e-12);
    EXPECT_NEAR(to_steer(believed, Steerpoint{"E", WorldPosition{nm, 0.0, 0.0}}).bearing_rad,
                f4::geo::PI / 2.0, 1e-12);
    EXPECT_NEAR(to_steer(believed, Steerpoint{"S", WorldPosition{0.0, -nm, 0.0}}).bearing_rad,
                f4::geo::PI, 1e-12);
    EXPECT_NEAR(to_steer(believed, Steerpoint{"W", WorldPosition{-nm, 0.0, 0.0}}).bearing_rad,
                3.0 * f4::geo::PI / 2.0, 1e-12);   // wrapped into [0, 2*pi)
}

TEST(Steerpoint, CurrentSteerReadsThroughTheIns) {
    TruthState t;
    t.east = 5000.0;
    t.north = -3000.0;
    t.alt_msl = 20000.0;

    SteerpointSequence seq;
    seq.push_back(Steerpoint{"STP", WorldPosition{t.east, t.north + 60761.15, 20000.0}});

    InsUnit ins = drifting_ins(31);
    ins.update(1.0, t);

    const f4::geo::BRA through_ins = current_steer(ins, seq);
    const f4::geo::BRA raw = to_steer(WorldPosition{t.east, t.north, t.alt_msl}, seq.current());

    // Same shape, different geometry: the drift moved the believed position,
    // so the read through the INS is no longer the raw read.
    EXPECT_NE(through_ins, raw);
    // And it is exactly the raw read with the believed position substituted.
    const f4::geo::BRA expected = to_steer(ins.solution().position, seq.current());
    EXPECT_EQ(through_ins, expected);
}

TEST(Steerpoint, DriftZeroInsGivesTheRawRead) {
    TruthState t;
    t.east = 5000.0;
    t.north = -3000.0;
    t.alt_msl = 20000.0;
    t.hdg_rad = f4::geo::PI / 6.0;   // in [0, 2*pi) so the wrap is identity

    SteerpointSequence seq;
    seq.push_back(Steerpoint{"STP", WorldPosition{t.east, t.north + 60761.15, 20000.0}});

    InsConfig cfg;
    cfg.align_time_s = 1.0;
    cfg.drift_enabled = false;
    InsUnit ins{cfg};
    TruthState ground;
    ground.on_ground_ = true;
    ins.power_on();
    ins.update(1.0, ground);
    for (int i = 0; i < 50; ++i) ins.update(0.1, t);

    const f4::geo::BRA through_ins = current_steer(ins, seq);
    const f4::geo::BRA raw = to_steer(WorldPosition{t.east, t.north, t.alt_msl}, seq.current());
    EXPECT_EQ(through_ins, raw) << "drift-zero: the through-INS read is the raw read";
}

// ---------------------------------------------------------------------------
// The HSI steering cue
// ---------------------------------------------------------------------------

TEST(Steerpoint, CueTurnsTheShortestWay) {
    const double d = f4::geo::DEG_TO_RAD;
    // 10 degrees left of north steering 010: turn right.
    SteeringCue c = steer_cue(350.0 * d, 10.0 * d);
    EXPECT_NEAR(c.bearing_error_rad, 20.0 * d, 1e-12);
    EXPECT_TRUE(c.turn_right);

    // Mirror: steering 350 from 010 turns left.
    c = steer_cue(10.0 * d, 350.0 * d);
    EXPECT_NEAR(c.bearing_error_rad, -20.0 * d, 1e-12);
    EXPECT_FALSE(c.turn_right);

    // Dead astern pins right (the documented arbitrary pin).
    c = steer_cue(0.0, f4::geo::PI);
    EXPECT_DOUBLE_EQ(c.bearing_error_rad, f4::geo::PI);
    EXPECT_TRUE(c.turn_right);
}

TEST(Steerpoint, CueThroughTheDriftingInsDiffersFromRaw) {
    TruthState t;
    t.east = 0.0;
    t.north = 0.0;
    t.hdg_rad = 0.0;   // pointing north

    SteerpointSequence seq;
    seq.push_back(Steerpoint{"STP", WorldPosition{0.0, 60761.15, 10000.0}});   // due north of TRUTH

    InsUnit ins = drifting_ins(77);
    ins.update(1.0, t);
    for (int i = 0; i < 100; ++i) ins.update(1.0, t);

    const SteeringCue through_ins = current_steer_cue(ins, seq);
    const f4::geo::BRA raw = to_steer(WorldPosition{t.east, t.north, t.alt_msl}, seq.current());
    const SteeringCue raw_cue = steer_cue(t.hdg_rad, raw.bearing_rad);

    // The drift moved the believed position off the north line, so the cue
    // through the INS is no longer the raw cue — that IS the avionics read.
    EXPECT_NE(through_ins, raw_cue);
}

TEST(Steerpoint, DriftZeroCueMatchesTheRawCue) {
    TruthState t;
    t.east = 123.0;
    t.north = -456.0;
    t.hdg_rad = f4::geo::DEG_TO_RAD * 40.0;

    SteerpointSequence seq;
    seq.push_back(Steerpoint{"STP", WorldPosition{-5000.0, 30000.0, 5000.0}});

    InsConfig cfg;
    cfg.align_time_s = 1.0;
    cfg.drift_enabled = false;
    InsUnit ins{cfg};
    TruthState ground;
    ground.on_ground_ = true;
    ins.power_on();
    ins.update(1.0, ground);
    ins.update(0.1, t);

    const SteeringCue through_ins = current_steer_cue(ins, seq);
    const f4::geo::BRA raw = to_steer(WorldPosition{t.east, t.north, t.alt_msl}, seq.current());
    const SteeringCue raw_cue = steer_cue(t.hdg_rad, raw.bearing_rad);

    EXPECT_EQ(through_ins.bearing_error_rad, raw_cue.bearing_error_rad);
    EXPECT_EQ(through_ins.turn_right, raw_cue.turn_right);
}

}  // namespace
