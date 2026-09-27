// f4-world-viewer/src/event_log.cpp
//
// The Event Log's model + formatters — see event_log.hpp. The window
// itself is ViewerApp::draw_event_log_view in campaign_session_view.cpp
// (this file stays ImGui-free so the store + formatters unit-test
// without a GL context).

#include "event_log.hpp"

#include <f4/viewer/enum_text.hpp>   // format_campaign_time (D# HH:MM:SS)

#include <f4/campaign/api/commands.hpp>  // RoEScope/RoeLevel (roe_changed face)

#include <cstdio>
#include <cstring>
#include <utility>

namespace f4::viewer {

void event_log_append(EventLogStore& log,
                      const f4::campaign::api::CampaignEvent& ev,
                      double wall_now, std::int64_t epoch_s,
                      const std::function<std::string(std::uint32_t)>&
                          objective_name,
                      std::size_t cap) {
    EventLogRow row;
    row.arrived_wall = wall_now;
    row.abs_t = epoch_s + campaign_event_time(ev);
    row.team = campaign_event_team(ev);
    format_campaign_event_label(ev, objective_name, row.label,
                                EventLogRow::kLabelCap);
    log.push_back(std::move(row));
    while (log.size() > cap) log.pop_front();
}

std::int64_t campaign_event_time(
    const f4::campaign::api::CampaignEvent& ev) noexcept {
    using K = f4::campaign::api::CampaignEvent::Kind;
    switch (ev.kind) {
        case K::MissionFiled:          return ev.mission_filed.t;
        case K::Kill:                  return ev.kill.t;
        case K::ObjectiveDamage:       return ev.objective_damage.t;
        case K::ObjectiveCaptured:     return ev.objective_captured.t;
        case K::ReinforcementDelivered:
                                       return ev.reinforcement_delivered.t;
        case K::WeatherChanged:        return ev.weather_changed.t;
        case K::RoeChanged:            return ev.roe_changed.t;
        case K::TaskingCycle:          return ev.tasking_cycle.t;
        case K::ActionFiled:           return ev.action_filed.t;
        case K::Verdict:               return ev.verdict.t;
        case K::ObjectiveRepaired:     return ev.objective_repaired.t;
        case K::PilotAssigned:         return ev.pilot_assigned.t;
        case K::PilotLost:             return ev.pilot_lost.t;
        case K::PilotRecovered:        return ev.pilot_recovered.t;
        case K::SlotDenied:            return ev.slot_denied.t;
    }
    return 0;
}

std::uint8_t campaign_event_team(
    const f4::campaign::api::CampaignEvent& ev) noexcept {
    using K = f4::campaign::api::CampaignEvent::Kind;
    switch (ev.kind) {
        case K::MissionFiled:          return ev.mission_filed.team;
        case K::Kill:                  return ev.kill.killer_team;
        case K::ObjectiveDamage:       return ev.objective_damage.owner;
        case K::ObjectiveCaptured:     return ev.objective_captured.new_owner;
        case K::ObjectiveRepaired:     return ev.objective_repaired.owner;
        case K::ActionFiled:           return ev.action_filed.team;
        case K::PilotAssigned:         return ev.pilot_assigned.team;
        case K::PilotLost:             return ev.pilot_lost.team;
        case K::PilotRecovered:        return ev.pilot_recovered.team;
        case K::SlotDenied:            return ev.slot_denied.team;
        case K::RoeChanged:
            return ev.roe_changed.scope.kind ==
                           f4::campaign::api::RoEScopeKind::Team
                       ? ev.roe_changed.scope.team
                       : 0xFF;
        case K::ReinforcementDelivered:
        case K::WeatherChanged:
        case K::TaskingCycle:
        case K::Verdict:
            return 0xFF;
    }
    return 0xFF;
}

bool format_campaign_event_label(
    const f4::campaign::api::CampaignEvent& ev,
    const std::function<std::string(std::uint32_t)>& objective_name,
    char* buf, std::size_t cap) {
    using K = f4::campaign::api::CampaignEvent::Kind;
    const auto name_or_id = [&](std::uint32_t id) {
        const std::string nm = objective_name ? objective_name(id)
                                              : std::string{};
        return nm.empty() ? "#" + std::to_string(id) : nm;
    };
    switch (ev.kind) {
        case K::MissionFiled:
            std::snprintf(buf, cap, "mission: %s team %u -> %s",
                          ev.mission_filed.mission_name.c_str(),
                          ev.mission_filed.team,
                          name_or_id(
                              ev.mission_filed.target_objective_id).c_str());
            return true;
        case K::Kill:
            std::snprintf(buf, cap,
                          "air kill: team %u downed team %u (%s) [%u vs %u]",
                          ev.kill.killer_team, ev.kill.victim_team,
                          ev.kill.weapon.c_str(),
                          ev.kill.killer_squadron, ev.kill.victim_squadron);
            return true;
        case K::ObjectiveDamage:
            std::snprintf(buf, cap, "objective damaged: %s (%u features)",
                          name_or_id(
                              ev.objective_damage.objective_id).c_str(),
                          ev.objective_damage.features_damaged);
            return true;
        case K::ObjectiveCaptured:
            std::snprintf(buf, cap, "CAPTURED: %s -> team %u",
                          name_or_id(
                              ev.objective_captured.objective_id).c_str(),
                          ev.objective_captured.new_owner);
            return true;
        case K::ObjectiveRepaired:
            std::snprintf(buf, cap, "repaired %u features at %s",
                          ev.objective_repaired.features_repaired,
                          name_or_id(
                              ev.objective_repaired.objective_id).c_str());
            return true;
        case K::ReinforcementDelivered:
            std::snprintf(buf, cap,
                          "reinforcements: %d aircraft (%d squadrons)",
                          ev.reinforcement_delivered.aircraft,
                          ev.reinforcement_delivered.squadrons_touched);
            return true;
        case K::WeatherChanged:
            std::snprintf(buf, cap, "weather: %s",
                          ev.weather_changed.condition.c_str());
            return true;
        case K::TaskingCycle:
            std::snprintf(buf, cap, "tasking cycle: %d intents",
                          ev.tasking_cycle.intents);
            return true;
        case K::ActionFiled:
            std::snprintf(buf, cap,
                          "action filed: %s (team %u, %d%% damage)",
                          ev.action_filed.mission_name.c_str(),
                          ev.action_filed.team,
                          ev.action_filed.damage_pct);
            return true;
        case K::Verdict:
            std::snprintf(buf, cap,
                          "verdict: %s (leader team %d, swing %d)",
                          ev.verdict.band.c_str(), ev.verdict.leader,
                          ev.verdict.swing);
            return true;
        case K::PilotAssigned:
            std::snprintf(buf, cap,
                          "pilots assigned: team %u flight %u — %u crew",
                          ev.pilot_assigned.team, ev.pilot_assigned.flight,
                          static_cast<unsigned>(
                              ev.pilot_assigned.pilots.size()));
            return true;
        case K::PilotLost:
            std::snprintf(buf, cap,
                          "pilot lost: team %u flight %u, roster slot #%u",
                          ev.pilot_lost.team, ev.pilot_lost.flight,
                          ev.pilot_lost.pilot);
            return true;
        case K::PilotRecovered:
            std::snprintf(buf, cap,
                          "pilot home: team %u flight %u, slot #%u "
                          "(%d sorties this run)",
                          ev.pilot_recovered.team, ev.pilot_recovered.flight,
                          ev.pilot_recovered.pilot,
                          ev.pilot_recovered.missions_run);
            return true;
        case K::RoeChanged: {
            const char* level =
                ev.roe_changed.roe == f4::campaign::api::RoeLevel::Free
                    ? "FREE"
                    : ev.roe_changed.roe ==
                              f4::campaign::api::RoeLevel::Tight
                          ? "TIGHT"
                          : "HOLD";
            char scope[48];
            switch (ev.roe_changed.scope.kind) {
                case f4::campaign::api::RoEScopeKind::Team:
                    std::snprintf(scope, sizeof(scope), "team %u",
                                  ev.roe_changed.scope.team);
                    break;
                case f4::campaign::api::RoEScopeKind::Mission:
                    std::snprintf(scope, sizeof(scope), "mission %u",
                                  ev.roe_changed.scope.mission);
                    break;
                case f4::campaign::api::RoEScopeKind::Flight:
                    std::snprintf(scope, sizeof(scope), "flight %u",
                                  static_cast<unsigned>(
                                      ev.roe_changed.scope.flight));
                    break;
                default:
                    std::snprintf(scope, sizeof(scope), "?");
                    break;
            }
            std::snprintf(buf, cap, "RoE %s: %s", level, scope);
            return true;
        }
        case K::SlotDenied:
            std::snprintf(buf, cap, "slot denied at %s: team %u (%s)",
                          name_or_id(ev.slot_denied.airbase).c_str(),
                          ev.slot_denied.team,
                          ev.slot_denied.reason == 1
                              ? "horizon full"
                              : "block full");
            return true;
    }
    return false;
}

bool event_log_matches(const char* filter, const char* label) noexcept {
    if (!filter || !*filter) return true;
    if (!label) return false;
    const auto lower = [](char c) -> char {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a')
                                      : c;
    };
    // O(n*m) on purpose: rows are ~100 chars, the needle is a word.
    for (const char* h = label; *h; ++h) {
        const char* hp = h;
        const char* np = filter;
        while (*np && lower(*hp) == lower(*np)) {
            ++hp;
            ++np;
        }
        if (!*np) return true;
    }
    return false;
}

void format_event_log_time(std::int64_t t, char* buf,
                           std::size_t buf_size) noexcept {
    if (t < INT32_MIN || t > INT32_MAX) {
        std::snprintf(buf, buf_size, "%lld", static_cast<long long>(t));
        return;
    }
    format_campaign_time(static_cast<std::int32_t>(t), buf, buf_size);
}

} // namespace f4::viewer
