// test_atc_sm.cpp — a subset of the 17-state ATC/landing state machine.
//
// Demonstrates two design capabilities the §7 proposal's plain-enum example
// doesn't exercise, but which the real landing SM (M5.7) needs:
//
//   1. PAYLOAD-CARRYING EVENTS via std::variant. The "ApproachCleared" event
//      carries the runway heading; the entry action for the Approach state
//      reads it to configure the localizer. This is why StateMachine actions
//      take const Event& rather than being nullary.
//
//   2. ENTRY ACTIONS that configure per-state behavior (gear/flaps) — the
//      thing the inline switch-statement landing code scatters everywhere.
//
// This is a slim subset (5 states) of the full 17-state machine; the full
// port is an f4-ai task (M5.7). Here we prove the framework can express it.

#include <f4/fsm/f4_fsm.hpp>

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <variant>

using namespace f4::fsm;

namespace {

// --- States: a slice of the landing pattern ---
enum class LandingState {
    HoldingPattern,    // awaiting clearance
    Approach,          // on the localizer, descending
    Flare,             // round-out before touchdown
    Rollout,           // on the runway, decelerating
    GoAround           // aborted approach, climbing out
};

// --- Events: a variant carrying payload where needed ---
// Each struct needs operator== so std::variant::operator== works (the
// framework compares events via t.event == e). C++20 defaulted operator==
// suffices for simple value structs.
struct ApproachCleared { double runwayHeadingDeg{0.0}; bool operator==(const ApproachCleared&) const = default; };
struct GearDown         { bool operator==(const GearDown&) const = default; };
struct FlareAltitude    { bool operator==(const FlareAltitude&) const = default; };
struct Touchdown        { bool operator==(const Touchdown&) const = default; };
struct DecelComplete    { bool operator==(const DecelComplete&) const = default; };
struct MissedApproach   { bool operator==(const MissedApproach&) const = default; };
struct GoAroundComplete { bool operator==(const GoAroundComplete&) const = default; };

using LandingEvent = std::variant<
    ApproachCleared, GearDown, FlareAltitude, Touchdown,
    DecelComplete, MissedApproach, GoAroundComplete>;

// Host-side configuration captured by entry actions.
struct LandingConfig {
    std::optional<double> clearedRunwayHeading;
    bool gearCommandedDown{false};
    bool flapsFull{false};
};

using LandingSM = StateMachine<LandingState, LandingEvent>;

LandingSM make_landing_machine(LandingConfig& cfg) {
    return LandingSM::Builder()
        .initial(LandingState::HoldingPattern)
        .state(LandingState::HoldingPattern, "HoldingPattern")
        .state(LandingState::Approach,       "Approach")
        .state(LandingState::Flare,          "Flare")
        .state(LandingState::Rollout,        "Rollout")
        .state(LandingState::GoAround,       "GoAround")
        // On entering Approach, capture the cleared runway heading from the
        // event payload and command gear down.
        .on_enter(LandingState::Approach, [&cfg](const LandingEvent& e) {
            if (auto* ac = std::get_if<ApproachCleared>(&e)) {
                cfg.clearedRunwayHeading = ac->runwayHeadingDeg;
            }
            cfg.gearCommandedDown = true;
        })
        .on_enter(LandingState::Flare, [&cfg](const LandingEvent&) {
            cfg.flapsFull = true;
        })
        .on_enter(LandingState::Rollout, [&cfg](const LandingEvent&) {
            cfg.flapsFull = false;  // retract on rollout
        })
        .on_enter(LandingState::GoAround, [&cfg](const LandingEvent&) {
            cfg.gearCommandedDown = false;  // cleanup
        })
        // Transitions: matched by EVENT TYPE (on_if + holds_alternative), not
        // by payload value. This is the key design point for payload-carrying
        // variant events — an ApproachCleared event matches regardless of
        // which runway heading it carries; the heading is read by the entry
        // action, not used for matching.
        .on_if(LandingState::HoldingPattern, LandingState::Approach,
            [](const LandingEvent& e){ return std::holds_alternative<ApproachCleared>(e); })
        .on_if(LandingState::Approach, LandingState::Flare,
            [](const LandingEvent& e){ return std::holds_alternative<FlareAltitude>(e); })
        .on_if(LandingState::Approach, LandingState::GoAround,
            [](const LandingEvent& e){ return std::holds_alternative<MissedApproach>(e); })
        .on_if(LandingState::Flare, LandingState::Rollout,
            [](const LandingEvent& e){ return std::holds_alternative<Touchdown>(e); })
        .on_if(LandingState::Rollout, LandingState::HoldingPattern,
            [](const LandingEvent& e){ return std::holds_alternative<DecelComplete>(e); })
        .on_if(LandingState::GoAround, LandingState::HoldingPattern,
            [](const LandingEvent& e){ return std::holds_alternative<GoAroundComplete>(e); })
        .build();
}

}  // namespace

