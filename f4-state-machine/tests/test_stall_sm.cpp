// test_stall_sm.cpp — the 6-state stall state machine from eom.cpp, as
// specified in Docs/ARCHITECTURE PROPOSAL.md §7.3.
//
// This is the first real domain SM built on f4-state-machine. It validates
// that the framework can express the stall lifecycle that f4-flight-model
// currently implements inline — the migration target referenced in the
// roadmap (M2.1: "Stall SM unit tests").

#include <f4/fsm/f4_fsm.hpp>

#include <gtest/gtest.h>

using namespace f4::fsm;

namespace {

enum class StallState {
    None, EnteringDeepStall, DeepStall, Spinning, FlatSpin, Recovering
};
enum class StallEvent {
    AoAExceed, TimerExpired, RecoveryAttempt, Recovered,
    SpinDetected, AsymmetryDetected
};

using StallSM = StateMachine<StallState, StallEvent>;

StallSM make_stall_machine() {
    return StallSM::Builder()
        .initial(StallState::None)
        .state(StallState::None,              "None")
        .state(StallState::EnteringDeepStall, "EnteringDeepStall")
        .state(StallState::DeepStall,         "DeepStall")
        .state(StallState::Spinning,          "Spinning")
        .state(StallState::FlatSpin,          "FlatSpin")
        .state(StallState::Recovering,        "Recovering")
        .event_name(StallEvent::AoAExceed,           "AoAExceed")
        .event_name(StallEvent::TimerExpired,        "TimerExpired")
        .event_name(StallEvent::RecoveryAttempt,     "RecoveryAttempt")
        .event_name(StallEvent::Recovered,           "Recovered")
        .event_name(StallEvent::SpinDetected,        "SpinDetected")
        .event_name(StallEvent::AsymmetryDetected,   "AsymmetryDetected")
        .on(StallState::None,              StallState::EnteringDeepStall, StallEvent::AoAExceed)
        .on(StallState::EnteringDeepStall, StallState::DeepStall,         StallEvent::TimerExpired)
        .on(StallState::DeepStall,         StallState::Spinning,          StallEvent::SpinDetected)
        .on(StallState::DeepStall,         StallState::FlatSpin,          StallEvent::AsymmetryDetected)
        .on(StallState::Spinning,          StallState::Recovering,        StallEvent::RecoveryAttempt)
        .on(StallState::FlatSpin,          StallState::Recovering,        StallEvent::RecoveryAttempt)
        .on(StallState::Recovering,        StallState::None,              StallEvent::Recovered)
        .build();
}

}  // namespace

TEST(StallSMTest, InitialStateIsNone) {
    auto sm = make_stall_machine();
    EXPECT_EQ(sm.current(), StallState::None);
}

TEST(StallSMTest, FullSpinLifecycle) {
    auto sm = make_stall_machine();
    sm.process(StallEvent::AoAExceed);
    EXPECT_EQ(sm.current(), StallState::EnteringDeepStall);
    sm.process(StallEvent::TimerExpired);
    EXPECT_EQ(sm.current(), StallState::DeepStall);
    sm.process(StallEvent::SpinDetected);
    EXPECT_EQ(sm.current(), StallState::Spinning);
    sm.process(StallEvent::RecoveryAttempt);
    EXPECT_EQ(sm.current(), StallState::Recovering);
    sm.process(StallEvent::Recovered);
    EXPECT_EQ(sm.current(), StallState::None);
}

TEST(StallSMTest, FlatSpinBranch) {
    auto sm = make_stall_machine();
    sm.process(StallEvent::AoAExceed);
    sm.process(StallEvent::TimerExpired);
    EXPECT_EQ(sm.current(), StallState::DeepStall);
    sm.process(StallEvent::AsymmetryDetected);
    EXPECT_EQ(sm.current(), StallState::FlatSpin);
    sm.process(StallEvent::RecoveryAttempt);
    EXPECT_EQ(sm.current(), StallState::Recovering);
}

TEST(StallSMTest, RecoveryAttemptIgnoredWhenNotStalled) {
    auto sm = make_stall_machine();
    // From None, RecoveryAttempt has no matching transition.
    EXPECT_EQ(sm.process(StallEvent::RecoveryAttempt), StallState::None);
    EXPECT_EQ(sm.current(), StallState::None);
}

TEST(StallSMTest, DeepStallRecoveryRequiresTimerFirst) {
    // AoAExceed -> EnteringDeepStall; must wait for TimerExpired before
    // recovery is possible. RecoveryAttempt from EnteringDeepStall is a no-op.
    auto sm = make_stall_machine();
    sm.process(StallEvent::AoAExceed);
    EXPECT_EQ(sm.process(StallEvent::RecoveryAttempt), StallState::EnteringDeepStall);
}

TEST(StallSMTest, CanFireReflectsReachableTransitions) {
    auto sm = make_stall_machine();
    EXPECT_TRUE(sm.can_fire(StallEvent::AoAExceed));
    EXPECT_FALSE(sm.can_fire(StallEvent::SpinDetected));  // not from None
    sm.process(StallEvent::AoAExceed);
    sm.process(StallEvent::TimerExpired);
    EXPECT_TRUE(sm.can_fire(StallEvent::SpinDetected));
    EXPECT_TRUE(sm.can_fire(StallEvent::AsymmetryDetected));
}

TEST(StallSMTest, ResetClearsStall) {
    auto sm = make_stall_machine();
    sm.process(StallEvent::AoAExceed);
    sm.process(StallEvent::TimerExpired);
    sm.process(StallEvent::SpinDetected);
    ASSERT_EQ(sm.current(), StallState::Spinning);
    sm.reset();
    EXPECT_EQ(sm.current(), StallState::None);
}

TEST(StallSMTest, TraceRecordsFullLifecycle) {
    auto sm = make_stall_machine();
    Trace<StallState, StallEvent> trace;
    sm.set_trace(&trace);
    sm.process(StallEvent::AoAExceed);
    sm.process(StallEvent::TimerExpired);
    sm.process(StallEvent::SpinDetected);
    sm.process(StallEvent::RecoveryAttempt);
    sm.process(StallEvent::Recovered);
    // 5 fired transitions recorded.
    EXPECT_EQ(trace.size(), 5u);
    const auto& recs = trace.records();
    EXPECT_EQ(recs[0].from, StallState::None);
    EXPECT_EQ(recs[0].to,   StallState::EnteringDeepStall);
    EXPECT_EQ(recs[4].to,   StallState::None);
    // Each record carries its tick (1-based after process).
    EXPECT_EQ(recs[0].tick, 1u);
    EXPECT_EQ(recs[4].tick, 5u);
}
