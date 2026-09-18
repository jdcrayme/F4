// f4-campaign-api/include/f4/campaign/api/events.hpp
//
// Surface 4 — the v1 event vocabulary (CAMP_HOST_PLAN.md §3.4). The
// envelope families and their field SHAPES are versioned here, in v1, so
// HOST-2 can wire emission without touching the wire: a client written
// against this header survives the tranche that makes the engine emit.
//
// HOST-1 does NOT emit any of these — the engine's diary/ledger keep
// being the books (the emitter lives in HOST-2, where the journal and
// its replay identity land). The encoders exist and are tested so the
// vocabulary is pinned before anything produces it.
//
// Delivery rules (plan §3.4): events are journaled at engine rate
// (complete) and delivered per step() return, filtered by the host's
// subscription. No real-time scheduling in the engine — ever.
//
// THE TIME AXIS (HOST-2 as built): every event's `t` is the ENGINE's
// own relative clock, llround'd to whole seconds — the same axis the
// ledger's books carry (a kill books m.sim_time_s, a capture books the
// ground war's clock). A host that wants the war's absolute time adds
// the save epoch it already holds (hello's campaign_time_s of a fresh
// session IS the epoch). One axis everywhere; no emitter needs the
// epoch to publish.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <f4/campaign/api/commands.hpp>
#include <f4/json/writer.hpp>

