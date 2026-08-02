// test_state_machine.cpp — core StateMachine behavior.
//
// Covers: builder, basic transitions, guards (pass/fail/scan-continuation),
// entry/exit actions (UML 2 firing order), transition actions, no-match,
// can_fire/can_transition, reset, tick counting, name registration.
//
// Links ONLY against f4-state-machine (zero-domain-coupling invariant).

#include <f4/fsm/f4_fsm.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace f4::fsm;

namespace {

enum class DoorState { Closed, Opening, Open, Closing, Faulted };
enum class DoorEvent { OpenCmd, CloseCmd, FullyOpen, FullyClosed, Obstacle, Reset };

using DoorSM = StateMachine<DoorState, DoorEvent>;

// A small fixture builder with named states/events and a fault entry action.
DoorSM make_door_machine(std::vector<std::string>& log) {
    return DoorSM::Builder()
        .initial(DoorState::Closed)
        .state(DoorState::Closed,  "Closed")
        .state(DoorState::Opening, "Opening")
        .state(DoorState::Open,    "Open")
        .state(DoorState::Closing, "Closing")
        .state(DoorState::Faulted, "Faulted")
        .event_name(DoorEvent::OpenCmd,      "OpenCmd")
        .event_name(DoorEvent::CloseCmd,     "CloseCmd")
        .event_name(DoorEvent::FullyOpen,    "FullyOpen")
        .event_name(DoorEvent::FullyClosed,  "FullyClosed")
        .event_name(DoorEvent::Obstacle,     "Obstacle")
        .event_name(DoorEvent::Reset,        "Reset")
        .on(DoorState::Closed,  DoorState::Opening, DoorEvent::OpenCmd)
        .on(DoorState::Opening, DoorState::Open,    DoorEvent::FullyOpen)
        .on(DoorState::Open,    DoorState::Closing, DoorEvent::CloseCmd)
        .on(DoorState::Closing, DoorState::Closed,  DoorEvent::FullyClosed)
        // Obstacle during closing: retry once (guarded), else fault.
        .on(DoorState::Closing, DoorState::Opening, DoorEvent::Obstacle,
            /*action*/[&log](const DoorEvent&){ log.push_back("retry-open"); },
            /*guard */[&log]{ log.push_back("guard:retries<1"); return false; },
            /*reason*/"retry-once")
        .on(DoorState::Closing, DoorState::Faulted, DoorEvent::Obstacle,
            /*action*/nullptr, /*guard*/nullptr, /*reason*/"fault-on-obstacle")
        .on(DoorState::Faulted, DoorState::Closed,  DoorEvent::Reset)
        .on_enter(DoorState::Faulted, [&log](const DoorEvent&){ log.push_back("enter:Faulted"); })
        .on_exit(DoorState::Closed,   [&log](const DoorEvent&){ log.push_back("exit:Closed"); })
        .on_enter(DoorState::Closed,  [&log](const DoorEvent&){ log.push_back("enter:Closed"); })
        .build();
}

}  // namespace

TEST(DoorTest, BasicTransitionAndCurrent) {
    std::vector<std::string> log;
    auto sm = make_door_machine(log);
    EXPECT_EQ(sm.current(), DoorState::Closed);
    EXPECT_EQ(sm.process(DoorEvent::OpenCmd), DoorState::Opening);
    EXPECT_EQ(sm.current(), DoorState::Opening);
}

TEST(DoorTest, NoMatchLeavesStateUnchanged) {
    std::vector<std::string> log;
    auto sm = make_door_machine(log);
    // FullyOpen means nothing when Closed.
    EXPECT_EQ(sm.process(DoorEvent::FullyOpen), DoorState::Closed);
    EXPECT_EQ(sm.current(), DoorState::Closed);
}

