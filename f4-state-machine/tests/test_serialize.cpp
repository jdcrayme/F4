// test_serialize.cpp — transition-table and trace serialization to text.
//
// Verifies the table dump is parseable and complete (the "what CAN this
// machine do?" contract surface) and the trace dump is consistent with the
// live trace. No JSON lib — plain text, line-oriented, grep-friendly.

#include <f4/fsm/f4_fsm.hpp>

#include <gtest/gtest.h>

#include <sstream>
#include <string>

using namespace f4::fsm;

namespace {

enum class S { Idle, Run, Done };
enum class E { Start, Finish, Reset };
using SM = StateMachine<S, E>;

SM make_sm() {
    return SM::Builder()
        .initial(S::Idle)
        .state(S::Idle, "Idle").state(S::Run, "Run").state(S::Done, "Done")
        .event_name(E::Start, "Start")
        .event_name(E::Finish, "Finish")
        .event_name(E::Reset, "Reset")
        .on(S::Idle, S::Run,  E::Start)
        .on(S::Run,  S::Done, E::Finish, nullptr, nullptr, "normal-completion")
        .on(S::Done, S::Idle, E::Reset)
        .build();
}

}  // namespace

TEST(SerializeTest, TableTextHasHeaderAndTransitions) {
    auto sm = make_sm();
    std::string t = to_text(sm);
    // Header line.
    EXPECT_NE(t.find("statemachine initial=Idle transitions=3"), std::string::npos);
    // Each transition line.
    EXPECT_NE(t.find("transition from=Idle to=Run event=Start guard=no action=no"),
              std::string::npos);
    EXPECT_NE(t.find("transition from=Run to=Done event=Finish guard=no action=no "
                      "reason=\"normal-completion\""), std::string::npos);
    EXPECT_NE(t.find("transition from=Done to=Idle event=Reset guard=no action=no"),
              std::string::npos);
    // Exactly 4 lines (1 header + 3 transitions).
    EXPECT_EQ(std::count(t.begin(), t.end(), '\n'), 4);
}

TEST(SerializeTest, TableTextUsesNumericFallbackForUnnamedStates) {
    // Build an SM with no registered names.
    auto sm = StateMachine<S, E>::Builder()
        .initial(S::Idle)
        .on(S::Idle, S::Run, E::Start)
        .build();
    std::string t = to_text(sm);
    EXPECT_NE(t.find("state(0)"), std::string::npos);  // S::Idle == 0
    EXPECT_NE(t.find("state(1)"), std::string::npos);  // S::Run  == 1
}

TEST(SerializeTest, TraceTextMatchesLiveTrace) {
    auto sm = make_sm();
    Trace<S, E> trace;
    sm.set_trace(&trace);
    sm.process(E::Start);
    sm.process(E::Finish);
    std::string t = to_text(sm, trace);
    EXPECT_NE(t.find("tick=1 from=Idle to=Run event=Start fired=1"), std::string::npos);
    EXPECT_NE(t.find("tick=2 from=Run to=Done event=Finish fired=1"), std::string::npos);
}

TEST(SerializeTest, SummaryTextAggregates) {
    auto sm = make_sm();
    Trace<S, E> trace;
    sm.set_trace(&trace);
    sm.process(E::Start);
    sm.process(E::Finish);
    sm.process(E::Reset);
    std::string s = summary_text(sm, trace);
    EXPECT_NE(s.find("from=Idle to=Run count=1"), std::string::npos);
    EXPECT_NE(s.find("from=Run to=Done count=1"), std::string::npos);
    EXPECT_NE(s.find("from=Done to=Idle count=1"), std::string::npos);
}

TEST(SerializeTest, TableTextIsStableForDiffing) {
    // Two identical machines produce identical text — enables build-to-build
    // table diffing to catch accidental behavioral changes.
    auto a = make_sm();
    auto b = make_sm();
    EXPECT_EQ(to_text(a), to_text(b));
}
