// f4-campaign-api/tests/test_command_wire.cpp
//
// The command wire (CAMP_HOST_PLAN.md §3.3): every v1 intent parses off
// its canonical line, malformed input is rejected LOUDLY (the house's
// loud-failure discipline — a silent skip would let a client drift), and
// the ack encodes byte-stably.

#include <f4/campaign/api/commands.hpp>

#include <gtest/gtest.h>

#include <string>

using namespace f4::campaign::api;
using f4::json::Reader;

namespace {

std::string ack_json(const CommandAck& ack) {
    f4::json::Writer w;
    encode(w, ack);
    return w.str();
}

} // namespace

// ============================================================================
// roe_set — the P7 vocabulary rides the wire verbatim
// ============================================================================

TEST(CommandWire, ParsesRoeSetCanonical) {
    const char* line =
        R"({"v":1,"op":"command","intent":"roe_set","scope":{"kind":"team","team":1},"roe":2})";
    f4::json::Reader r(line);
    r.expect('{');
    ASSERT_EQ(r.read_string(), "v");
    r.expect(':');
    (void)r.read_int();
    (void)r.expect(',');
    (void)r.read_string(); // "op"
    r.expect(':');
    (void)r.read_string();
    (void)r.expect(',');
    (void)r.read_string(); // "intent"
    r.expect(':');
    const auto name = r.read_string();
    ASSERT_EQ(name, "roe_set");
    const auto cmd = parse_command_body(name, r);

    EXPECT_EQ(cmd.kind, CommandIntent::Kind::RoeSet);
    EXPECT_EQ(cmd.scope.kind, RoEScopeKind::Team);
    EXPECT_EQ(cmd.scope.team, 1);
    EXPECT_EQ(cmd.roe, RoeLevel::Hold);
}

TEST(CommandWire, RoeScopeKindsRoundTrip) {
    // flight-scoped: kind + flight id
    const char* line =
        R"({"kind":"flight","flight":118})";
    f4::json::Reader r(line);
    const auto scope = parse_roe_scope(r);
    EXPECT_EQ(scope.kind, RoEScopeKind::Flight);
    EXPECT_EQ(scope.flight, 118U);
    EXPECT_EQ(scope.team, 0);
}

TEST(CommandWire, RejectsRoeOutOfRange) {
    f4::json::Reader r("3");
    EXPECT_THROW(parse_roe_level(r.read_int()), std::runtime_error);
}

TEST(CommandWire, RejectsUnknownScopeKind) {
    // parse_command_body positions after the intent value's colon; feed
    // it a scope object with a bogus kind and expect the throw.
    f4::json::Reader r(R"(,"scope":{"kind":"navy","team":1},"roe":0})");
    EXPECT_THROW(
        (void)parse_command_body("roe_set", r), std::runtime_error);
}

// ============================================================================
// flight family — retask / abort / deagg / reagg share the flight key
// ============================================================================

TEST(CommandWire, ParsesFlightRetask) {
    f4::json::Reader r(R"(,"flight":118,"mission":9,"target":214})");
    const auto cmd = parse_command_body("flight_retask", r);
    EXPECT_EQ(cmd.kind, CommandIntent::Kind::FlightRetask);
    EXPECT_EQ(cmd.flight, 118U);
    EXPECT_EQ(cmd.mission_byte, 9);
    EXPECT_EQ(cmd.target_objective_id, 214U);
}

TEST(CommandWire, ParsesFlightAbortMinimal) {
    f4::json::Reader r(R"(,"flight":233})");
    const auto cmd = parse_command_body("flight_abort", r);
    EXPECT_EQ(cmd.kind, CommandIntent::Kind::FlightAbort);
    EXPECT_EQ(cmd.flight, 233U);
    EXPECT_EQ(cmd.mission_byte, 0);
}

TEST(CommandWire, RetaskRejectsUnknownKeys) {
    // the strict envelope: an unknown key is malformed, not a silent skip
    f4::json::Reader r(R"(,"bogus":1})");
    EXPECT_THROW((void)parse_command_body("flight_retask", r),
                 std::runtime_error);
}

// ============================================================================
// focus — the generalized camera bubble (plan §4)
// ============================================================================

TEST(CommandWire, ParsesFocus) {
    f4::json::Reader r(R"(,"x":5000.0,"y":-3000.0,"z":20000.0,"radius_ft":120000.0})");
    const auto cmd = parse_command_body("focus", r);
    EXPECT_EQ(cmd.kind, CommandIntent::Kind::Focus);
    EXPECT_DOUBLE_EQ(cmd.x, 5000.0);
    EXPECT_DOUBLE_EQ(cmd.y, -3000.0);
    EXPECT_DOUBLE_EQ(cmd.z, 20000.0);
    EXPECT_DOUBLE_EQ(cmd.radius_ft, 120000.0);
}

TEST(CommandWire, ParsesClearFocusEmptyBody) {
    f4::json::Reader r(R"(})");
    const auto cmd = parse_command_body("clear_focus", r);
    EXPECT_EQ(cmd.kind, CommandIntent::Kind::ClearFocus);
}

// ============================================================================
// the ack — typed refusals are data
// ============================================================================

TEST(CommandWire, AckAppliedGolden) {
    CommandAck ack;
    ack.status = CommandAck::Status::Applied;
    ack.detail = "view bubble set";
    ack.apply_tick = 38574360;
    EXPECT_EQ(ack_json(ack),
              R"({"status":"applied","refusal":"none","detail":"view bubble set","apply_tick":38574360})");
}

TEST(CommandWire, AckRefusedGolden) {
    CommandAck ack;
    ack.status = CommandAck::Status::Refused;
    ack.refusal = CommandAck::Refusal::NotImplemented;
    ack.detail = "CAMP-CMD-1 lands roe_set";
    ack.apply_tick = 1234;
    EXPECT_EQ(ack_json(ack),
              R"({"status":"refused","refusal":"not_implemented","detail":"CAMP-CMD-1 lands roe_set","apply_tick":1234})");
}

// CAMP-CMD-2 — the fourth refusal: no objective carries the id.
TEST(CommandWire, AckRefusedUnknownObjectiveGolden) {
    CommandAck ack;
    ack.status = CommandAck::Status::Refused;
    ack.refusal = CommandAck::Refusal::UnknownObjective;
    ack.detail = "no objective carries id 999999";
    ack.apply_tick = 99;
    EXPECT_EQ(ack_json(ack),
              R"({"status":"refused","refusal":"unknown_objective","detail":"no objective carries id 999999","apply_tick":99})");
}

// ============================================================================
// unknown intents are a protocol error, not a silent no-op
// ============================================================================

TEST(CommandWire, RejectsUnknownIntent) {
    f4::json::Reader r(R"(,"flight":1})");
    EXPECT_THROW((void)parse_command_body("launch_nukes", r),
             std::runtime_error);
}
