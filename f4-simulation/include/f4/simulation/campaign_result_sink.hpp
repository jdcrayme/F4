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
// objective's damage state at construction and, at each sync, hands
// every CHANGED objective's state to the ledger. The entity is
// authoritative; the events are the log.
//
// AGG-2a — the sync has two forms (Docs/AGGREGATE_CLOCK_PLAN.md §4):
//   * the FULL walk (sync_objective_damage) — every snapshotted
//     objective, end of run (the QC/writeback callers). O(objectives).
//   * the DIRTY walk (sync_dirty_objective_damage) — only the
//     objectives a TRANSITION marked since the last sync (bomb impacts
//     resolve their target here; the session's repair mirror marks
//     explicitly). O(changes). The per-objective diff/book logic is
//     shared, and the dirty set iterates in snapshot (wire) order, so
//     the ledger byte-stream for the same transitions is identical to
//     the full walk's — the records move, never their shape or order.
//
// Threading/ownership: same discipline as the spawner — the bus and the
// world must outlive the sink (or detach() first). Single-threaded
// handler, called from the sim's own tick.
//
// Dependencies: f4-campaign (ledger), f4-entities, f4-messaging,
// f4-weapons (message types + objective_damage_summary), f4-world
// (nothing — the VUs come from PropertyBag residue). C++20.

#pragma once

#include <f4/campaign/api/events.hpp>
#include <f4/campaign/result_ledger.hpp>
#include <f4/entities/entity.hpp>
#include <f4/messaging/bus.hpp>
#include <f4/simulation/campaign_origin.hpp>
#include <f4/weapons/messages.hpp>

#include <cstdint>
#include <cstddef>
#include <set>
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

    /// Full objective damage sync: walk EVERY snapshotted objective,
    /// diff each one's damage state against its snapshot, and hand
    /// every CHANGED objective's state to the ledger. O(objectives).
    /// The end-of-run form (QC, writeback, tests) and the safety net —
    /// the per-pass callers use the dirty form below. Also consumes
    /// (clears) any pending dirt: everything is synced after the walk.
    /// Idempotent relative to itself: the snapshot advances to what was
    /// just reported, so a repeated call re-sends nothing (a repeat
    /// re-sends a state only if the world moved again between calls).
    void sync_objective_damage();

    // --- AGG-2a: the transition-triggered sync ---------------------------

    /// Mark one objective dirty, by ENTITY id (the id the impact event
    /// carries). Unknown entities are a no-op — only objectives in the
    /// construction snapshot can sync. Deterministic: the mark is a
    /// set insert; the sync walks the marks in wire order.
    void mark_objective_dirty(std::uint64_t entity_id);

    /// The per-pass form (AGG-2a): sync ONLY the objectives a
    /// transition marked since the last sync — O(changes), never
    /// O(objectives). Same per-objective diff/book logic as the full
    /// walk (the shared row body); the dirty set iterates ASCENDING by
    /// snapshot index, which IS wire order, so the changed subset's
    /// records land in exactly the order the full walk would have
    /// booked them. Consumes the dirt (clears the set).
    void sync_dirty_objective_damage();

    /// Objectives waiting for the dirty sync (probes/QC).
    [[nodiscard]] std::size_t dirty_objectives() const noexcept {
        return dirty_objectives_.size();
    }

    // --- CAMP-HOST-2: the event stream ----------------------------------

    /// One objective the last sync (either form) found CHANGED (the
    /// kill event publishes inline; damage needs the SESSION's owner
    /// view, so the sync COLLECTS here and the session publishes right
    /// after the call — detection and publication stay adjacent).
    struct DamageSync {
        std::uint32_t vu = 0;
        int features_damaged = 0;   ///< features at damage state 1+
    };

    /// What the last sync_objective_damage() collected (the session
    /// drains it right after the call; the next sync clears first).
    [[nodiscard]] const std::vector<DamageSync>& damage_synced()
        const noexcept {
        return damage_synced_;
    }

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

    /// The shared per-objective body (full walk and dirty walk): read
    /// the entity face, diff against `snap`, book + collect on change,
    /// advance the snapshot. Returns whether the objective changed.
    bool sync_objective_row_(ObjectiveSnapshot& snap);

    f4::campaign::CampaignResultLedger& ledger_;
    f4::entities::EntityWorld& world_;

    std::vector<ObjectiveSnapshot> objective_snapshots_;
    /// Entity id → the snapshot row it owns (built once, at
    /// construction — the snapshot set never grows mid-run).
    std::unordered_map<std::uint64_t, std::size_t> snapshot_index_;
    /// AGG-2a — the transitions' dirty set: snapshot indices, kept
    /// ASCENDING so the dirty walk's order == the full walk's wire
    /// order over the same subset (the ledger byte-stream contract).
    std::set<std::size_t> dirty_objectives_;
    std::size_t kill_subscription_ = static_cast<std::size_t>(-1);
    std::size_t impact_subscription_ = static_cast<std::size_t>(-1);
    std::size_t unit_loss_subscription_ = static_cast<std::size_t>(-1);
    bool book_unit_losses_ = false;   // G2: the unit_strike arm
    Stats stats_;

    // HOST-2: the bus the kill event publishes to (bound by attach();
    // null when the sink is driven bus-less — tests, QC tools — and
    // then nothing publishes, exactly like HOST-1).
    f4::messaging::MessageBus* bus_ = nullptr;
    std::vector<DamageSync> damage_synced_;
};

} // namespace f4::simulation
