// f4-simulation/include/f4/simulation/campaign_result_sink.hpp
//
// CampaignResultSink — the sim-side half of the C1 war loop.
//
//     f4-weapons events on the SIM bus            (EntityKilledMessage,
//      BombImpactMessage — plain structs)            DamageAppliedMessage*)
//       → THIS SINK (f4-simulation)                resolves EntityIds back
//      to campaign identity through the ECS:
//        aircraft → CampaignOriginComponent        (flight/squadron VUs,
//                                                   team slot — stamped at
//                                                   spawn by the bridge)
//        objectives → PropertyBag vu_id_num        (the VU every decoded
//                                                   objective carries)
//       → CampaignResultLedger::apply_*()          (f4-campaign write model)
//
// Why this lives in f4-simulation and not f4-campaign: the ledger's
// boundary rule is absolute — f4-campaign NEVER sees EntityWorld
// components. Entity→identity resolution needs the ECS, so it happens
// here, at the layer that already owns both sides (the spawner's mirror
// image: the spawner turns campaign identity into entities, the sink
// turns entities back into campaign results).
//
// Kill classification (the victim decides the accounting):
//   * victim has CampaignOriginComponent → AIR LOSS: team pool −1,
//     victim squadron books the loss, killer's squadron gets aa credit
//     when IT has an origin (unattributed otherwise — a player entity
//     or synthetic defender kills without credit, exactly as recorded).
//   * victim has NO origin but the killer does → AIR-TO-GROUND kill:
//     ag credit; when the victim is a BATTALION entity the G1 branch
//     also books the victim's own ground loss (air-sourced, one
//     vehicle per entity-kill event — the deagg-vehicle path).
//   * neither side resolvable → unclassified (counted, no ledger effect
//     — the loud-boundary rule; never a silent guess).
//
// G2 — the interdiction booking: GroundUnitLossMessage (one per bomb
// whose blast removed vehicles from a battalion — the AGGREGATE unit
// damage endpoint, see bomb_battery.hpp) books
// apply_ground_loss(air=true, kills) + per-vehicle apply_ag_kill credit
// through the sink's own subscription. This is the OPT-IN arm of the
// tranche (book_unit_losses_, default off — the aa_combat/ground_war
// contract): the blast endpoint itself cannot know the session's flags,
// and saves already carry unit-targeted CAS/BAI flights that drop
// harmless ordnance on battalions — with booking ungated, every pre-G2
// golden that flew one would change. Off: the events count in stats and
// nothing books (documents byte-identical). On: the ledger fills, the
// ground-war engine pulls the loss, the line thins.
//
// Objective damage is FINAL-STATE SYNC, not event-driven: bombs update
// the objective entities' own DamageBitmapComponent/FeatureSetComponent
// during the run (f4-weapons owns that ledger); the sink snapshots each
// objective's damage state at construction and, at sync_objective_damage()
// (end of run), hands every CHANGED objective's final state to the
// ledger. The entity is authoritative; the events are the log.
//
// Threading/ownership: same discipline as the spawner — the bus and the
// world must outlive the sink (or detach() first). Single-threaded
// handler, called from the sim's own tick.
//
// Dependencies: f4-campaign (ledger), f4-entities, f4-messaging,
// f4-weapons (message types + objective_damage_summary), f4-world
// (nothing — the VUs come from PropertyBag residue). C++20.

#pragma once

#include <f4/campaign/result_ledger.hpp>
#include <f4/entities/entity.hpp>
#include <f4/messaging/bus.hpp>
#include <f4/simulation/campaign_origin.hpp>
#include <f4/weapons/messages.hpp>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace f4::simulation {

