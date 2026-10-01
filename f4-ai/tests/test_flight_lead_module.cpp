// test_flight_lead_module.cpp — FlightLeadModule unit tests (Step 14).
//
// The module is pure: roster + echoes in, radio rows + orders out. These
// tests pin the CommandFlight() tranche's edge rules exactly:
//   1. REJOIN — ordered when the wingman's echo reports Rejoining,
//      latched until it reports Following, re-armed on the next blowout.
//   2. ENGAGE — ordered when the lead fights a bandit the wingman holds
//      and is not already on; follows the lead's target; re-arms when
//      the wingman joins; never fires without the wingman's eyes.
//   3. RTB — ordered once per flight at the wingman's bingo; takes the
//      lead home with it (v1's both-RTB).
// Plus the delivery contract (order_for consumes), the corpse rule, and
// the inert-by-construction shape (no roster = no rows).

#include <gtest/gtest.h>

#include "f4/ai/modules/flight_lead_module.hpp"

using f4::ai::modules::FlightLeadModule;
using f4::ai::modules::WingState;
using WingRadio = f4::ai::WingRadio;

namespace {

/// A live, on-station, non-bingo, idle wingman echo (tests mutate the
/// interesting fields from this baseline).
FlightLeadModule::WingmanEcho baseline(std::uint64_t id = 2) {
    FlightLeadModule::WingmanEcho e{};
    e.entity_id = id;
    e.alive = true;
    e.wing_state = WingState::Following;
    e.bingo = false;
    e.engaged_id = 0;
    e.sees_lead_target = false;
    return e;
}

} // namespace

// ============================================================================
// 1. REJOIN — the wingman's blowout, seen from the other side
// ============================================================================

TEST(FlightLeadModule, OrdersRejoinOnTheBlowoutEcho) {
    FlightLeadModule m;
    m.register_wingman(2);
    m.report_wingman(baseline());
    EXPECT_TRUE(m.update().empty());  // in station — nothing to command

    // The blowout: the wingman's own SM reports Rejoining.
    auto blown = baseline();
    blown.wing_state = WingState::Rejoining;
    m.report_wingman(blown);
    const auto rows = m.update();
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].peer_id, 2u);
    EXPECT_EQ(rows[0].event, WingRadio::OrderRejoin);

    // The order latches — no re-fire while the wingman stays out.
    m.report_wingman(blown);
    EXPECT_TRUE(m.update().empty());

    // Back on station: the latch re-arms.
    m.report_wingman(baseline());
    EXPECT_TRUE(m.update().empty());  // re-arm tick consumes the transition
    m.report_wingman(blown);
    const auto again = m.update();
    ASSERT_EQ(again.size(), 1u);
    EXPECT_EQ(again[0].event, WingRadio::OrderRejoin);
}

TEST(FlightLeadModule, RejoinOrderIsDeliveredOnce) {
    FlightLeadModule m;
    m.register_wingman(2);
    auto blown = baseline();
    blown.wing_state = WingState::Rejoining;
    m.report_wingman(blown);
    (void)m.update();

    // One delivery; a second read is empty (the host queues the order on
    // the wingman brain exactly once).
    const auto first = m.order_for(2);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->event, WingRadio::OrderRejoin);
    EXPECT_FALSE(m.order_for(2).has_value());
}

// ============================================================================
// 2. ENGAGE — the shared contact, ranked from the other side
// ============================================================================

TEST(FlightLeadModule, OrdersEngageOnTheSharedContact) {
    FlightLeadModule m;
    m.register_wingman(2);

    // The lead is not fighting: never an engage order.
    m.report_wingman(baseline());
    EXPECT_TRUE(m.update().empty());

    // The lead engages a bandit the wingman cannot see: no order (an
    // order can never conjure a track — sensor truth still wins).
    m.set_lead_engagement(100);
    EXPECT_TRUE(m.update().empty());

    // The wingman's fusion holds the bandit: the order goes out with
    // the bandit's id.
    auto sees = baseline();
    sees.sees_lead_target = true;
    m.report_wingman(sees);
    const auto rows = m.update();
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].peer_id, 2u);
    EXPECT_EQ(rows[0].event, WingRadio::OrderEngageMyTarget);
    EXPECT_EQ(rows[0].target_id, 100u);
}

