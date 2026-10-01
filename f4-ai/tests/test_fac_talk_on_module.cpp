// test_fac_talk_on_module.cpp — the Step-15 talk-on unit tests
// (AI_IMPLEMENTATION_PLAN.md §16 Step 15; FreeFalcon facbrain.cpp's
// target marking + talk-on).
//
// The module is pure (no world, no bus — the brain/host resolve every
// input), so these tests exercise the talk-on rule's whole surface
// without an entity world:
//
//   1. the gates: no mark / not on station / no addressee / a dead
//      mark / a dead addressee each hold the row back
//   2. the BRA arithmetic: the bearing is FROM the addressee TO the
//      mark, degrees true (000 north, 090 east, 180 south, 270 west),
//      the range the horizontal ground distance
//   3. the message shape: sender = the FAC, peer = the assigned
//      flight, target = the mark, the closed GroundAssets description
//   4. the one-shot latch: one talk-on per mark (v1 marks ONE target);
//      a re-set after the latch is ignored; reset() is the only
//      re-arm path
//   5. the addressee picture is consumed with the talk-on (a stale
//      position can never put a second BRA on the radio)
//   6. the strike-side hint API (BrainComponent::offer_fac_talk_on):
//      a talk-on row with a target latches the id; other rows and
//      target-less rows are ignored
//
// The brain-level arcs (the orbit, the delivery) live in the f4-sim
// E2E (test_support_fac_e2e.cpp) — this suite owns the module's edges.

#include <gtest/gtest.h>

#include <f4/ai/brain_component.hpp>
#include <f4/ai/modules/fac_talk_on_module.hpp>
#include <f4/ai/wingradio.hpp>

#include <cmath>

using namespace f4::ai;
using namespace f4::ai::modules;
namespace geo = f4::geo;

namespace {

constexpr double kPi = 3.14159265358979323846;

FacTalkOnModule::AddresseeEcho make_addressee(double x, double y,
                                              bool alive = true) {
    FacTalkOnModule::AddresseeEcho e{};
    e.entity_id = 77;  // "STRIKE1"
    e.position = geo::WorldPosition(x, y, 15000.0);
    e.alive = alive;
    return e;
}

FacTalkOnModule::MarkPicture make_mark(double x, double y,
                                       bool alive = true) {
    FacTalkOnModule::MarkPicture m{};
    m.position = geo::WorldPosition(x, y, 0.0);
    m.alive = alive;
    return m;
}

// The all-gates-aligned input: a live mark, a live addressee, on station.
FacTalkOnModule::TalkOnInput on_station() { return {true}; }

} // namespace

// ── 1. the gates ─────────────────────────────────────────────────────

TEST(FacTalkOnModule, NoMarkIsInert) {
    FacTalkOnModule m;
    auto row = m.update(42, make_mark(0.0, 10000.0), on_station(), 10.0);
    EXPECT_FALSE(row.has_value());
    EXPECT_FALSE(m.published());
}

TEST(FacTalkOnModule, NotOnStationHoldsTheRow) {
    FacTalkOnModule m;
    m.set_mark(900);
    // Short of the station: the talk-on waits (the tick's picture is
    // consumed regardless — the host pushes a FRESH one each tick).
    m.report_addressee(make_addressee(0.0, 0.0));
    auto row = m.update(42, make_mark(0.0, 10000.0), {false}, 10.0);
    EXPECT_FALSE(row.has_value());
    // Established, with the next tick's push: it goes out.
    m.report_addressee(make_addressee(0.0, 0.0));
    row = m.update(42, make_mark(0.0, 10000.0), on_station(), 11.0);
    ASSERT_TRUE(row.has_value());
}

TEST(FacTalkOnModule, NoAddresseeHoldsTheRow) {
    FacTalkOnModule m;
    m.set_mark(900);
    auto row = m.update(42, make_mark(0.0, 10000.0), on_station(), 10.0);
    EXPECT_FALSE(row.has_value());
}

TEST(FacTalkOnModule, DeadMarkIsNeverTalkedOn) {
    FacTalkOnModule m;
    m.set_mark(900);
    m.report_addressee(make_addressee(0.0, 0.0));
    auto row =
        m.update(42, make_mark(0.0, 10000.0, false), on_station(), 10.0);
    EXPECT_FALSE(row.has_value());
    EXPECT_FALSE(m.published());
}