namespace f4::campaign::api {

// A mission filed by the tasking pipeline (C4) — including the support
// family's share-or-file (P7: FindSupportFlights) and the reactive
// enemy CAP (RequestEnemyMission).
struct MissionFiledEvent {
    std::int64_t t{0};                 ///< campaign time
    std::uint32_t package_id{0};
    std::uint32_t flight_id{0};
    std::uint8_t team{0};
    std::uint8_t mission_byte{0};
    std::string mission_name;
    std::uint32_t target_objective_id{0};
    bool synthetic{false};
};

// An air-to-air kill (the C1 ledger books it; the event is the same
// shape the books key on — killer/victim resolve to squadrons + teams).
struct KillEvent {
    std::int64_t t{0};
    std::uint32_t killer_squadron{0};
    std::uint8_t killer_team{0};
    std::uint32_t victim_squadron{0};
    std::uint8_t victim_team{0};
    std::string weapon;
};

// An objective's damage bitmap changed (the C1 fstatus diff).
struct ObjectiveDamageEvent {
    std::int64_t t{0};
    std::uint32_t objective_id{0};
    std::uint8_t owner{0};             ///< the owner AFTER the damage
    std::uint32_t features_damaged{0}; ///< count of features with damage bits
};

// An objective changed hands (G1).
struct ObjectiveCapturedEvent {
    std::int64_t t{0};
    std::uint32_t objective_id{0};
    std::uint8_t new_owner{0};
};

// An objective's features were repaired (DOM-2 — the repair cadence's
// event face; the repaired bitmap itself rides the ledger's damage
// state and the write-back, the event carries the books' summary).
struct ObjectiveRepairedEvent {
    std::int64_t t{0};
    std::uint32_t objective_id{0};
    std::uint8_t owner{0};              ///< the holding team
    std::uint32_t features_repaired{0}; ///< flipped to VIS_REPAIRED
    std::uint32_t features_destroyed{0};///< still VIS_DESTROYED after
    std::uint8_t supply{0};             ///< the objective's stock after
};

// A reinforcement fire delivered aircraft (C2).
struct ReinforcementDeliveredEvent {
    std::int64_t t{0};
    int aircraft{0};
    int squadrons_touched{0};
};

// The weather chain turned (Task 73).
struct WeatherChangedEvent {
    std::int64_t t{0};
    std::string condition;             ///< the 3-state chain's wire name
};

// A RoE value changed (P7's byte riding the pipeline; the command's
// eventual ack — CAMP-CMD-1).
struct RoeChangedEvent {
    std::int64_t t{0};
    RoEScope scope{};
    RoeLevel roe{RoeLevel::Free};
};

// A tasking cycle fired (the 7-phase ATM pass).
struct TaskingCycleEvent {
    std::int64_t t{0};
    int cycles{0};
    int next_tasking_sec{0};
    int intents{0};
};

// An ACTION-table filing (CAMP-ATM-1): the strategy layer's
// objective-damage-driven contextual request — the war REACTING. Own
// damage files the defense (CAS, plus a garrison BARCAP when the
// damage is heavy); enemy damage files the suppression (SEADSTRIKE).
// action_type: 1 = Defend (own objective), 2 = Punish (enemy
// objective), 3 = Sweep (the ladder's sweep lines). context: the
// driving objective's own type byte. damage_pct: the destroyed
// features that drove the filing (0..100).
struct ActionFiledEvent {
    std::int64_t t{0};
    std::uint8_t team{0};
    std::uint8_t mission_byte{0};
    std::string mission_name;
    std::uint8_t action_type{0};
    std::uint8_t context{0};
    std::uint32_t objective_id{0};
    int damage_pct{0};
};

// The verdict's coarse state changed (CAMP-DOM-1) — the books'
// projection moved a band or found a (new) leader. The SPARSE war
// signal: the full rows live on the `verdict` query; the event is the
// wake-up call a war-room client reacts to. Fires only on CHANGE — a
// quiet front publishes nothing. leader is the leading team's SLOT
// (-1 when the lead dissolved); swing is the leader's net gained
// priority (0 when none).
struct VerdictEvent {
    std::int64_t t{0};
    std::string band;      ///< stalemate | advantage | decisive
    int leader{-1};
    int swing{0};
};

// --- encoders (byte-stable; the family name is the discriminator) -------

inline void encode(f4::json::Writer& w, const MissionFiledEvent& e) {
    w.raw("{\"ev\":\"mission_filed\",\"t\":");
    w.number(static_cast<long long>(e.t));
    w.raw(",\"package_id\":");
    w.number(static_cast<std::uint64_t>(e.package_id));
    w.raw(",\"flight_id\":");
    w.number(static_cast<std::uint64_t>(e.flight_id));
    w.raw(",\"team\":");
    w.number(e.team);
    w.raw(",\"mission_byte\":");
    w.number(e.mission_byte);
    w.raw(",\"mission_name\":\"");
    w.put(f4::json::escape_string(e.mission_name));
    w.raw("\",\"target_objective_id\":");
    w.number(static_cast<std::uint64_t>(e.target_objective_id));
    w.raw(",\"synthetic\":");
    w.raw(e.synthetic ? "1" : "0");
    w.put('}');
}

inline void encode(f4::json::Writer& w, const KillEvent& e) {
    w.raw("{\"ev\":\"kill\",\"t\":");
    w.number(static_cast<long long>(e.t));
    w.raw(",\"killer\":{\"sq\":");
    w.number(static_cast<std::uint64_t>(e.killer_squadron));
    w.raw(",\"team\":");
    w.number(e.killer_team);
    w.raw("},\"victim\":{\"sq\":");
    w.number(static_cast<std::uint64_t>(e.victim_squadron));
    w.raw(",\"team\":");
    w.number(e.victim_team);
    w.raw("},\"weapon\":\"");
    w.put(f4::json::escape_string(e.weapon));
    w.raw("\"}");
}

inline void encode(f4::json::Writer& w, const ObjectiveDamageEvent& e) {
    w.raw("{\"ev\":\"objective_damage\",\"t\":");
    w.number(static_cast<long long>(e.t));
    w.raw(",\"objective_id\":");
    w.number(static_cast<std::uint64_t>(e.objective_id));
    w.raw(",\"owner\":");
    w.number(e.owner);
    w.raw(",\"features_damaged\":");
    w.number(static_cast<std::uint64_t>(e.features_damaged));
    w.put('}');
}

inline void encode(f4::json::Writer& w, const ObjectiveCapturedEvent& e) {
    w.raw("{\"ev\":\"objective_captured\",\"t\":");
    w.number(static_cast<long long>(e.t));
    w.raw(",\"objective_id\":");
    w.number(static_cast<std::uint64_t>(e.objective_id));
    w.raw(",\"new_owner\":");
    w.number(e.new_owner);
    w.put('}');
}

inline void encode(f4::json::Writer& w, const ObjectiveRepairedEvent& e) {
    w.raw("{\"ev\":\"objective_repaired\",\"t\":");
    w.number(static_cast<long long>(e.t));
    w.raw(",\"objective_id\":");
    w.number(static_cast<std::uint64_t>(e.objective_id));
    w.raw(",\"owner\":");
    w.number(e.owner);
    w.raw(",\"features_repaired\":");
    w.number(static_cast<std::uint64_t>(e.features_repaired));
    w.raw(",\"features_destroyed\":");
    w.number(static_cast<std::uint64_t>(e.features_destroyed));
    w.raw(",\"supply\":");
    w.number(e.supply);
    w.put('}');
}

inline void encode(f4::json::Writer& w, const ReinforcementDeliveredEvent& e) {
    w.raw("{\"ev\":\"reinforcement_delivered\",\"t\":");
    w.number(static_cast<long long>(e.t));
    w.raw(",\"aircraft\":");
    w.number(e.aircraft);
    w.raw(",\"squadrons_touched\":");
    w.number(e.squadrons_touched);
    w.put('}');
}

inline void encode(f4::json::Writer& w, const WeatherChangedEvent& e) {
    w.raw("{\"ev\":\"weather_changed\",\"t\":");
    w.number(static_cast<long long>(e.t));
    w.raw(",\"condition\":\"");
    w.put(f4::json::escape_string(e.condition));
    w.raw("\"}");
}

inline void encode(f4::json::Writer& w, const RoeChangedEvent& e) {
    w.raw("{\"ev\":\"roe_changed\",\"t\":");
    w.number(static_cast<long long>(e.t));
    w.raw(",\"scope\":");
    encode_roe_scope(w, e.scope);
    w.raw(",\"roe\":");
    w.number(static_cast<unsigned>(e.roe));
    w.put('}');
}

inline void encode(f4::json::Writer& w, const TaskingCycleEvent& e) {
    w.raw("{\"ev\":\"tasking_cycle\",\"t\":");
    w.number(static_cast<long long>(e.t));
    w.raw(",\"cycles\":");
    w.number(e.cycles);
    w.raw(",\"next_tasking_sec\":");
    w.number(e.next_tasking_sec);
    w.raw(",\"intents\":");
    w.number(e.intents);
    w.put('}');
}

inline void encode(f4::json::Writer& w, const ActionFiledEvent& e) {
    w.raw("{\"ev\":\"action_filed\",\"t\":");
    w.number(static_cast<long long>(e.t));
    w.raw(",\"team\":");
    w.number(e.team);
    w.raw(",\"mission_byte\":");
    w.number(e.mission_byte);
    w.raw(",\"mission_name\":\"");
    w.put(f4::json::escape_string(e.mission_name));
    w.raw("\",\"action_type\":");
    w.number(e.action_type);
    w.raw(",\"context\":");
    w.number(e.context);
    w.raw(",\"objective_id\":");
    w.number(static_cast<std::uint64_t>(e.objective_id));
    w.raw(",\"damage_pct\":");
    w.number(e.damage_pct);
    w.put('}');
}

inline void encode(f4::json::Writer& w, const VerdictEvent& e) {
    w.raw("{\"ev\":\"verdict\",\"t\":");
    w.number(static_cast<long long>(e.t));
    w.raw(",\"band\":\"");
    w.put(f4::json::escape_string(e.band));
    w.raw("\",\"leader\":");
    w.number(e.leader);
    w.raw(",\"swing\":");
    w.number(e.swing);
    w.put('}');
}

// --- the tagged envelope (the ONE bus message type) ---------------------
//
// HOST-2 publishes ONE message type onto the session's bus — the bus is
// type-indexed, and a single type means one subscription point for every
// sink (the journal, the wire buffer) and one arrival order = engine
// occurrence order. The payloads are the pinned structs above; unused
// members stay default-constructed.
struct CampaignEvent {
    enum class Kind : std::uint8_t {
        MissionFiled,
        Kill,
        ObjectiveDamage,
        ObjectiveCaptured,
        ReinforcementDelivered,
        WeatherChanged,
        RoeChanged,
        TaskingCycle,
        ActionFiled,
        Verdict,
        ObjectiveRepaired,
    };

