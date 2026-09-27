// f4-world-viewer/tests/test_event_log.cpp
//
// Unit tests for the Event Log's model + formatters (event_log.hpp/cpp
// — deliberately ImGui-free). No GL context: the store and the label
// formatter are pure functions over the CAMP-HOST-2 event vocabulary.
//
// The coverage that matters: EVERY v1 event family formats a face. The
// Campaign Session window's old compact feed silently dropped the
// pilot/roe/slot families (`default: return false`); the log's contract
// is that it never does — this test pins that for all 15 kinds.

#include "../src/event_log.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <string>

using namespace f4::viewer;
namespace api = f4::campaign::api;

namespace {

const std::function<std::string(std::uint32_t)> kNamer =
    [](std::uint32_t id) -> std::string {
        return id == 7 ? std::string("Seoul") : std::string{};
    };

// One filled event per family — every payload field the formatter reads
// carries its display value.
api::CampaignEvent make_event(api::CampaignEvent::Kind kind) {
    api::CampaignEvent ev;
    ev.kind = kind;
    switch (kind) {
        case api::CampaignEvent::Kind::MissionFiled:
            ev.mission_filed = {100, 5, 6, 1, 2, "OCA", 7, false};
            break;
        case api::CampaignEvent::Kind::Kill:
            ev.kill = {101, 11, 1, 12, 2, "AIM-120"};
            break;
        case api::CampaignEvent::Kind::ObjectiveDamage:
            ev.objective_damage = {102, 7, 1, 4};
            break;
        case api::CampaignEvent::Kind::ObjectiveCaptured:
            ev.objective_captured = {103, 7, 2};
            break;
        case api::CampaignEvent::Kind::ObjectiveRepaired:
            ev.objective_repaired = {104, 7, 1, 3, 5, 90};
            break;
        case api::CampaignEvent::Kind::ReinforcementDelivered:
            ev.reinforcement_delivered = {105, 8, 2};
            break;
        case api::CampaignEvent::Kind::WeatherChanged:
            ev.weather_changed = {106, "rain"};
            break;
        case api::CampaignEvent::Kind::RoeChanged:
            ev.roe_changed.t = 107;
            ev.roe_changed.scope =
                api::RoEScope{api::RoEScopeKind::Team, 1, 0, 0};
            ev.roe_changed.roe = api::RoeLevel::Tight;
            break;
        case api::CampaignEvent::Kind::TaskingCycle:
            ev.tasking_cycle = {108, 3, 1800, 12};
            break;
        case api::CampaignEvent::Kind::ActionFiled:
            ev.action_filed = {109, 2, 9, "SEADSTRIKE", 2, 21, 7, 55};
            break;
        case api::CampaignEvent::Kind::Verdict:
            ev.verdict = {110, "advantage", 1, 250};
            break;
        case api::CampaignEvent::Kind::PilotAssigned:
            ev.pilot_assigned = {111, 1, 21, 22, {3, 4}};
            break;
        case api::CampaignEvent::Kind::PilotLost:
            ev.pilot_lost = {112, 1, 21, 22, 3};
            break;
        case api::CampaignEvent::Kind::PilotRecovered:
            ev.pilot_recovered = {113, 1, 21, 22, 3, 2};
            break;
        case api::CampaignEvent::Kind::SlotDenied:
            ev.slot_denied = {114, 1, 7, 1};
            break;
    }
    return ev;
}

} // namespace

// ── every family has a face ───────────────────────────────────────────────

TEST(EventLogFormat, EveryKindFormats) {
    for (const auto k : {
             api::CampaignEvent::Kind::MissionFiled,
             api::CampaignEvent::Kind::Kill,
             api::CampaignEvent::Kind::ObjectiveDamage,
             api::CampaignEvent::Kind::ObjectiveCaptured,
             api::CampaignEvent::Kind::ObjectiveRepaired,
             api::CampaignEvent::Kind::ReinforcementDelivered,
             api::CampaignEvent::Kind::WeatherChanged,
             api::CampaignEvent::Kind::RoeChanged,
             api::CampaignEvent::Kind::TaskingCycle,
             api::CampaignEvent::Kind::ActionFiled,
             api::CampaignEvent::Kind::Verdict,
             api::CampaignEvent::Kind::PilotAssigned,
             api::CampaignEvent::Kind::PilotLost,
             api::CampaignEvent::Kind::PilotRecovered,
             api::CampaignEvent::Kind::SlotDenied,
         }) {
        char buf[192] = "";
        ASSERT_TRUE(format_campaign_event_label(make_event(k), kNamer,
                                                buf, sizeof(buf)))
            << "kind index " << static_cast<int>(k);
        ASSERT_NE(buf[0], '\0') << "kind index " << static_cast<int>(k);
    }
}

TEST(EventLogFormat, ResolvesObjectiveNames) {
    char buf[192];
    ASSERT_TRUE(format_campaign_event_label(
        make_event(api::CampaignEvent::Kind::ObjectiveCaptured), kNamer,
        buf, sizeof(buf)));
    EXPECT_NE(std::strstr(buf, "Seoul"), nullptr);
    // An unknown id carries the raw number instead of a blank.
    auto ev = make_event(api::CampaignEvent::Kind::ObjectiveCaptured);
    ev.objective_captured.objective_id = 8;
    ASSERT_TRUE(format_campaign_event_label(ev, kNamer, buf, sizeof(buf)));
    EXPECT_NE(std::strstr(buf, "#8"), nullptr);
}