TEST(FacTalkOnModule, DeadAddresseeIsNeverTalkedOn) {
    FacTalkOnModule m;
    m.set_mark(900);
    m.report_addressee(make_addressee(0.0, 0.0, false));
    auto row = m.update(42, make_mark(0.0, 10000.0), on_station(), 10.0);
    EXPECT_FALSE(row.has_value());
    EXPECT_FALSE(m.published());
}

// ── 2. the BRA arithmetic (bearing FROM the addressee TO the mark) ──

TEST(FacTalkOnModule, BraNorth) {
    FacTalkOnModule m;
    m.set_mark(900);
    m.report_addressee(make_addressee(0.0, 0.0));
    auto row = m.update(42, make_mark(0.0, 20000.0), on_station(), 10.0);
    ASSERT_TRUE(row.has_value());
    EXPECT_NEAR(row->target_bra_deg, 0.0, 1e-6);   // 000
    EXPECT_NEAR(row->target_range_ft, 20000.0, 1e-6);
}

TEST(FacTalkOnModule, BraEast) {
    FacTalkOnModule m;
    m.set_mark(900);
    m.report_addressee(make_addressee(0.0, 0.0));
    auto row = m.update(42, make_mark(20000.0, 0.0), on_station(), 10.0);
    ASSERT_TRUE(row.has_value());
    EXPECT_NEAR(row->target_bra_deg, 90.0, 1e-6);  // 090
}

TEST(FacTalkOnModule, BraSouthAndWestNormalize) {
    FacTalkOnModule m;
    m.set_mark(900);
    m.report_addressee(make_addressee(0.0, 0.0));
    auto row = m.update(42, make_mark(0.0, -20000.0), on_station(), 10.0);
    ASSERT_TRUE(row.has_value());
    EXPECT_NEAR(row->target_bra_deg, 180.0, 1e-6);  // 180

    FacTalkOnModule m2;
    m2.set_mark(900);
    m2.report_addressee(make_addressee(0.0, 0.0));
    row = m2.update(42, make_mark(-20000.0, 0.0), on_station(), 10.0);
    ASSERT_TRUE(row.has_value());
    EXPECT_NEAR(row->target_bra_deg, 270.0, 1e-6);  // 270
}

TEST(FacTalkOnModule, BraDiagonal) {
    // The mark sits NORTHEAST of the addressee at 45 deg: the bearing
    // (from the addressee, radio convention) reads 045.
    FacTalkOnModule m;
    m.set_mark(900);
    m.report_addressee(make_addressee(-1000.0, -1000.0));
    auto row = m.update(42, make_mark(0.0, 0.0), on_station(), 10.0);
    ASSERT_TRUE(row.has_value());
    EXPECT_NEAR(row->target_bra_deg, 45.0, 1e-6);
    EXPECT_NEAR(row->target_range_ft,
                std::sqrt(2.0) * 1000.0, 1e-6);
}

// ── 3. the message shape ─────────────────────────────────────────────

TEST(FacTalkOnModule, MessageShape) {
    FacTalkOnModule m;
    m.set_mark(900);
    m.report_addressee(make_addressee(0.0, 0.0));
    auto row = m.update(42, make_mark(0.0, 20000.0), on_station(), 123.5);
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ(row->sender_id, 42u);      // the FAC speaks
    EXPECT_EQ(row->peer_id, 77u);        // to the assigned flight
    EXPECT_EQ(row->target_id, 900u);     // about the mark
    EXPECT_EQ(row->event, WingRadio::FacTalkOn);
    EXPECT_DOUBLE_EQ(row->time_s, 123.5);  // the host's clock
    EXPECT_EQ(row->target_desc, TalkOnDesc::GroundAssets);
}

// ── 4. the one-shot latch ────────────────────────────────────────────

TEST(FacTalkOnModule, OneShotPerMark) {
    FacTalkOnModule m;
    m.set_mark(900);
    m.report_addressee(make_addressee(0.0, 0.0));
    auto row = m.update(42, make_mark(0.0, 20000.0), on_station(), 10.0);
    ASSERT_TRUE(row.has_value());
    EXPECT_TRUE(m.published());

    // The second tick (same everything) is silent — v1 marks ONE target.
    m.report_addressee(make_addressee(0.0, 0.0));
    row = m.update(42, make_mark(0.0, 20000.0), on_station(), 11.0);
    EXPECT_FALSE(row.has_value());

    // A re-set after the latch is ignored (reset() is the only way back).
    m.set_mark(901);
    m.report_addressee(make_addressee(0.0, 0.0));
    row = m.update(42, make_mark(0.0, 20000.0), on_station(), 12.0);
    EXPECT_FALSE(row.has_value());
    EXPECT_EQ(m.mark_id(), 900u);  // the original mark stands
}

