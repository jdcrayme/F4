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

#pragma once

#include <cstdint>
#include <string>

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
    w.raw(",\"scope\":{\"kind\":\"");
    switch (e.scope.kind) {
        case RoEScopeKind::Team:    w.raw("team");    break;
        case RoEScopeKind::Mission: w.raw("mission"); break;
        case RoEScopeKind::Flight:  w.raw("flight");  break;
    }
    w.raw("\",\"team\":");
    w.number(e.scope.team);
    w.raw(",\"mission\":");
    w.number(e.scope.mission);
    w.raw(",\"flight\":");
    w.number(static_cast<std::uint64_t>(e.scope.flight));
    w.raw("},\"roe\":");
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

} // namespace f4::campaign::api