TEST(EventLogFormat, TargetlessMissionsSaySo) {
    // The defensive family (BARCAP/HAVCAP/escort) files with no ground
    // target — the row must not render the placeholder "#0".
    auto ev = make_event(api::CampaignEvent::Kind::MissionFiled);
    ev.mission_filed.target_objective_id = 0;
    char buf[192];
    ASSERT_TRUE(format_campaign_event_label(ev, kNamer, buf, sizeof(buf)));
    EXPECT_NE(std::strstr(buf, "(no target)"), nullptr);
    EXPECT_EQ(std::strstr(buf, "#0"), nullptr);
}

TEST(EventLogFormat, PersonnelFamiliesHaveFaces) {
    char buf[192];
    ASSERT_TRUE(format_campaign_event_label(
        make_event(api::CampaignEvent::Kind::PilotLost), kNamer, buf,
        sizeof(buf)));
    EXPECT_NE(std::strstr(buf, "pilot lost"), nullptr);
    ASSERT_TRUE(format_campaign_event_label(
        make_event(api::CampaignEvent::Kind::PilotRecovered), kNamer,
        buf, sizeof(buf)));
    EXPECT_NE(std::strstr(buf, "pilot home"), nullptr);
    ASSERT_TRUE(format_campaign_event_label(
        make_event(api::CampaignEvent::Kind::RoeChanged), kNamer, buf,
        sizeof(buf)));
    EXPECT_NE(std::strstr(buf, "TIGHT"), nullptr);
    EXPECT_NE(std::strstr(buf, "team 1"), nullptr);
    ASSERT_TRUE(format_campaign_event_label(
        make_event(api::CampaignEvent::Kind::SlotDenied), kNamer, buf,
        sizeof(buf)));
    EXPECT_NE(std::strstr(buf, "horizon full"), nullptr);
}

// ── the envelope accessors ────────────────────────────────────────────────

TEST(EventLogEnvelope, TimeReadsPerFamily) {
    const api::CampaignEvent ev =
        make_event(api::CampaignEvent::Kind::ActionFiled);
    EXPECT_EQ(campaign_event_time(ev), 109);
    auto alt = make_event(api::CampaignEvent::Kind::WeatherChanged);
    alt.weather_changed.t = 77;
    EXPECT_EQ(campaign_event_time(alt), 77);
}

TEST(EventLogEnvelope, TeamReadsPerFamily) {
    EXPECT_EQ(campaign_event_team(
                  make_event(api::CampaignEvent::Kind::MissionFiled)),
              1);
    EXPECT_EQ(campaign_event_team(
                  make_event(api::CampaignEvent::Kind::SlotDenied)),
              1);
    // Teamless families draw no dot.
    EXPECT_EQ(campaign_event_team(
                  make_event(api::CampaignEvent::Kind::Verdict)),
              0xFF);
    EXPECT_EQ(campaign_event_team(
                  make_event(api::CampaignEvent::Kind::WeatherChanged)),
              0xFF);
    // A mission-scoped RoE change is teamless; a team-scoped one is not.
    auto roe = make_event(api::CampaignEvent::Kind::RoeChanged);
    roe.roe_changed.scope.kind = api::RoEScopeKind::Mission;
    EXPECT_EQ(campaign_event_team(roe), 0xFF);
}

// ── the store ─────────────────────────────────────────────────────────────

TEST(EventLogStore, AppendFreezesRowAndCaps) {
    EventLogStore log;
    const auto ev =
        make_event(api::CampaignEvent::Kind::ObjectiveCaptured);
    event_log_append(log, ev, 12.5, 1000, kNamer);
    ASSERT_EQ(log.size(), 1u);
    EXPECT_EQ(log.front().abs_t, 1000 + 103);
    EXPECT_EQ(log.front().team, 2);
    EXPECT_DOUBLE_EQ(log.front().arrived_wall, 12.5);
    EXPECT_NE(std::strstr(log.front().label, "Seoul"), nullptr);

    for (int i = 0; i < 2100; ++i) {
        event_log_append(log, ev, static_cast<double>(i), 0, kNamer, 2000);
    }
    EXPECT_EQ(log.size(), 2000u);
    // Oldest fall off the front; order is preserved (newest last).
    EXPECT_EQ(log.back().arrived_wall, 2099.0);
}

TEST(EventLogFilter, SubstringCaseInsensitive) {
    EXPECT_TRUE(event_log_matches("", "anything"));
    EXPECT_TRUE(event_log_matches(nullptr, "anything"));
    EXPECT_TRUE(event_log_matches("CAPTURED",
                                  "05:00:00  CAPTURED: Seoul -> team 2"));
    EXPECT_TRUE(event_log_matches("captured",
                                  "05:00:00  CAPTURED: Seoul -> team 2"));
    EXPECT_FALSE(event_log_matches("verdict", "CAPTURED: Seoul"));
}