    Kind kind{Kind::TaskingCycle};

    MissionFiledEvent mission_filed{};
    KillEvent kill{};
    ObjectiveDamageEvent objective_damage{};
    ObjectiveCapturedEvent objective_captured{};
    ReinforcementDeliveredEvent reinforcement_delivered{};
    WeatherChangedEvent weather_changed{};
    RoeChangedEvent roe_changed{};
    TaskingCycleEvent tasking_cycle{};
    ActionFiledEvent action_filed{};
    VerdictEvent verdict{};
    ObjectiveRepairedEvent objective_repaired{};
};

// The v1 kind names — the wire's filter vocabulary (the `subscribe`
// op's "kinds" list; the journal stays unfiltered by design).
[[nodiscard]] inline std::string_view
event_kind_name(CampaignEvent::Kind k) noexcept {
    switch (k) {
        case CampaignEvent::Kind::MissionFiled:          return "mission_filed";
        case CampaignEvent::Kind::Kill:                  return "kill";
        case CampaignEvent::Kind::ObjectiveDamage:       return "objective_damage";
        case CampaignEvent::Kind::ObjectiveCaptured:     return "objective_captured";
        case CampaignEvent::Kind::ReinforcementDelivered: return "reinforcement_delivered";
        case CampaignEvent::Kind::WeatherChanged:        return "weather_changed";
        case CampaignEvent::Kind::RoeChanged:            return "roe_changed";
        case CampaignEvent::Kind::TaskingCycle:          return "tasking_cycle";
        case CampaignEvent::Kind::ActionFiled:           return "action_filed";
        case CampaignEvent::Kind::Verdict:               return "verdict";
        case CampaignEvent::Kind::ObjectiveRepaired:     return "objective_repaired";
    }
    return "tasking_cycle";
}

/// Exact-name parse (the protocol maps a false return to malformed).
[[nodiscard]] inline bool
parse_event_kind(std::string_view name, CampaignEvent::Kind& out) noexcept {
    for (const auto k : {
             CampaignEvent::Kind::MissionFiled,
             CampaignEvent::Kind::Kill,
             CampaignEvent::Kind::ObjectiveDamage,
             CampaignEvent::Kind::ObjectiveCaptured,
             CampaignEvent::Kind::ReinforcementDelivered,
             CampaignEvent::Kind::WeatherChanged,
             CampaignEvent::Kind::RoeChanged,
             CampaignEvent::Kind::TaskingCycle,
             CampaignEvent::Kind::ActionFiled,
             CampaignEvent::Kind::Verdict,
             CampaignEvent::Kind::ObjectiveRepaired,
         }) {
        if (name == event_kind_name(k)) {
            out = k;
            return true;
        }
    }
    return false;
}

inline void encode(f4::json::Writer& w, const CampaignEvent& e) {
    switch (e.kind) {
        case CampaignEvent::Kind::MissionFiled:           encode(w, e.mission_filed); break;
        case CampaignEvent::Kind::Kill:                   encode(w, e.kill); break;
        case CampaignEvent::Kind::ObjectiveDamage:        encode(w, e.objective_damage); break;
        case CampaignEvent::Kind::ObjectiveCaptured:      encode(w, e.objective_captured); break;
        case CampaignEvent::Kind::ReinforcementDelivered: encode(w, e.reinforcement_delivered); break;
        case CampaignEvent::Kind::WeatherChanged:         encode(w, e.weather_changed); break;
        case CampaignEvent::Kind::RoeChanged:             encode(w, e.roe_changed); break;
        case CampaignEvent::Kind::TaskingCycle:           encode(w, e.tasking_cycle); break;
        case CampaignEvent::Kind::ActionFiled:            encode(w, e.action_filed); break;
        case CampaignEvent::Kind::Verdict:                encode(w, e.verdict); break;
        case CampaignEvent::Kind::ObjectiveRepaired:      encode(w, e.objective_repaired); break;
    }
}

// --- the subscription filter (the wire's, not the journal's) ------------
//
// The journal is engine-rate and COMPLETE (plan §3.4); the wire is what
// the host subscribed to. `all` short-circuits the kind gate; an empty
// `teams` list means every team. Team matching is per family (an event
// belongs to the sides its payload names):
//
//   mission_filed           the filing team
//   kill                    killer OR victim (a war-room sees both)
//   objective_damage        the owner after the damage
//   objective_captured      the new owner
//   objective_repaired      the holding team (the base being rebuilt
//                           is theirs — the same side objective_
//                           damage reports)
//   action_filed            the filing team (the ACTION reacts FOR
//                           them — the owner defends, the striker
//                           punishes)
//   reinforcement_delivered teamless in v1 (matches any team gate)
//   weather_changed         teamless (matches any team gate)
//   roe_changed             the scope's team when scoped to a team;
//                           mission/flight scopes match any team gate
//   tasking_cycle           teamless (the ATM pass is theater-wide)
//   verdict                 teamless (the war's outcome is theater-
//                           wide; the rows name the teams)
struct EventFilter {
    bool all{false};
    std::vector<CampaignEvent::Kind> kinds;
    std::vector<int> teams;
};

[[nodiscard]] inline bool
listed(const std::vector<int>& teams, int team) noexcept {
    if (teams.empty()) return true;
    for (const auto t : teams) {
        if (t == team) return true;
    }
    return false;
}

[[nodiscard]] inline bool matches(const EventFilter& f,
                                  const CampaignEvent& e) noexcept {
    if (!f.all) {
        bool kind_ok = false;
        for (const auto k : f.kinds) {
            if (k == e.kind) {
                kind_ok = true;
                break;
            }
        }
        if (!kind_ok) return false;
    }
    switch (e.kind) {
        case CampaignEvent::Kind::MissionFiled:
            return listed(f.teams, e.mission_filed.team);
        case CampaignEvent::Kind::ActionFiled:
            return listed(f.teams, e.action_filed.team);
        case CampaignEvent::Kind::Kill:
            return listed(f.teams, e.kill.killer_team) ||
                   listed(f.teams, e.kill.victim_team);
        case CampaignEvent::Kind::ObjectiveDamage:
            return listed(f.teams, e.objective_damage.owner);
        case CampaignEvent::Kind::ObjectiveCaptured:
            return listed(f.teams, e.objective_captured.new_owner);
        case CampaignEvent::Kind::ObjectiveRepaired:
            return listed(f.teams, e.objective_repaired.owner);
        case CampaignEvent::Kind::RoeChanged:
            return e.roe_changed.scope.kind == RoEScopeKind::Team
                       ? listed(f.teams, e.roe_changed.scope.team)
                       : true;
        case CampaignEvent::Kind::ReinforcementDelivered:
        case CampaignEvent::Kind::WeatherChanged:
        case CampaignEvent::Kind::TaskingCycle:
        case CampaignEvent::Kind::Verdict:
            return true;
    }
    return true;
}

} // namespace f4::campaign::api
