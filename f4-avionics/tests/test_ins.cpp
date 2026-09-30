// f4-avionics/tests/test_ins.cpp
//
// The INS test suite (AVIONICS-1's done-when pins):
//   - alignment completes on the ground clock (and ONLY on it),
//   - drift accumulates deterministically (seed + update stream keyed,
//     bounded by the config's clamps, derived from the nav data),
//   - the drift-zero twin compares equal to raw truth member-for-member.

#include <f4/avionics/f4_avionics.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <string>

using namespace f4::avionics;
using f4::geo::WorldPosition;

namespace {

// ============================================================================
// The truth mock — a scripted IAircraftState the tests dial in per tick.
// ============================================================================

class TruthState final : public f4::flight::IAircraftState {
public:
    // Position (ENU feet).
    double east{0.0};
    double north{0.0};
    double alt_msl{10000.0};

    // Attitude / speed.
    double hdg_rad{0.0};      // keep in [0, 2*pi) so drift-zero equality is exact
    double gs{0.0};           // ground speed ft/s
    bool on_ground_{false};

    // --- IAircraftState ---
    [[nodiscard]] double position_east_ft() const override { return east; }
    [[nodiscard]] double position_north_ft() const override { return north; }
    [[nodiscard]] double altitude_msl_ft() const override { return alt_msl; }
    [[nodiscard]] double altitude_agl_ft() const override { return alt_msl - 100.0; }
    [[nodiscard]] double vcas_kts() const override { return 400.0; }
    [[nodiscard]] double ground_speed_fps() const override { return gs; }
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

/// A drifting-walk-friendly config: large clamps so a handful of ticks
/// moves the chain visibly, seed pinned for reproducibility.
InsConfig drift_cfg(std::uint64_t seed = 1234) {
    InsConfig cfg;
    cfg.align_time_s = 5.0;
    cfg.nav_data_key = "test-db-korea";
    cfg.max_pos_drift_rate_fps = 50.0;
    cfg.max_heading_drift_rate_radps = 1.0e-3;
    cfg.seed = seed;
    cfg.drift_enabled = true;
    return cfg;
}

/// Run `n` ticks of dt at a fixed truth; returns nothing (the unit keeps
/// the state under test).
void run_ticks(InsUnit& ins, const TruthState& truth, double dt, int n) {
    for (int i = 0; i < n; ++i) {
        ins.update(dt, truth);
    }
}

// ============================================================================
// Lifecycle: Off / Aligning / Aligned
// ============================================================================

TEST(Ins, StartsOffAndReportsRawTruth) {
    TruthState t;
    t.east = 1234.5;
    t.north = -987.25;
    t.alt_msl = 20000.0;
    t.hdg_rad = 1.0;

    InsUnit ins{InsConfig{}};
    ASSERT_EQ(ins.state(), InsState::Off);
    ASSERT_DOUBLE_EQ(ins.align_progress(), 0.0);

    ins.update(0.5, t);
    const auto sol = ins.solution();
    EXPECT_EQ(sol.position.x, t.east);
    EXPECT_EQ(sol.position.y, t.north);
    EXPECT_EQ(sol.position.z, t.alt_msl);
    EXPECT_EQ(sol.heading_rad, t.hdg_rad);
    EXPECT_EQ(sol.altitude_msl_ft, t.alt_msl);
}

TEST(Ins, PowerOnMovesOffToAligning) {
    TruthState t;
    t.on_ground_ = true;

    InsUnit ins{InsConfig{}};
    ins.power_on();
    EXPECT_EQ(ins.state(), InsState::Aligning);

    // Re-power mid-align is a no-op (the clock does not restart).
    ins.update(1.0, t);
    ins.power_on();
    EXPECT_EQ(ins.state(), InsState::Aligning);
    // 1 second of the align clock survived the no-op power_on.
    EXPECT_NEAR(ins.align_progress(), 1.0 / ins.config().align_time_s, 1e-12);
}

TEST(Ins, AlignmentCompletesOnTheGroundClock) {
    TruthState t;
    t.on_ground_ = true;

    InsConfig cfg;
    cfg.align_time_s = 60.0;
    InsUnit ins{cfg};
    ins.power_on();

    for (int i = 0; i < 59; ++i) {
        ins.update(1.0, t);
        EXPECT_EQ(ins.state(), InsState::Aligning) << "tick " << i;
    }
    EXPECT_DOUBLE_EQ(ins.align_progress(), 59.0 / 60.0);

    ins.update(1.0, t);   // the 60th ground second fires AlignTimer
    EXPECT_EQ(ins.state(), InsState::Aligned);
    EXPECT_DOUBLE_EQ(ins.align_progress(), 1.0);
}

TEST(Ins, AlignmentClockRunsOnlyWhileOnGround) {
    TruthState t;
    InsConfig cfg;
    cfg.align_time_s = 60.0;
    InsUnit ins{cfg};
    ins.power_on();

    t.on_ground_ = true;
    for (int i = 0; i < 30; ++i) ins.update(1.0, t);

    // Airborne: the clock freezes mid-align.
    t.on_ground_ = false;
    for (int i = 0; i < 100; ++i) ins.update(1.0, t);
    EXPECT_EQ(ins.state(), InsState::Aligning);
    EXPECT_DOUBLE_EQ(ins.align_progress(), 0.5);

    // Back on the deck: the remaining 30 seconds complete the align.
    t.on_ground_ = true;
    for (int i = 0; i < 29; ++i) {
        ins.update(1.0, t);
        EXPECT_EQ(ins.state(), InsState::Aligning);
    }
    ins.update(1.0, t);
    EXPECT_EQ(ins.state(), InsState::Aligned);
}

TEST(Ins, AlignTimeZeroCompletesOnTheFirstGroundTick) {
    TruthState t;
    t.on_ground_ = true;

    InsConfig cfg;
    cfg.align_time_s = 0.0;
    InsUnit ins{cfg};
    ins.power_on();
    ins.update(1.0, t);
    EXPECT_EQ(ins.state(), InsState::Aligned);
}

TEST(Ins, ShutdownDiscardsTheChainFromAnyLiveState) {
    TruthState t;
    t.on_ground_ = true;

    // From Aligned.
    InsUnit ins{drift_cfg()};
    ins.power_on();
    run_ticks(ins, t, 1.0, 10);   // 5 s align + 5 s aligned drift
    ASSERT_EQ(ins.state(), InsState::Aligned);
    ins.shutdown();
    EXPECT_EQ(ins.state(), InsState::Off);
    EXPECT_EQ(ins.position_drift(), WorldPosition{});
    EXPECT_DOUBLE_EQ(ins.heading_drift_rad(), 0.0);
    EXPECT_DOUBLE_EQ(ins.align_progress(), 0.0);

    // From Aligning.
    InsUnit ins2{InsConfig{}};
    ins2.power_on();
    ins2.update(1.0, t);
    ASSERT_EQ(ins2.state(), InsState::Aligning);
    ins2.shutdown();
    EXPECT_EQ(ins2.state(), InsState::Off);
}

TEST(Ins, RealignmentResetsTheDrift) {
    TruthState t;
    t.on_ground_ = true;

    InsUnit ins{drift_cfg()};
    ins.power_on();
    run_ticks(ins, t, 1.0, 105);   // align + 100 drifting seconds
    ASSERT_EQ(ins.state(), InsState::Aligned);
    ASSERT_NE(ins.position_drift(), WorldPosition{}) << "walk must move before the reset pin";

    ins.shutdown();
    ins.power_on();
    ASSERT_EQ(ins.state(), InsState::Aligning);
    run_ticks(ins, t, 1.0, 5);     // the same 5-second ground align
    ASSERT_EQ(ins.state(), InsState::Aligned);

    // The chain restarted from truth at the NEW alignment completion.
    EXPECT_EQ(ins.position_drift(), WorldPosition{});
    EXPECT_DOUBLE_EQ(ins.heading_drift_rad(), 0.0);
    EXPECT_EQ(ins.solution().position.x, t.east);
}

// ============================================================================
// The transition table is data (f4-state-machine discipline)
// ============================================================================

TEST(Ins, TransitionTableIsNamedAndPinned) {
    const auto m = make_ins_machine();
    EXPECT_EQ(m.current(), InsState::Off);
    ASSERT_EQ(m.transitions().size(), std::size_t{4});

    EXPECT_STREQ(std::string(m.name_of(InsState::Off)).c_str(), "Off");
    EXPECT_STREQ(std::string(m.name_of(InsState::Aligning)).c_str(), "Aligning");
    EXPECT_STREQ(std::string(m.name_of(InsState::Aligned)).c_str(), "Aligned");
    EXPECT_STREQ(std::string(m.name_of(InsEvent::AlignTimer)).c_str(), "AlignTimer");

    // Every row fires from its source state (no dead rows). force_to_state
    // is the documented test-fixture path — the machine's table, walked.
    auto walk = make_ins_machine();
    walk.force_to_state(InsState::Off);
    EXPECT_TRUE(walk.can_fire(InsEvent::PowerOn));
    EXPECT_FALSE(walk.can_fire(InsEvent::AlignTimer));
    EXPECT_FALSE(walk.can_fire(InsEvent::Shutdown));

    walk.force_to_state(InsState::Aligning);
    EXPECT_TRUE(walk.can_fire(InsEvent::AlignTimer));
    EXPECT_TRUE(walk.can_fire(InsEvent::Shutdown));
    EXPECT_FALSE(walk.can_fire(InsEvent::PowerOn));

    walk.force_to_state(InsState::Aligned);
    EXPECT_TRUE(walk.can_fire(InsEvent::Shutdown));
    EXPECT_FALSE(walk.can_fire(InsEvent::AlignTimer));
    EXPECT_FALSE(walk.can_fire(InsEvent::PowerOn));
}

// ============================================================================
// Drift: deterministic, keyed on the nav data, bounded
// ============================================================================

TEST(Ins, DriftWalkIsDeterministicForASeedAndUpdateStream) {
    TruthState t;
    t.on_ground_ = true;

    InsUnit a{drift_cfg()};
    InsUnit b{drift_cfg()};
    a.power_on();
    b.power_on();
    run_ticks(a, t, 1.0, 100);
    run_ticks(b, t, 1.0, 100);

    EXPECT_EQ(a.solution(), b.solution()) << "same seed + same stream => identical chain";
    EXPECT_EQ(a.position_drift(), b.position_drift());
    EXPECT_DOUBLE_EQ(a.heading_drift_rad(), b.heading_drift_rad());
    // And the walk actually moved (the determinism pin is not vacuous).
    EXPECT_NE(a.position_drift(), WorldPosition{});
}

TEST(Ins, DriftWalkFollowsItsUpdateStream) {
    TruthState t;
    t.on_ground_ = true;

    // Same seed, same simulated TIME, different tick stream -> different
    // walk (deterministic given the stream, not given the wall clock).
    InsUnit a{drift_cfg(77)};
    InsUnit b{drift_cfg(77)};
    a.power_on();
    b.power_on();
    for (int i = 0; i < 95; ++i) {   // 5 s align, then 95 x 1 s
        a.update(1.0, t);
        if (i >= 5) b.update(0.5, t);
    }
    for (int i = 0; i < 190; ++i) b.update(0.5, t);   // 95 s more at half-dt

    EXPECT_EQ(a.state(), InsState::Aligned);
    EXPECT_EQ(b.state(), InsState::Aligned);
    EXPECT_NE(a.position_drift(), b.position_drift());
}

TEST(Ins, DifferentSeedsWalkDifferently) {
    TruthState t;
    t.on_ground_ = true;

    InsUnit a{drift_cfg(1)};
    InsUnit b{drift_cfg(2)};
    a.power_on();
    b.power_on();
    run_ticks(a, t, 1.0, 100);
    run_ticks(b, t, 1.0, 100);

    EXPECT_NE(a.position_drift(), b.position_drift());
    EXPECT_NE(a.solution(), b.solution());
}

TEST(Ins, DriftIsKeyedOnTheAirframesNavData) {
    TruthState t;
    t.on_ground_ = true;

    auto keyed = [](const std::string& key, double age_days) {
        InsConfig cfg;
        cfg.align_time_s = 5.0;
        cfg.nav_data_key = key;
        cfg.nav_data_age_days = age_days;
        cfg.max_pos_drift_rate_fps = 50.0;
        cfg.max_heading_drift_rate_radps = 1.0e-3;
        return InsUnit{cfg};
    };

    // Same key at different ages -> different walks.
    InsUnit fresh = keyed("F-16C blk40 korea", 0.0);
    InsUnit stale = keyed("F-16C blk40 korea", 3650.0);
    // Different keys at the same age -> different walks.
    InsUnit other = keyed("F-16C blk40 baltic", 0.0);

    for (InsUnit* u : {&fresh, &stale, &other}) u->power_on();
    run_ticks(fresh, t, 1.0, 100);
    run_ticks(stale, t, 1.0, 100);
    run_ticks(other, t, 1.0, 100);

    EXPECT_NE(fresh.position_drift(), stale.position_drift());
    EXPECT_NE(fresh.position_drift(), other.position_drift());
    EXPECT_NE(stale.position_drift(), other.position_drift());
}

TEST(Ins, DriftStaysInsideTheConfiguredClamps) {
    TruthState t;
    t.on_ground_ = true;

    InsConfig cfg;
    cfg.align_time_s = 1.0;
    cfg.max_pos_drift_rate_fps = 1.0;
    cfg.max_heading_drift_rate_radps = 1.0e-6;
    cfg.seed = 42;

    InsUnit ins{cfg};
    ins.power_on();
    ins.update(1.0, t);   // align completes

    const int ticks = 5000;
    const double dt = 0.1;
    const double total = static_cast<double>(ticks) * dt;
    for (int i = 0; i < ticks; ++i) ins.update(dt, t);

    // Per-axis position drift is bounded by the clamp rate times the
    // aligned time (plus float slack).
    const auto drift = ins.position_drift();
    EXPECT_LE(std::abs(drift.x), 1.0 * total + 1e-6);
    EXPECT_LE(std::abs(drift.y), 1.0 * total + 1e-6);
    EXPECT_LE(std::abs(drift.z), 1.0 * total + 1e-6);
    EXPECT_LE(std::abs(ins.heading_drift_rad()), 1.0e-6 * total + 1e-15);

    // The believed chain is exactly truth-plus-drift (the model's contract).
    const auto sol = ins.solution();
    EXPECT_EQ(sol.position.x, t.east + drift.x);
    EXPECT_EQ(sol.position.y, t.north + drift.y);
    EXPECT_EQ(sol.position.z, t.alt_msl + drift.z);
}

TEST(Ins, DriftZeroTwinIsByteIdenticalToRawTruth) {
    TruthState t;
    t.on_ground_ = true;

    InsConfig cfg = drift_cfg(999);
    cfg.drift_enabled = false;
    InsUnit ins{cfg};
    ins.power_on();

    for (int i = 0; i < 200; ++i) {
        // Fly the truth around so the pin holds across a moving stream,
        // then take off halfway through.
        t.east += 600.0 * 0.1;
        t.north += 200.0 * 0.1;
        t.alt_msl = t.on_ground_ ? 100.0 : t.alt_msl + 100.0 * 0.1;
        if (i == 100) {
            t.on_ground_ = false;
            t.alt_msl = 20000.0;
        }
        ins.update(0.1, t);

        const auto sol = ins.solution();
        EXPECT_EQ(sol.position.x, t.east);
        EXPECT_EQ(sol.position.y, t.north);
        EXPECT_EQ(sol.position.z, t.alt_msl);
        EXPECT_EQ(sol.heading_rad, t.hdg_rad);
        EXPECT_EQ(sol.altitude_msl_ft, t.alt_msl);
        EXPECT_EQ(ins.position_drift(), WorldPosition{});
        EXPECT_DOUBLE_EQ(ins.heading_drift_rad(), 0.0);
    }
}

TEST(Ins, BelievedHeadingWrapsIntoTwoPi) {
    TruthState t;
    t.on_ground_ = true;

    InsConfig cfg = drift_cfg(5);
    cfg.max_heading_drift_rate_radps = 1.0;   // huge, so the wrap is exercised
    InsUnit ins{cfg};
    ins.power_on();
    run_ticks(ins, t, 1.0, 200);

    const double wrapped = ins.solution().heading_rad;
    EXPECT_GE(wrapped, 0.0);
    EXPECT_LT(wrapped, 2.0 * f4::geo::PI);
    // The drift itself stays unwrapped (the integral, not the report).
    EXPECT_DOUBLE_EQ(ins.solution().heading_rad,
                     detail::wrap_two_pi(t.hdg_rad + ins.heading_drift_rad()));
}

}  // namespace
