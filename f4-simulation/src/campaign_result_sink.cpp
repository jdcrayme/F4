// f4-simulation/src/campaign_result_sink.cpp
//
// CampaignResultSink implementation — see campaign_result_sink.hpp for
// the classification rules and the final-state-sync rationale.

#include <f4/simulation/campaign_result_sink.hpp>

#include <f4/simulation/campaign_origin.hpp>
#include <f4/weapons/bomb_battery.hpp>   // objective_damage_summary

#include <algorithm>

namespace f4::simulation {

using f4::entities::DamageBitmapComponent;
using f4::entities::EntityHandle;
using f4::entities::EntityId;
using f4::entities::FeatureSetComponent;
using f4::entities::PropertyBag;

CampaignResultSink::CampaignResultSink(
        f4::campaign::CampaignResultLedger& ledger,
        f4::entities::EntityWorld& world)
    : ledger_(ledger), world_(world) {
    snapshot_objectives_();
}

void CampaignResultSink::snapshot_objectives_() {
    // const-cast: the handle API needs a mutable world ref; this method
    // only reads (the entity system has no const-handle view).
    auto& mut_world = const_cast<f4::entities::EntityWorld&>(world_);
    for (const auto id : world_.with_component<FeatureSetComponent>()) {
        EntityHandle h(id, &mut_world);
        const auto* fs = h.get<FeatureSetComponent>();
        if (fs == nullptr || fs->features.empty()) continue;

        ObjectiveSnapshot snap;
        snap.entity = id.value;
        const auto* pb = h.get<PropertyBag>();
        if (pb) {
            const auto it = pb->ints.find("vu_id_num");
            if (it != pb->ints.end()) {
                snap.vu = static_cast<std::uint32_t>(it->second);
            }
        }
        const auto* db = h.get<DamageBitmapComponent>();
        if (db) snap.fstatus = db->fstatus;
        // Destroyed count + weighted pct: the same reading
        // objective_damage_summary() does, at snapshot time.
        double value_total = 0.0, value_destroyed = 0.0;
        for (const auto& f : fs->features) {
            const double weight = (f.value > 0) ? f.value : 1.0;
            value_total += weight;
            if (f.damage_state == 3) {   // kVisDestroyed (f4-weapons)
                ++snap.features_destroyed;
                value_destroyed += weight;
            }
        }
        snap.destroyed_pct_x100 = (value_total > 0.0)
            ? static_cast<int>(10000.0 * value_destroyed / value_total + 0.5)
            : 0;
        objective_snapshots_.push_back(std::move(snap));
    }
}

std::size_t CampaignResultSink::attach(f4::messaging::MessageBus& bus) {
    kill_subscription_ = bus.subscribe<f4::weapons::EntityKilledMessage>(
        [this](const f4::weapons::EntityKilledMessage& m) {
            handle_kill(m);
        });
    impact_subscription_ = bus.subscribe<f4::weapons::BombImpactMessage>(
        [this](const f4::weapons::BombImpactMessage& m) {
            handle_bomb_impact(m);
        });
    return kill_subscription_;
}

void CampaignResultSink::detach(f4::messaging::MessageBus& bus) {
    if (kill_subscription_ != static_cast<std::size_t>(-1)) {
        bus.unsubscribe<f4::weapons::EntityKilledMessage>(
            kill_subscription_);
        kill_subscription_ = static_cast<std::size_t>(-1);
    }
    if (impact_subscription_ != static_cast<std::size_t>(-1)) {
        bus.unsubscribe<f4::weapons::BombImpactMessage>(
            impact_subscription_);
        impact_subscription_ = static_cast<std::size_t>(-1);
    }
}

const CampaignOriginComponent*
CampaignResultSink::origin_of_(EntityId id) {
    if (!id.valid()) return nullptr;
    EntityHandle h(id, &world_);
    return h.get<CampaignOriginComponent>();
}

void CampaignResultSink::handle_kill(
        const f4::weapons::EntityKilledMessage& m) {
    ++stats_.kills_seen;

    const CampaignOriginComponent* victim = origin_of_(
        EntityId{m.target_id});
    const CampaignOriginComponent* killer = origin_of_(
        EntityId{m.shooter_id});

    if (victim != nullptr) {
        // Air loss: the victim was a campaign aircraft. Team pool −1,
        // squadron loss, aa credit when the killer is one of ours.
        ledger_.apply_air_loss(
            m.sim_time_s,
            victim->team_slot,
            victim->squadron_vu,
            victim->flight_vu,
            killer != nullptr ? killer->squadron_vu : 0);
        ++stats_.air_losses_recorded;
        if (killer != nullptr) {
            ++stats_.kills_attributed;
        }
        return;
    }

    if (killer != nullptr) {
        // Origin-less victim, origin-ful shooter: air-to-ground credit.
        // The victim's OWN ledger (battalion rosters, the ground-war
        // tranche) does not exist yet — the credit side is what the
        // campaign books today.
        ledger_.apply_ag_kill(m.sim_time_s, killer->squadron_vu);
        ++stats_.ag_kills_recorded;
        return;
    }

    // Neither side resolves (two scenario entities, a feature entity,
    // a missile-vs-missile kill). Counted, visible, unbooked.
    ++stats_.kills_unclassified;
}

void CampaignResultSink::handle_bomb_impact(
        const f4::weapons::BombImpactMessage& m) {
    ++stats_.bomb_impacts_seen;

    // Resolve the strike target to its objective VU for the log (the
    // damage itself is state on the entity — the sync reads it).
    std::uint32_t objective_vu = 0;
    if (m.target_id != 0) {
        EntityHandle h(EntityId{m.target_id}, &world_);
        const auto* pb = h.get<PropertyBag>();
        if (pb) {
            const auto it = pb->ints.find("vu_id_num");
            if (it != pb->ints.end()) {
                objective_vu = static_cast<std::uint32_t>(it->second);
            }
        }
    }
    ledger_.apply_bomb_impact(m.sim_time_s, objective_vu,
                              m.miss_distance_ft, m.features_destroyed);
}

void CampaignResultSink::sync_objective_damage() {
    auto& mut_world = const_cast<f4::entities::EntityWorld&>(world_);
    for (const auto& snap : objective_snapshots_) {
        EntityHandle h(EntityId{snap.entity}, &mut_world);

        // Current state — the same read the snapshot took.
        const auto* fs = h.get<FeatureSetComponent>();
        if (fs == nullptr || fs->features.empty()) continue;
        double value_total = 0.0, value_destroyed = 0.0;
        int destroyed = 0;
        for (const auto& f : fs->features) {
            const double weight = (f.value > 0) ? f.value : 1.0;
            value_total += weight;
            if (f.damage_state == 3) {
                ++destroyed;
                value_destroyed += weight;
            }
        }
        const int pct_x100 = (value_total > 0.0)
            ? static_cast<int>(10000.0 * value_destroyed / value_total + 0.5)
            : 0;
        const auto* db = h.get<DamageBitmapComponent>();
        const std::vector<std::uint8_t> current_fstatus =
            db ? db->fstatus : std::vector<std::uint8_t>{};

        // Diff against the snapshot: unchanged objectives are NOT sent
        // (a mid-campaign save's pre-existing damage stays save damage —
        // only THIS RUN's deltas become result records).
        if (destroyed == snap.features_destroyed &&
            pct_x100 == snap.destroyed_pct_x100 &&
            current_fstatus == snap.fstatus) {
            continue;
        }

        f4::campaign::ObjectiveDamageRecord rec;
        rec.objective = snap.vu;
        rec.features_total = static_cast<int>(fs->features.size());
        rec.features_destroyed = destroyed;
        rec.destroyed_pct = (pct_x100 + 50) / 100;  // hundredths → percent
        rec.fstatus = current_fstatus;
        ledger_.apply_objective_damage(rec);
        ++stats_.objectives_synced;
    }
}

} // namespace f4::simulation