TEST(FlightLeadModule, EngageLatchesUntilTheWingmanJoins) {
    FlightLeadModule m;
    m.register_wingman(2);
    m.set_lead_engagement(100);
    auto sees = baseline();
    sees.sees_lead_target = true;
    m.report_wingman(sees);
    (void)m.update();  // order out

    // Still off the target (and still seeing it): no re-fire.
    m.report_wingman(sees);
    EXPECT_TRUE(m.update().empty());

    // The wingman joins the fight on the ordered id: the latch re-arms.
    auto engaged = baseline();
    engaged.engaged_id = 100;
    engaged.sees_lead_target = true;
    m.report_wingman(engaged);
    EXPECT_TRUE(m.update().empty());

    // The bandit dies; the lead switches to a NEW bandit the wingman
    // sees: the order re-fires with the new id.
    auto sees_new = baseline();
    sees_new.sees_lead_target = true;
    m.report_wingman(sees_new);
    m.set_lead_engagement(200);
    const auto rows = m.update();
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].event, WingRadio::OrderEngageMyTarget);
    EXPECT_EQ(rows[0].target_id, 200u);
}

TEST(FlightLeadModule, EngageSkipsTheWingmanAlreadyOnIt) {
    FlightLeadModule m;
    m.register_wingman(2);
    m.set_lead_engagement(100);
    auto engaged = baseline();
    engaged.engaged_id = 100;
    engaged.sees_lead_target = true;
    m.report_wingman(engaged);
    EXPECT_TRUE(m.update().empty());  // the wingman is on it — say nothing
}

// ============================================================================
// 3. RTB — the wingman's bingo, v1 takes the lead home too
// ============================================================================

TEST(FlightLeadModule, OrdersRTBAtBingoAndTakesTheLeadHome) {
    FlightLeadModule m;
    m.register_wingman(2);
    m.report_wingman(baseline());
    EXPECT_FALSE(m.lead_rtb());

    auto dry = baseline();
    dry.bingo = true;
    m.report_wingman(dry);
    const auto rows = m.update();
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].peer_id, 2u);
    EXPECT_EQ(rows[0].event, WingRadio::OrderRTB);
    // v1's both-RTB: the lead's own stand-down latched.
    EXPECT_TRUE(m.lead_rtb());

    // One order per flight — a still-dry wingman stays quiet.
    m.report_wingman(dry);
    EXPECT_TRUE(m.update().empty());
    EXPECT_TRUE(m.lead_rtb());
}

// ============================================================================
// 4. The flight bookkeeping — corpses, duplicates, the inert shape
// ============================================================================

TEST(FlightLeadModule, DeadWingmanIsQuietAndClearsTheLatches) {
    FlightLeadModule m;
    m.register_wingman(2);

    // A corpse: no orders, latches cleared (the slot starts fresh if
    // the host ever reports the entity again).
    auto dead = baseline();
    dead.alive = false;
    m.report_wingman(dead);
    EXPECT_TRUE(m.update().empty());
    EXPECT_FALSE(m.order_for(2).has_value());
    EXPECT_FALSE(m.lead_rtb());

    // A dry corpse never fires the RTB rule either.
    dead.bingo = true;
    m.report_wingman(dead);
    EXPECT_TRUE(m.update().empty());
    EXPECT_FALSE(m.lead_rtb());
}

TEST(FlightLeadModule, DuplicateRegistrationCollapses) {
    FlightLeadModule m;
    m.register_wingman(2);
    m.register_wingman(2);  // the same wingman resolved twice (4-ship)
    EXPECT_EQ(m.roster_size(), 1u);

    auto blown = baseline();
    blown.wing_state = WingState::Rejoining;
    m.report_wingman(blown);
    const auto rows = m.update();
    ASSERT_EQ(rows.size(), 1u);  // one order, not one per registration
}

TEST(FlightLeadModule, NoRosterIsInert) {
    FlightLeadModule m;  // a single-ship "lead": nobody registered
    m.set_lead_engagement(100);
    EXPECT_TRUE(m.update().empty());
    EXPECT_FALSE(m.order_for(2).has_value());
    EXPECT_FALSE(m.lead_rtb());
}

TEST(FlightLeadModule, UnregisteredEchoIsIgnored) {
    FlightLeadModule m;
    m.register_wingman(3);  // the flight is #3, not #2
    m.report_wingman(baseline(2));  // someone else's wingman drifted in
    auto blown = baseline(2);
    blown.wing_state = WingState::Rejoining;
    m.report_wingman(blown);
    EXPECT_TRUE(m.update().empty());
    EXPECT_FALSE(m.order_for(2).has_value());
}

TEST(FlightLeadModule, ResetDissolvesTheFlight) {
    FlightLeadModule m;
    m.register_wingman(2);
    auto dry = baseline();
    dry.bingo = true;
    m.report_wingman(dry);
    (void)m.update();
    EXPECT_TRUE(m.lead_rtb());

    m.reset();  // the host re-tasks / the flight dissolves
    EXPECT_EQ(m.roster_size(), 0u);
    EXPECT_FALSE(m.lead_rtb());
    EXPECT_EQ(m.lead_engagement(), 0u);
}