TEST(LandingSMTest, NormalApproachAndLanding) {
    LandingConfig cfg;
    auto sm = make_landing_machine(cfg);
    EXPECT_EQ(sm.current(), LandingState::HoldingPattern);
    EXPECT_FALSE(cfg.clearedRunwayHeading.has_value());

    // Cleared to approach RWY 270.
    sm.process(LandingEvent{ApproachCleared{270.0}});
    EXPECT_EQ(sm.current(), LandingState::Approach);
    ASSERT_TRUE(cfg.clearedRunwayHeading.has_value());
    EXPECT_DOUBLE_EQ(*cfg.clearedRunwayHeading, 270.0);
    EXPECT_TRUE(cfg.gearCommandedDown);

    // Descend to flare altitude.
    sm.process(LandingEvent{FlareAltitude{}});
    EXPECT_EQ(sm.current(), LandingState::Flare);
    EXPECT_TRUE(cfg.flapsFull);

    // Touchdown.
    sm.process(LandingEvent{Touchdown{}});
    EXPECT_EQ(sm.current(), LandingState::Rollout);
    EXPECT_FALSE(cfg.flapsFull);  // retracted on rollout entry

    // Decelerate complete -> back to holding.
    sm.process(LandingEvent{DecelComplete{}});
    EXPECT_EQ(sm.current(), LandingState::HoldingPattern);
}

TEST(LandingSMTest, MissedApproachTriggersGoAround) {
    LandingConfig cfg;
    auto sm = make_landing_machine(cfg);
    sm.process(LandingEvent{ApproachCleared{90.0}});
    ASSERT_EQ(sm.current(), LandingState::Approach);
    // Abort.
    sm.process(LandingEvent{MissedApproach{}});
    EXPECT_EQ(sm.current(), LandingState::GoAround);
    EXPECT_FALSE(cfg.gearCommandedDown);  // cleaned up on GoAround entry
    // Complete the go-around -> back to holding.
    sm.process(LandingEvent{GoAroundComplete{}});
    EXPECT_EQ(sm.current(), LandingState::HoldingPattern);
}

TEST(LandingSMTest, GearDownOnlyEventIgnoredInHolding) {
    // GearDown event has no transition from HoldingPattern.
    LandingConfig cfg;
    auto sm = make_landing_machine(cfg);
    EXPECT_EQ(sm.process(LandingEvent{GearDown{}}), LandingState::HoldingPattern);
    EXPECT_FALSE(cfg.gearCommandedDown);  // entry action didn't fire (no transition)
}

TEST(LandingSMTest, CanFireOnVariantEvent) {
    LandingConfig cfg;
    auto sm = make_landing_machine(cfg);
    EXPECT_TRUE(sm.can_fire(LandingEvent{ApproachCleared{}}));
    EXPECT_FALSE(sm.can_fire(LandingEvent{FlareAltitude{}}));  // not from Holding
}

TEST(LandingSMTest, MatchesByTypeNotPayloadValue) {
    // Two ApproachCleared events with different headings both match the same
    // transition (matching is by variant TYPE via on_if, not by payload value).
    // The heading is read by the entry action, not used for matching. This is
    // the correct semantics for payload-carrying events: the runway heading is
    // data the action consumes, not part of the trigger identity.
    LandingConfig cfg;
    auto sm = make_landing_machine(cfg);
    sm.process(LandingEvent{ApproachCleared{180.0}});
    EXPECT_EQ(sm.current(), LandingState::Approach);
    ASSERT_TRUE(cfg.clearedRunwayHeading.has_value());
    EXPECT_DOUBLE_EQ(*cfg.clearedRunwayHeading, 180.0);
}
