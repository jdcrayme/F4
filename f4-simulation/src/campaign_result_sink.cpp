// f4-simulation/src/campaign_result_sink.cpp
//
// CampaignResultSink implementation — see campaign_result_sink.hpp for
// the classification rules and the final-state-sync rationale.

#include <f4/simulation/campaign_result_sink.hpp>

#include <f4/simulation/campaign_origin.hpp>
#include <f4/weapons/bomb_battery.hpp>   // objective_damage_summary
#include <f4/entities/entity.hpp>        // UnitCoreComponent, PropertyBag

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
    unit_loss_subscription_ =
        bus.subscribe<f4::weapons::GroundUnitLossMessage>(
            [this](const f4::weapons::GroundUnitLossMessage& m) {
                handle_unit_loss(m);
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
    if (unit_loss_subscription_ != static_cast<std::size_t>(-1)) {
        bus.unsubscribe<f4::weapons::GroundUnitLossMessage>(
            unit_loss_subscription_);
        unit_loss_subscription_ = static_cast<std::size_t>(-1);
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
        // G1: when the victim is a BATTALION entity (the campaign's
        // ground unit — bubble-deaggregated vehicles are separate
        // entities and stay the deagg-parenting tranche's), the
        // victim's own ledger books too: one vehicle loss on the
        // battalion, air-sourced, provenance-carried. The ground war
        // engine PULLS these events on its next update and thins the
        // line accordingly — air power shapes the ground fight.
        // (G2's AGGREGATE blast path rides GroundUnitLossMessage —
        // handle_unit_loss above; THIS branch is the entity-kill path:
        // deaggregated vehicles today, missile/gun-vs-battalion later.)
        std::uint32_t victim_battalion = 0;
        std::uint8_t victim_team = 0;
        const EntityId vid{m.target_id};
        if (vid.valid()) {
            EntityHandle vh(vid, &world_);
            const auto* uc = vh.get<f4::entities::UnitCoreComponent>();
            if (uc != nullptr &&
                uc->unit_class == f4::entities::UnitClass::Battalion) {
                const auto* pb = vh.get<f4::entities::PropertyBag>();
                if (pb != nullptr) {
                    const auto it = pb->ints.find("vu_id_num");
                    if (it != pb->ints.end() && it->second > 0) {
                        victim_battalion =
                            static_cast<std::uint32_t>(it->second);
                    }
                }
                if (const auto t =
                        vh.get_tag(f4::entities::tags::TEAM);
                    t.has_value() && t->as_int() != nullptr) {
                    victim_team = static_cast<std::uint8_t>(
                        *t->as_int());
                }
            }
        }
        ledger_.apply_ag_kill(m.sim_time_s, killer->squadron_vu);
        if (victim_battalion != 0) {
            ledger_.apply_ground_loss(
                m.sim_time_s, victim_battalion, victim_team, 0, 0,
                /*kills=*/1, /*air_source=*/true,
                killer->squadron_vu);
        }
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

void CampaignResultSink::handle_unit_loss(
        const f4::weapons::GroundUnitLossMessage& m) {
    ++stats_.unit_losses_seen;
    if (!book_unit_losses_ || m.vehicles_killed <= 0) return;

    // The victim's campaign keys (vu + team), resolved the same way the
    // entity-kill branch resolves them; the shooter's squadron for the
    // credit (an origin-ful aircraft — the release stamped it).
    std::uint32_t victim_battalion = 0;
    std::uint8_t victim_team = 0;
    const EntityId vid{m.target_id};
    if (vid.valid()) {
        EntityHandle vh(vid, &world_);
        if (const auto* uc =
                vh.get<f4::entities::UnitCoreComponent>();
            uc != nullptr &&
            uc->unit_class == f4::entities::UnitClass::Battalion) {
            const auto* pb = vh.get<PropertyBag>();
            if (pb != nullptr) {
                const auto it = pb->ints.find("vu_id_num");
                if (it != pb->ints.end() && it->second > 0) {
                    victim_battalion =
                        static_cast<std::uint32_t>(it->second);
                }
            }
            if (const auto t = vh.get_tag(f4::entities::tags::TEAM);
                t.has_value() && t->as_int() != nullptr) {
                victim_team = static_cast<std::uint8_t>(*t->as_int());
            }
        }
    }
    if (victim_battalion == 0) {
        // The battalion no longer resolves (a stale target id after the
        // entity's removal): counted, loud, not booked — the boundary
        // rule; the engine's pull would drop it anyway.
        return;
    }

    const CampaignOriginComponent* killer = origin_of_(
        EntityId{m.shooter_id});
    const std::uint32_t killer_squadron =
        (killer != nullptr) ? killer->squadron_vu : 0;

    // The books: one ground-loss record (air-sourced, provenance-
    // carried — the engine pulls it on its next update and thins the
    // line) + per-vehicle ag credit (the reference counts vehicles, and
    // so does the squadron book).
    ledger_.apply_ground_loss(
        m.sim_time_s, victim_battalion, victim_team, 0, 0,
        m.vehicles_killed, /*air_source=*/true, killer_squadron);
    for (int i = 0; i < m.vehicles_killed; ++i) {
        ledger_.apply_ag_kill(m.sim_time_s, killer_squadron);
    }
    ++stats_.unit_losses_booked;
    stats_.unit_vehicles_booked += m.vehicles_killed;
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
