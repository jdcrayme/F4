// test_layered.cpp — LayeredStateMachine priority preemption.
//
// Models a simplified AI DigiMode ladder: GroundAvoid (highest) >
// MissileDefeat > Navigation (lowest, the default). Each layer is an
// independent StateMachine with an idle state. effective_mode() returns the
// highest-precedence non-idle layer.

#include <f4/fsm/f4_fsm.hpp>

#include <gtest/gtest.h>

using namespace f4::fsm;

namespace {

enum class Mode { Idle, NavWaypoint, GroundAvoidPullUp, MissileDefeatBreak };
enum class Signal { ThreatDetected, ThreatCleared, MissileInbound, MissileGone, NavTick };

using ModeSM = StateMachine<Mode, Signal>;

ModeSM make_ground_avoid_sm() {
    return ModeSM::Builder()
        .initial(Mode::Idle)
        .state(Mode::Idle,              "GA-Idle")
        .state(Mode::GroundAvoidPullUp, "GroundAvoidPullUp")
        .on(Mode::Idle, Mode::GroundAvoidPullUp, Signal::ThreatDetected)
        .on(Mode::GroundAvoidPullUp, Mode::Idle, Signal::ThreatCleared)
        .build();
}

ModeSM make_missile_defeat_sm() {
    return ModeSM::Builder()
        .initial(Mode::Idle)
        .state(Mode::Idle,              "MD-Idle")
        .state(Mode::MissileDefeatBreak,"MissileDefeatBreak")
        .on(Mode::Idle, Mode::MissileDefeatBreak, Signal::MissileInbound)
        .on(Mode::MissileDefeatBreak, Mode::Idle, Signal::MissileGone)
        .build();
}

ModeSM make_nav_sm() {
    return ModeSM::Builder()
        .initial(Mode::NavWaypoint)  // nav is never idle
        .state(Mode::NavWaypoint, "NavWaypoint")
        .build();
}

LayeredStateMachine<Mode, Signal> make_digi_ladder() {
    LayeredStateMachine<Mode, Signal> lsm;
    lsm.add_layer(0, Mode::Idle, make_ground_avoid_sm());   // highest precedence
    lsm.add_layer(1, Mode::Idle, make_missile_defeat_sm());
    lsm.add_layer(2, Mode::Idle, make_nav_sm());            // lowest / default
    return lsm;
}

}  // namespace

TEST(LayeredTest, DefaultModeIsNavWhenNoThreats) {
    auto lsm = make_digi_ladder();
    EXPECT_EQ(lsm.effective_mode(), Mode::NavWaypoint);
    EXPECT_EQ(lsm.active_layer_index(), 2u);  // nav layer (last) is the fallback
}

TEST(LayeredTest, GroundAvoidPreemptsNav) {
    auto lsm = make_digi_ladder();
    lsm.process(Signal::ThreatDetected);
    EXPECT_EQ(lsm.effective_mode(), Mode::GroundAvoidPullUp);
    EXPECT_EQ(lsm.active_layer_index(), 0u);  // ground-avoid layer active
}

TEST(LayeredTest, MissileDefeatPreemptsNavButNotGroundAvoid) {
    auto lsm = make_digi_ladder();
    lsm.process(Signal::MissileInbound);
    EXPECT_EQ(lsm.effective_mode(), Mode::MissileDefeatBreak);
    EXPECT_EQ(lsm.active_layer_index(), 1u);
    // Now a ground threat appears — ground-avoid (higher precedence) wins.
    lsm.process(Signal::ThreatDetected);
    EXPECT_EQ(lsm.effective_mode(), Mode::GroundAvoidPullUp);
    EXPECT_EQ(lsm.active_layer_index(), 0u);
}

TEST(LayeredTest, ClearingThreatReturnsToNextHighestActive) {
    auto lsm = make_digi_ladder();
    lsm.process(Signal::ThreatDetected);
    lsm.process(Signal::MissileInbound);  // both active; ground-avoid wins
    EXPECT_EQ(lsm.effective_mode(), Mode::GroundAvoidPullUp);
    // Clear ground threat -> missile-defeat becomes highest active.
    lsm.process(Signal::ThreatCleared);
    EXPECT_EQ(lsm.effective_mode(), Mode::MissileDefeatBreak);
    // Clear missile -> back to nav.
    lsm.process(Signal::MissileGone);
    EXPECT_EQ(lsm.effective_mode(), Mode::NavWaypoint);
}

TEST(LayeredTest, ResetReturnsAllLayersToInitial) {
    auto lsm = make_digi_ladder();
    lsm.process(Signal::ThreatDetected);
    lsm.process(Signal::MissileInbound);
    lsm.reset();
    EXPECT_EQ(lsm.effective_mode(), Mode::NavWaypoint);
    EXPECT_EQ(lsm.active_layer_index(), 2u);
}

TEST(LayeredTest, LayerIntrospection) {
    auto lsm = make_digi_ladder();
    EXPECT_EQ(lsm.layer_count(), 3u);
    EXPECT_EQ(lsm.layer_priority(0), 0);
    EXPECT_EQ(lsm.layer_priority(1), 1);
    EXPECT_EQ(lsm.layer_priority(2), 2);
    EXPECT_EQ(lsm.layer_idle(0), Mode::Idle);
    EXPECT_EQ(lsm.layer_state(2), Mode::NavWaypoint);  // nav's "initial" is non-idle
}
