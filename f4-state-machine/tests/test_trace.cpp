// test_trace.cpp — transition trace recording and text emission.
//
// Verifies the observability layer that directly addresses the F4Flight digi
// "impossible to track down" problem: every transition is recorded with
// enough context to diagnose it from text alone.

#include <f4/fsm/f4_fsm.hpp>

#include <gtest/gtest.h>

#include <string>

using namespace f4::fsm;

namespace {

enum class S { A, B, C };
enum class E { Go, Back, Stop };

using SM = StateMachine<S, E>;

SM make_sm() {
    return SM::Builder()
        .initial(S::A)
        .state(S::A, "A").state(S::B, "B").state(S::C, "C")
        .event_name(E::Go, "Go")
        .event_name(E::Back, "Back")
        .event_name(E::Stop, "Stop")
        .on(S::A, S::B, E::Go)
        .on(S::B, S::C, E::Go, nullptr, []{ return false; }, "guarded-go-from-B")
        .on(S::B, S::A, E::Back)
        .on(S::C, S::A, E::Back)
        .build();
}

}  // namespace

TEST(TraceTest, RecordsFiredTransitions) {
    auto sm = make_sm();
    Trace<S, E> trace;
    sm.set_trace(&trace);
    sm.process(E::Go);   // A -> B
    sm.process(E::Back); // B -> A
    EXPECT_EQ(trace.size(), 2u);
    EXPECT_EQ(trace.records()[0].from, S::A);
    EXPECT_EQ(trace.records()[0].to,   S::B);
    EXPECT_EQ(trace.records()[1].from, S::B);
    EXPECT_EQ(trace.records()[1].to,   S::A);
}

TEST(TraceTest, DoesNotRecordRejectionsByDefault) {
    auto sm = make_sm();
    Trace<S, E> trace;
    sm.set_trace(&trace);
    sm.process(E::Go);  // A -> B
    sm.process(E::Go);  // B -> C is guarded (returns false); no transition
    EXPECT_EQ(sm.current(), S::B);  // stayed
    // Only the fired A->B is recorded; the rejected B->C is not (default).
    EXPECT_EQ(trace.size(), 1u);
}

TEST(TraceTest, RecordsRejectionsWhenEnabled) {
    auto sm = make_sm();
    Trace<S, E> trace;
    trace.set_trace_rejections(true);
    sm.set_trace(&trace);
    sm.process(E::Go);  // A -> B
    sm.process(E::Go);  // B -> C rejected by guard
    EXPECT_EQ(trace.size(), 2u);
    const auto& recs = trace.records();
    EXPECT_TRUE(recs[0].fired);
    EXPECT_FALSE(recs[1].fired);
    EXPECT_EQ(recs[1].reason, "guarded-go-from-B");
    EXPECT_FALSE(recs[1].guard_passed);
}

TEST(TraceTest, RingBufferEvictsOldest) {
    auto sm = make_sm();
    Trace<S, E> trace;
    trace.set_capacity(3);
    sm.set_trace(&trace);
    // Bounce A<->B many times; only last 3 should be retained.
    for (int i = 0; i < 10; ++i) {
        sm.process(E::Go);
        sm.process(E::Back);
    }
    EXPECT_EQ(trace.size(), 3u);
    // Most recent record should be A -> A? No: last was Back (B->A).
    EXPECT_EQ(trace.records().back().to, S::A);
}

TEST(TraceTest, ToTextEmitsParseableLines) {
    auto sm = make_sm();
    Trace<S, E> trace;
    trace.set_trace_rejections(true);  // record the rejection so it appears in text
    sm.set_trace(&trace);
    sm.process(E::Go);   // A->B
    sm.process(E::Go);   // B->C rejected by guard
    std::string text = trace.to_text(
        [&sm](S s){ return sm.name_of(s); },
        [&sm](const E& e){ return sm.name_of(e); });
    // Two lines, each starting with "tick=".
    EXPECT_NE(text.find("tick=1 from=A to=B event=Go fired=1"), std::string::npos);
    EXPECT_NE(text.find("tick=2 from=B to=B event=Go fired=0 guard=FAIL"), std::string::npos);
    EXPECT_NE(text.find("reason=\"guarded-go-from-B\""), std::string::npos);
    // Count newlines == 2 (one fired + one rejected; no spurious no-match
    // record because a (from,event) pair WAS matched, just guard-rejected).
    EXPECT_EQ(std::count(text.begin(), text.end(), '\n'), 2);
}

TEST(TraceTest, SummaryAggregatesTransitionCounts) {
    auto sm = make_sm();
    Trace<S, E> trace;
    sm.set_trace(&trace);
    // A->B, B->A, A->B, B->A
    sm.process(E::Go); sm.process(E::Back);
    sm.process(E::Go); sm.process(E::Back);
    std::string s = trace.summary([&sm](S st){ return sm.name_of(st); });
    EXPECT_NE(s.find("from=A to=B count=2"), std::string::npos);
    EXPECT_NE(s.find("from=B to=A count=2"), std::string::npos);
}

TEST(TraceTest, ObserverReceivesRecordsLive) {
    auto sm = make_sm();
    int calls = 0;
    S last_to = S::A;
    sm.set_observer([&](const TransitionRecord<S, E>& r){ ++calls; last_to = r.to; });
    sm.process(E::Go);
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(last_to, S::B);
}

TEST(TraceTest, UnboundedCapacity) {
    auto sm = make_sm();
    Trace<S, E> trace;
    trace.set_capacity(0);  // unbounded
    sm.set_trace(&trace);
    for (int i = 0; i < 1000; ++i) { sm.process(E::Go); sm.process(E::Back); }
    EXPECT_EQ(trace.size(), 2000u);  // nothing evicted
}