TEST(FacTalkOnModule, ReSetBeforeTheLatchRetargets) {
    FacTalkOnModule m;
    m.set_mark(900);
    m.set_mark(901);  // pre-latch re-target (the authoring fix-up path)
    m.report_addressee(make_addressee(0.0, 0.0));
    auto row = m.update(42, make_mark(0.0, 20000.0), on_station(), 10.0);
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ(row->target_id, 901u);
}

TEST(FacTalkOnModule, ResetRearms) {
    FacTalkOnModule m;
    m.set_mark(900);
    m.report_addressee(make_addressee(0.0, 0.0));
    ASSERT_TRUE(m.update(42, make_mark(0.0, 20000.0), on_station(), 10.0)
                    .has_value());
    m.reset();
    EXPECT_FALSE(m.published());
    EXPECT_EQ(m.mark_id(), 0u);

    // A fresh mark + a FRESH addressee picture (the last one was
    // consumed with the first talk-on) re-arms the whole rule.
    m.set_mark(902);
    m.report_addressee(make_addressee(0.0, 0.0));
    auto row = m.update(42, make_mark(0.0, 20000.0), on_station(), 20.0);
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ(row->target_id, 902u);
}

TEST(FacTalkOnModule, AddresseePictureIsConsumed) {
    FacTalkOnModule m;
    m.set_mark(900);
    m.report_addressee(make_addressee(0.0, 0.0));
    // The rule does not fire yet (not on station) — but the picture is
    // THIS tick's: the next update without a fresh push must not use it.
    (void)m.update(42, make_mark(0.0, 20000.0), {false}, 10.0);
    auto row = m.update(42, make_mark(0.0, 20000.0), on_station(), 11.0);
    EXPECT_FALSE(row.has_value());  // nobody to talk on this tick
}

// ── 6. the strike-side hint API ──────────────────────────────────────

TEST(FacTalkOnHint, TalkOnRowLatchesTheMark) {
    BrainComponent bc;
    WingRadioMessage m{};
    m.sender_id = 42;
    m.peer_id = 77;
    m.target_id = 900;
    m.event = WingRadio::FacTalkOn;
    bc.offer_fac_talk_on(m);
    EXPECT_EQ(bc.talk_on_target_id(), 900u);
}

TEST(FacTalkOnHint, NonTalkOnRowsAreIgnored) {
    BrainComponent bc;
    WingRadioMessage m{};
    m.sender_id = 42;
    m.peer_id = 77;
    m.target_id = 900;
    m.event = WingRadio::OrderEngageMyTarget;
    bc.offer_fac_talk_on(m);
    EXPECT_EQ(bc.talk_on_target_id(), 0u);
}

TEST(FacTalkOnHint, TargetlessRowsAreIgnored) {
    BrainComponent bc;
    WingRadioMessage m{};
    m.sender_id = 42;
    m.peer_id = 77;
    m.target_id = 0;
    m.event = WingRadio::FacTalkOn;
    bc.offer_fac_talk_on(m);
    EXPECT_EQ(bc.talk_on_target_id(), 0u);
}

TEST(FacTalkOnHint, FacProfileImpliesSupport) {
    BrainComponent bc;
    EXPECT_FALSE(bc.is_support_profile());
    EXPECT_FALSE(bc.is_fac());
    bc.set_fac(true);
    EXPECT_TRUE(bc.is_fac());
    EXPECT_TRUE(bc.is_support_profile());  // the FAC is a station aircraft
}

// ── the drain (one slot, consumed) ───────────────────────────────────

TEST(FacTalkOnHint, DrainConsumes) {
    BrainComponent bc;
    WingRadioMessage m{};
    m.target_id = 900;
    m.event = WingRadio::FacTalkOn;
    bc.offer_fac_talk_on(m);
    // The offer only sets the hint; the DRAIN reads the publisher's
    // stash — empty here (no update ran).
    EXPECT_FALSE(bc.drain_fac_talk_on().has_value());
    EXPECT_FALSE(bc.drain_fac_talk_on().has_value());  // stays drained
}