class CampaignResultSink {
public:
    /// What the sink saw and where it routed it — the QC summary block
    /// and the tests' assertions read exactly these counters.
    struct Stats {
        /// EntityKilledMessage events delivered.
        int kills_seen = 0;
        /// Kills that were air losses (victim had a campaign origin).
        int air_losses_recorded = 0;
        /// Air losses whose killer was attributed to a squadron.
        int kills_attributed = 0;
        /// Kills with an origin-less victim but an origin-ful shooter
        /// (ag credit booked).
        int ag_kills_recorded = 0;
        /// Kills neither side of which resolved (counted, not booked).
        int kills_unclassified = 0;
        /// BombImpactMessage events delivered.
        int bomb_impacts_seen = 0;
        /// Objectives whose damage state changed and synced into the
        /// ledger at the last sync_objective_damage() call.
        int objectives_synced = 0;
        // --- G2: the interdiction counters --------------------------------
        /// GroundUnitLossMessage events delivered (bomb blasts that
        /// removed vehicles — counted whether or not booking is armed).
        int unit_losses_seen = 0;
        /// Unit-loss events actually booked (the unit_strike arm).
        int unit_losses_booked = 0;
        /// Vehicles removed from battalions by air power (booked).
        int unit_vehicles_booked = 0;
    };

    /// Construct over the SIM's world (the world the combat events
    /// mutate). Snapshots every objective's damage state immediately —
    /// construct BEFORE the first tick so the snapshot is the pristine
    /// (save-time) state; a mid-campaign save's prior damage is then
    /// correctly treated as "initial", not "this run's".
    CampaignResultSink(f4::campaign::CampaignResultLedger& ledger,
                       f4::entities::EntityWorld& world);

    /// Subscribe to the combat events on `bus` (returns the first
    /// subscription id). The bus must outlive the sink unless detach()
    /// is called first — the handlers are raw this-captures.
    std::size_t attach(f4::messaging::MessageBus& bus);

    /// Unsubscribe from a previously attached bus. No-op when not
    /// attached. Safe to call repeatedly.
    void detach(f4::messaging::MessageBus& bus);

    /// Process one kill event directly (tests, QC tools without a bus).
    void handle_kill(const f4::weapons::EntityKilledMessage& m);

    /// Process one bomb impact directly (tests, QC tools without a bus).
    void handle_bomb_impact(const f4::weapons::BombImpactMessage& m);

    /// Process one unit-loss event directly (tests, QC tools without a
    /// bus). Books only when the unit_strike arm is set.
    void handle_unit_loss(const f4::weapons::GroundUnitLossMessage& m);

    /// G2 — arm the interdiction booking (the session's unit_strike
    /// flag). Default OFF: events count, nothing books (the golden
    /// identity). Armed: ground losses + per-vehicle ag credit book.
    void set_book_unit_losses(bool on) noexcept { book_unit_losses_ = on; }

    /// End-of-run objective damage sync: walk the world's objectives,
    /// diff each one's damage state against the construction snapshot,
    /// and hand every CHANGED objective's final state to the ledger.
    /// Call after the last tick. Idempotent relative to itself (the
    /// snapshot never moves); repeated calls re-send the same final
    /// states (the ledger's last-write-wins makes that a no-op).
    void sync_objective_damage();

    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

private:
    /// One objective's snapshotted damage state (construction time).
    struct ObjectiveSnapshot {
        std::uint64_t entity = 0;      // EntityId::value
        std::uint32_t vu = 0;          // VU_ID.num
        std::vector<std::uint8_t> fstatus;
        int features_destroyed = 0;
        int destroyed_pct_x100 = 0;    // hundredths of a percent
    };

    /// EntityId → campaign origin, or nullptr when the entity has none
    /// (const-cast pattern: the handle API needs a mutable world ref).
    [[nodiscard]] const CampaignOriginComponent*
    origin_of_(f4::entities::EntityId id);

    void snapshot_objectives_();

    f4::campaign::CampaignResultLedger& ledger_;
    f4::entities::EntityWorld& world_;

    std::vector<ObjectiveSnapshot> objective_snapshots_;
    std::size_t kill_subscription_ = static_cast<std::size_t>(-1);
    std::size_t impact_subscription_ = static_cast<std::size_t>(-1);
    std::size_t unit_loss_subscription_ = static_cast<std::size_t>(-1);
    bool book_unit_losses_ = false;   // G2: the unit_strike arm
    Stats stats_;
};

} // namespace f4::simulation
