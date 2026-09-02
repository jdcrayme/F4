// f4-simulation/src/campaign_spawner.cpp
//
// CampaignSimSpawner implementation — see campaign_spawner.hpp for the
// B.3 loop this closes and the ownership rules.

#include "f4/simulation/campaign_spawner.hpp"

#include <f4/entities/entity.hpp>
#include <f4/ai/brain_component.hpp>

#include <cstdio>

namespace f4::simulation {

CampaignSimSpawner::CampaignSimSpawner(
        f4::entities::EntityWorld& world,
        std::unordered_map<std::uint32_t, f4::entities::EntityId> unit_id_map,
        const f4::world_convert::ClassTable& ct,
        const f4::models::ModelDatabase& db,
        const f4::data::AircraftConfig& cfg,
        const ScenarioAirfield& airfield,
        const ScenarioAircraft& scenario_aircraft,
        FlightSpawnFilter filter)
    : world_(world),
      unit_id_map_(std::move(unit_id_map)),
      ct_(ct),
      db_(db),
      cfg_(cfg),
      airfield_(airfield),
      tpl_(scenario_aircraft),
      filter_(filter) {}

std::size_t CampaignSimSpawner::attach(f4::messaging::MessageBus& bus) {
    subscription_ = bus.subscribe<f4::campaign::MissionIntent>(
        [this](const f4::campaign::MissionIntent& intent) {
            handle(intent);
        });
    return subscription_;
}

void CampaignSimSpawner::detach(f4::messaging::MessageBus& bus) {
    if (subscription_ == static_cast<std::size_t>(-1)) return;
    bus.unsubscribe<f4::campaign::MissionIntent>(subscription_);
    subscription_ = static_cast<std::size_t>(-1);
}

void CampaignSimSpawner::handle(const f4::campaign::MissionIntent& intent) {
    ++stats_.intents_seen;

    // Intent-side filter (the same FlightSpawnFilter vocabulary the bulk
    // path and the scenario JSON use — applied to the intent's own team /
    // mission bytes, which the campaign filled from the flight).
    if (filter_.team >= 0 &&
        static_cast<int>(intent.team) != filter_.team) {
        return;
    }
    if (filter_.mission >= 0 &&
        static_cast<int>(intent.mission_byte) != filter_.mission) {
        return;
    }
    if (filter_.max_flights > 0 &&
        stats_.aircraft_spawned >= filter_.max_flights) {
        return;
    }

    // Resolve the flight entity. Synthetic-ladder intents carry counter
    // ids that never resolve — they're counted, not errors.
    const auto it = unit_id_map_.find(intent.flight_id);
    if (it == unit_id_map_.end() || !it->second.valid()) {
        ++stats_.unknown_flight_ids;
        return;
    }
    {
        f4::entities::EntityHandle dbg(it->second, &world_);
        const auto* dbg_fp = dbg.get<f4::entities::FlightPlanComponent>();
        if (!dbg_fp) {
            std::fprintf(stderr, "[dbg] intent flight_id=%u resolved to a "
                         "non-flight entity\n", intent.flight_id);
        }
    }

    // Duplicate guard: one aircraft per flight, no matter how often the
    // intent is republished.
    if (spawned_flight_ids_.count(intent.flight_id) != 0) {
        ++stats_.duplicate_skips;
        return;
    }

    // Per-airbase parking slot — the same bookkeeping the bulk path uses
    // (keyed on the flight's squadron airbase; 0 = shared fallback).
    f4::entities::EntityId airbase_id;
    {
        f4::entities::EntityHandle flight_h(it->second, &world_);
        const auto* fp = flight_h.get<f4::entities::FlightPlanComponent>();
        if (fp && fp->squadron.value != 0) {
            auto* sq = f4::entities::EntityHandle(fp->squadron, &world_)
                           .get<f4::entities::SquadronComponent>();
            if (sq && sq->airbase.value != 0) {
                airbase_id = sq->airbase;
            }
        }
    }
    const int slot = per_airbase_index_[airbase_id.value]++;

    // The shared Milestone-A spawn path: routes attached, TEAM tag mapped,
    // per-airbase parking — identical to the bulk path's output. The A-G
    // tranche passes the objective id map (waypoint strike-target
    // resolution) and the built-in weapon table (loadout arming) when the
    // host supplied them; null table = unarmed spawns (the pre-A-G
    // behavior for single-purpose route QC).
    auto spawned_id = spawn_aircraft_for_flight(
        world_, it->second, ct_, db_, cfg_, airfield_, tpl_, slot,
        nullptr, objective_id_map_, weapon_table_);
    if (!spawned_id) {
        // Resolved entity wasn't a flight (corrupt map?). Count it as
        // unknown rather than failing the loop.
        ++stats_.unknown_flight_ids;
        return;
    }

    // Route bookkeeping for the QC summary.
    {
        f4::entities::EntityHandle h(*spawned_id, &world_);
        auto* brain = h.get<f4::ai::BrainComponent>();
        if (brain && !brain->mission_plan().route.empty()) {
            ++stats_.routes_attached;
        }
    }

    spawned_flight_ids_.insert(intent.flight_id);
    spawned_.push_back(*spawned_id);
    ++stats_.aircraft_spawned;
}

} // namespace f4::simulation