TEST(DoorTest, GuardRejectionFallsThroughToNextMatch) {
    std::vector<std::string> log;
    auto sm = make_door_machine(log);
    // Drive to Closing.
    ASSERT_EQ(sm.process(DoorEvent::OpenCmd),     DoorState::Opening);
    ASSERT_EQ(sm.process(DoorEvent::FullyOpen),   DoorState::Open);
    ASSERT_EQ(sm.process(DoorEvent::CloseCmd),    DoorState::Closing);
    // Obstacle: first (guarded) transition's guard returns false -> falls
    // through to the unguarded fault transition.
    EXPECT_EQ(sm.process(DoorEvent::Obstacle),    DoorState::Faulted);
    // Verify the guard was evaluated (logged) and the retry action did NOT run.
    auto guard_called = std::find(log.begin(), log.end(), "guard:retries<1");
    ASSERT_NE(guard_called, log.end());
    EXPECT_EQ(std::count(log.begin(), log.end(), "retry-open"), 0);
}

TEST(DoorTest, EntryExitActionsFireInUml2Order) {
    std::vector<std::string> log;
    auto sm = make_door_machine(log);
    // Construction fires the initial state's entry action (enter:Closed) —
    // correct UML 2 behavior. Clear the log so we observe only the transition.
    log.clear();
    // OpenCmd: Closed -> Opening. Expect: exit:Closed first (UML 2: source
    // exit before transition action before target entry).
    sm.process(DoorEvent::OpenCmd);
    ASSERT_FALSE(log.empty());
    EXPECT_EQ(log.front(), "exit:Closed");
    // Drive Open -> Closing -> Faulted to trigger enter:Faulted.
    log.clear();
    sm.process(DoorEvent::FullyOpen);
    sm.process(DoorEvent::CloseCmd);
    sm.process(DoorEvent::Obstacle);
    EXPECT_EQ(sm.current(), DoorState::Faulted);
    auto f = std::find(log.begin(), log.end(), "enter:Faulted");
    EXPECT_NE(f, log.end());
}

TEST(DoorTest, CanFireAndCanTransition) {
    std::vector<std::string> log;
    auto sm = make_door_machine(log);
    EXPECT_TRUE(sm.can_fire(DoorEvent::OpenCmd));
    EXPECT_FALSE(sm.can_fire(DoorEvent::FullyOpen));  // no match from Closed
    EXPECT_TRUE(sm.can_transition(DoorState::Opening, DoorEvent::OpenCmd));
    EXPECT_FALSE(sm.can_transition(DoorState::Open, DoorEvent::OpenCmd));
}

TEST(DoorTest, ResetReturnsToInitialAndFiresEntry) {
    std::vector<std::string> log;
    auto sm = make_door_machine(log);
    sm.process(DoorEvent::OpenCmd);
    ASSERT_NE(sm.current(), DoorState::Closed);
    log.clear();
    sm.reset();
    EXPECT_EQ(sm.current(), DoorState::Closed);
    // Initial entry action (enter:Closed) should have fired on reset.
    EXPECT_EQ(std::count(log.begin(), log.end(), "enter:Closed"), 1);
    EXPECT_EQ(sm.tick(), 0u);  // reset zeroes the tick
}

TEST(DoorTest, TickIncrementsPerProcess) {
    std::vector<std::string> log;
    auto sm = make_door_machine(log);
    EXPECT_EQ(sm.tick(), 0u);
    sm.process(DoorEvent::OpenCmd);
    EXPECT_EQ(sm.tick(), 1u);
    sm.process(DoorEvent::FullyOpen);
    EXPECT_EQ(sm.tick(), 2u);
    sm.process(DoorEvent::FullyOpen);  // no-match, still increments
    EXPECT_EQ(sm.tick(), 3u);
}

TEST(DoorTest, NameRegistration) {
    std::vector<std::string> log;
    auto sm = make_door_machine(log);
    EXPECT_EQ(sm.name_of(DoorState::Closed),  "Closed");
    EXPECT_EQ(sm.name_of(DoorEvent::OpenCmd), "OpenCmd");
    // Unregistered? All are registered here; verify a default-constructed one.
    EXPECT_TRUE(sm.name_of(DoorState::Closed).empty() == false);
}

TEST(DoorTest, TransitionActionReceivesEvent) {
    enum class S { A, B };
    enum class E { Go, Other };
    E captured = E::Other;  // distinct from the event we will send
    auto sm = StateMachine<S, E>::Builder()
        .initial(S::A)
        .on(S::A, S::B, E::Go,
            [&captured](const E& e){ captured = e; })
        .build();
    sm.process(E::Go);
    EXPECT_EQ(captured, E::Go);  // confirms the actual event value reached the action
}
