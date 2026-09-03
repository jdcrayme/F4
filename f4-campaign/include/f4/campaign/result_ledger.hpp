// f4-campaign/include/f4/campaign/result_ledger.hpp
//
// CampaignResultLedger — the campaign's WRITE model (Phase C1, the
// "close the war loop" tranche).
//
// Up to C1 every campaign-side number was read-only: the Campaign bound
// the f4-world IDataSource interfaces, ticked its tasking cycles, and
// published MissionIntents — but nothing ever flowed BACK. Sim outcomes
// (kills, bomb impacts, objective damage) landed on the MessageBus and
// died there; the team aircraft pools, squadron kill/loss counters, and
// objective damage bitmaps stayed frozen at their save-time values. That
// one-way shape is a mission replay engine, not a campaign.
//
// This ledger is the write side. It snapshots the same read-only sources
// the Campaign binds (per-team aircraft pools, the squadron roster), then
// applies TYPED result events produced by the sim:
//
//     f4-weapons EntityKilledMessage / BombImpactMessage  (the sim bus)
//       → CampaignResultSink                [f4-simulation — resolves
//                                            entity ids back to campaign
//                                            identity through the ECS]
//       → CampaignResultLedger::apply_*()   [THIS — pure state transitions]
//
// FreeFalcon correspondence (the behavior this reproduces):
//   * An air kill decrements the VICTIM team's aircraft pool and bumps
//     the victim squadron's total_losses (a saturating uchar on the wire
//     — squadron.cpp's own field), and credits the KILLER's squadron
//     aa_kills (int16). Kills by non-campaign shooters (a player entity,
//     a synthetic defender) are UNATTRIBUTED: the loss applies, no
//     squadron gets credit — the same shape as a Flight dying without
//     an attributable killer in the reference.
//   * Objective feature damage is FINAL-STATE SYNC: the sink walks the
//     sim world's objectives and hands the ledger each damaged
//     objective's full 2-bit-per-feature fstatus bitmap + destroyed
//     counts (the .obd "save-format face" of the damage). Last write
//     wins per objective — the entity's own DamageBitmapComponent is
//     authoritative, the ledger just carries it back.
//   * Bomb impacts are also logged as events (miss distance, TOF) for
//     the debrief artifact — counters only, no state transition of
//     their own (the fstatus sync above is the state).
//
// DISCIPLINE (unchanged from the rest of f4-campaign):
//   * NO EntityWorld, NO f4-world-convert, NO weapons types here. The
//     ledger sees campaign identity (team slots, squadron VU_IDs,
//     objective VU_IDs) and plain numbers. The entity→identity mapping
//     is the sink's job (f4-simulation), because it needs the ECS.
//   * NO RNG, NO clocks of its own. Events carry the sim time they
//     happened at; ordering is arrival order, which is bus order,
//     which is deterministic in this engine.
//   * Byte-stable to_json(): two ledgers fed the same event sequence
//     produce byte-identical documents (pinned by the golden test, the
//     same way Campaign::to_summary_json() is).
//   * Saturating wire ranges: total_losses clamps at 255 (uchar),
//     aa/ag kills clamp at 32767 (int16) — the reference's own limits,
//     not silent overflow.
//
// The C2 hooks read this ledger: Campaign::set_result_ledger() makes
// the ledger THE tasking pool — mission draws debit it
// (apply_mission_draw), combat losses net against the draws, and the
// reinforcement cadence refills it (apply_reinforcements) — one pool
// for cycles, combat, and resupply, the write-back's single source of
// truth. apply_to(WorldState) writes the results back into the typed
// world state (see world_writeback.hpp — the opt-in header that
// carries the WorldState dependency).
//
// Dependencies: f4-world (IDataSource), f4-json (to_json writer, PRIVATE
// link), stdlib. C++20.

#pragma once

#include <f4/campaign/mission_type.hpp>
#include <f4/world/data_source.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace f4::campaign {

/// One air-loss event as the ledger records it. Arrival order; the JSON
/// artifact and the tests both assert that order.
struct AirLossRecord {
    /// Sim time of the kill (seconds; from EntityKilledMessage).
    double sim_time_s = 0.0;
    /// Victim's campaign team slot.
    std::uint8_t victim_team = 0;
    /// Victim's squadron VU_ID.num (0 when the victim had no campaign
    /// origin — the loss still hits the team pool, no squadron books it).
    std::uint32_t victim_squadron = 0;
    /// Victim's flight VU_ID.num (0 when unknown).
    std::uint32_t victim_flight = 0;
    /// Killer's squadron VU_ID.num — 0 = unattributed.
    std::uint32_t killer_squadron = 0;
    /// True when the killer resolved to a campaign squadron (credit due).
    bool attributed = false;
};

/// One objective damage sync event (final state, from the end-of-run
/// world walk — see CampaignResultSink::sync_objective_damage()).
struct ObjectiveDamageRecord {
    /// Objective VU_ID.num.
    std::uint32_t objective = 0;
    /// Total feature slots on the objective (FeatureSetComponent size).
    int features_total = 0;
    /// Features currently in VIS state 3 (destroyed).
    int features_destroyed = 0;
    /// 100 * destroyed-value / total-value (integer percent, 0..100).
    int destroyed_pct = 0;
    /// The full 2-bit-per-feature bitmap — the .obd fstatus face.
    std::vector<std::uint8_t> fstatus;
};

/// One bomb impact event (debrief log; counters, not state).
struct BombImpactRecord {
    double sim_time_s = 0.0;
    std::uint32_t objective = 0;      // 0 = impact resolved no objective
    std::int64_t miss_distance_ft = 0; // rounded (the no-floats rule)
    int features_destroyed = 0;       // per the impact message
};

/// Per-team write state. Seeded from the campaign source at
/// construction. C2 adds the tasking-side counters: `drawn` (aircraft
/// committed to missions this run) and `reinforced` (aircraft
/// delivered by the reinforcement cadence). aircraft_remaining stays
/// the EXISTENCE view (drawn aircraft still exist — only deaths and
/// reinforcements move it).
struct TeamLedger {
    int slot = 0;
    std::string name;               // empty when the slot is unnamed
    int aircraft_initial = 0;       // te_number_aircraft at snapshot
    int aircraft_remaining = 0;     // post-loss (floored at 0)
    int losses = 0;                 // air losses booked this run
    int drawn = 0;                  // aircraft drawn into missions (C2)
    int reinforced = 0;             // aircraft delivered by resupply (C2)
    /// Deaths already netted against draws (internal; the tasking view
    /// subtracts them from `losses` — those aircraft left the pool when
    /// they were drawn).
    int drawn_deaths = 0;
};

/// Per-squadron write state. Seeded from the source at construction,
/// then only the apply_* methods move the counters.
struct SquadronLedger {
    std::uint32_t vu = 0;             // VU_ID.num (the campaign key)
    std::uint8_t owner = 0;           // team slot
    std::string name;                 // class_name (display)
    std::int16_t aa_kills = 0;        // air-to-air credits (absolute)
    std::int16_t ag_kills = 0;        // air-to-ground credits (absolute)
    std::uint16_t total_losses = 0;   // aircraft lost (absolute, uchar wire)
    /// THIS-RUN deltas (absolute − seed) — what apply_to() writes and
    /// the Campaign's availability gate subtracts (C2 hooks). A
    /// mid-campaign save seeds NON-ZERO absolutes; "activity" means a
    /// non-zero DELTA, never a non-zero absolute.
    int run_losses = 0;
    int run_aa_kills = 0;
    int run_ag_kills = 0;
    // --- C2: the tasking pool (one pool for draws, losses, resupply) --
    /// Snapshot availability — the SAME rule Campaign::Campaign uses
    /// (roster decoded from the 2-bit group packing, else the shared
    /// team-pool share; see src/squadron_snapshot.hpp). The numbers
    /// agree with the Campaign's by construction.
    int availability = 0;
    /// The wire's reinforcement budget (aircraft on order — the unit
    /// record's own `reinforcement` i16). Consumed by
    /// apply_reinforcements; never replenished in this slice.
    int reinforce_pending = 0;
    /// Aircraft drawn into missions THIS RUN (tasking debit).
    int run_draws = 0;
    /// Aircraft delivered by reinforcement ticks THIS RUN (tasking
    /// credit, capped at `availability`).
    int run_reinforced = 0;
    /// Deaths netted against draws (internal netting counter — a drawn
    /// aircraft's death consumes the draw, it does not debit the pool
    /// twice; the existence counters still count it).
    int drawn_deaths = 0;
};

/// One mission-draw event (the tasking side of the ledger — a package
/// committed `count` aircraft). Arrival order; campaign-time seconds
/// (the Campaign's relative clock, not the sim's).
struct MissionDrawRecord {
    double t_s = 0.0;
    std::uint8_t team = 0;
    std::uint32_t squadron = 0;    // 0 = unresolvable (counted, loud)
    int count = 0;
};

/// One reinforcement delivery (a squadron received aircraft on the
/// resupply cadence). One record per receiving squadron per tick.
struct ReinforcementRecord {
    double t_s = 0.0;
    std::uint8_t team = 0;
    std::uint32_t squadron = 0;
    int delivered = 0;
    int budget_left = 0;            // reinforce_pending after delivery
};

class CampaignResultLedger {
public:
    /// Snapshot the initial state: per-team aircraft pools (the campaign
    /// source's te_number_aircraft — the SAME seed Campaign::Campaign
    /// reads, so a zero-event ledger agrees with a zero-cycle Campaign)
    /// and the squadron roster (identity + current kill/loss counters —
    /// a mid-campaign save like TestCamp starts non-zero, and the ledger
    /// must build on the save's own history, not zero it).
    ///
    /// All references are borrowed for the constructor call only (the
    /// snapshot is taken immediately; the ledger owns its state).
    CampaignResultLedger(const f4::world::ICampaignSource& camp,
                         const f4::world::ITeamSource& teams,
                         const f4::world::IUnitCoreSource& units);

    // ------------------------------------------------------------------
    // Event application (typed; the f4-simulation sink calls these)
    // ------------------------------------------------------------------

    /// An aircraft died. Team pool −1 (floor 0), victim squadron
    /// total_losses +1 (saturating), killer squadron aa_kills +1 when
    /// killer_squadron resolves (saturating). Victim identity is the
    /// SINK's resolution; passing victim_squadron 0 books a team-only
    /// loss (the unresolvable-origin case — counted, visible, honest).
    ///
    /// C2 netting: when the victim's squadron has aircraft outstanding
    /// on missions (run draws not yet killed), the death consumes a
    /// draw slot — the draw ALREADY removed that aircraft from the
    /// tasking pool, so killing it must not debit the pool twice.
    /// Deaths with no outstanding draws (a parked or scenario aircraft)
    /// debit the tasking pool directly. The existence counters (team
    /// pool, total_losses) count EVERY death either way.
    void apply_air_loss(double sim_time_s,
                        std::uint8_t victim_team,
                        std::uint32_t victim_squadron,
                        std::uint32_t victim_flight,
                        std::uint32_t killer_squadron);

    /// A mission drew `count` aircraft from the squadron — the tasking
    /// debit (C2: cycles and combat deplete ONE pool). Drawn aircraft
    /// still EXIST (they fly), so the team's existence pool and the
    /// write-back are untouched; only tasking availability drops. An
    /// unknown squadron VU still books the team draw and is counted
    /// unmatched (draws_unmatched — loud, never silent).
    void apply_mission_draw(double t_s,
                            std::uint8_t team,
                            std::uint32_t squadron_vu,
                            int count);

    /// The reinforcement tick: every squadron whose tasking availability
    /// dropped below its snapshot refills toward the snapshot, drawing
    /// on its wire reinforcement budget (min(deficit, budget)); the
    /// budget is consumed. Team existence pools gain the deliveries
    /// (capped at aircraft_initial). Returns total aircraft delivered
    /// (0 when nobody has a deficit or a budget — a legal, quiet fire).
    int apply_reinforcements(double t_s);

    /// A ground victim died at a campaign shooter's hands (air-to-ground
    /// credit only — ag_kills on the killer's squadron; no team pool
    /// effect: ground strength is battalion rosters, the ground-war
    /// tranche's ledger, not this one).
    void apply_ag_kill(double sim_time_s,
                       std::uint32_t killer_squadron);

    /// Final-state objective damage sync (last write wins per
    /// objective). Also updates the objective's destroyed counters.
    void apply_objective_damage(const ObjectiveDamageRecord& rec);

    /// Bomb impact log entry (counters only).
    void apply_bomb_impact(double sim_time_s,
                           std::uint32_t objective_vu,
                           double miss_distance_ft,
                           int features_destroyed);

    // ------------------------------------------------------------------
    // Queries (the C2 tasking hooks + the QC gates read these)
    // ------------------------------------------------------------------

    /// Post-loss aircraft pool for a team slot (the number
    /// Campaign::set_result_ledger() feeds the availability gate).
    /// Unknown slots report the seeded value (0 for never-seen slots).
    /// EXISTENCE view: deaths and reinforcements move it, draws do not.
    [[nodiscard]] int team_aircraft_remaining(int slot) const;

    /// Tasking view for a team slot: existence − draws outstanding −
    /// non-drawn losses (the pool the next tasking cycle can draw
    /// from). Same netting as squadron_tasking_available().
    [[nodiscard]] int team_aircraft_tasking(int slot) const;

    /// The squadron ledger entry by VU_ID.num (nullptr when unknown).
    [[nodiscard]] const SquadronLedger* squadron(std::uint32_t vu) const;

    /// Air losses booked THIS RUN for one squadron (0 when unknown) —
    /// the delta Campaign's availability gate subtracts (C2 hook).
    [[nodiscard]] int squadron_run_losses(std::uint32_t vu) const;

    /// TASKING AVAILABILITY for one squadron (the C2 gate the Campaign
    /// reads while a ledger is attached): snapshot availability −
    /// mission draws − non-drawn losses + reinforcements. A fresh
    /// ledger reports the snapshot itself (identical to the un-attached
    /// Campaign — the golden identity). Unknown VU → 0.
    [[nodiscard]] int squadron_tasking_available(std::uint32_t vu) const;

    /// Total air losses applied (all teams).
    [[nodiscard]] int air_losses() const noexcept { return air_losses_; }

    /// Total attributed air kills (killer credit actually booked).
    [[nodiscard]] int air_kills_attributed() const noexcept {
        return air_kills_attributed_;
    }

    /// Total unattributed air losses (killer unknown / non-campaign).
    [[nodiscard]] int air_losses_unattributed() const noexcept {
        return air_losses_unattributed_;
    }

    /// Objectives with damage state recorded (sync order).
    [[nodiscard]] const std::vector<ObjectiveDamageRecord>&
    objective_damage() const noexcept {
        return objective_damage_;
    }

    /// Total features destroyed across synced objectives.
    [[nodiscard]] int features_destroyed() const noexcept {
        return features_destroyed_;
    }

    /// Bomb impacts logged (resolved + unresolved objectives alike).
    [[nodiscard]] int bomb_impacts() const noexcept {
        return static_cast<int>(impacts_.size());
    }

    /// Mission draws booked this run, all teams (the C2 tasking side).
    [[nodiscard]] int mission_draws() const noexcept {
        return mission_draws_;
    }

    /// Aircraft drawn into missions this run, all teams.
    [[nodiscard]] int mission_draw_aircraft() const noexcept {
        return mission_draw_aircraft_;
    }

    /// Reinforcement ticks applied (fires of the cadence, delivered or
    /// not — a fire that finds no deficit is still a fire).
    [[nodiscard]] int reinforcement_fires() const noexcept {
        return reinforcement_fires_;
    }

    /// Aircraft delivered by reinforcement ticks this run, all teams.
    [[nodiscard]] int aircraft_reinforced() const noexcept {
        return aircraft_reinforced_;
    }

    /// Mission-draw events booked against squadrons the ledger never
    /// saw (the loud counter — a stale world or a foreign tasking
    /// source; never a silent drop).
    [[nodiscard]] int draws_unmatched() const noexcept {
        return draws_unmatched_;
    }

    /// The draw event log (arrival order).
    [[nodiscard]] const std::vector<MissionDrawRecord>&
    mission_draw_log() const noexcept {
        return draws_;
    }

    /// The reinforcement delivery log (arrival order — one record per
    /// receiving squadron per fire).
    [[nodiscard]] const std::vector<ReinforcementRecord>&
    reinforcement_log() const noexcept {
        return reinforcements_;
    }

    /// The impact event log (arrival order).
    [[nodiscard]] const std::vector<BombImpactRecord>&
    bomb_impact_log() const noexcept {
        return impacts_;
    }

    /// The air-loss event log (arrival order).
    [[nodiscard]] const std::vector<AirLossRecord>&
    air_loss_log() const noexcept {
        return losses_;
    }

    /// Full team view, slot order (the write-back and the viewer-side
    /// QC read this; the counters API above is the common path).
    [[nodiscard]] const std::vector<TeamLedger>& teams() const noexcept {
        return teams_;
    }

    /// Full squadron view, wire order.
    [[nodiscard]] const std::vector<SquadronLedger>&
    squadrons() const noexcept {
        return squadrons_;
    }

    /// True when NOTHING was applied — the QC gate's "results didn't
    /// write back" condition (combat happened, the ledger stayed
    /// empty). Draws and reinforcements are TASKING state, not combat
    /// results — a run that only tasked stays "empty" here (its
    /// write-back is correctly a no-op: drawn aircraft still exist).
    [[nodiscard]] bool empty() const noexcept {
        return losses_.empty() && impacts_.empty() &&
               objective_damage_.empty() && ag_kills_ == 0;
    }

    // ------------------------------------------------------------------
    // Artifacts
    // ------------------------------------------------------------------

    /// The result document — "f4-campaign-result" v2 (C1 was v1; C2
    /// adds the tasking side: mission_draws, reinforcements, and the
    /// per-team/per-squadron tasking counters). Deterministic: fixed
    /// key order, slot-ordered teams, VU-sorted squadrons,
    /// arrival-ordered events, NO floats (ms times, integer percents,
    /// hex fstatus). Byte-stable across identical event sequences
    /// (golden-tested).
    [[nodiscard]] std::string to_json() const;

private:
    [[nodiscard]] SquadronLedger* find_squadron_(std::uint32_t vu);
    [[nodiscard]] const SquadronLedger*
    find_squadron_(std::uint32_t vu) const;
    [[nodiscard]] TeamLedger* find_team_(int slot);

    std::vector<TeamLedger> teams_;
    std::vector<SquadronLedger> squadrons_;  // wire order (deterministic)
    std::vector<AirLossRecord> losses_;
    std::vector<BombImpactRecord> impacts_;
    std::vector<MissionDrawRecord> draws_;
    std::vector<ReinforcementRecord> reinforcements_;
    /// Sync order with VU keys — to_json() sorts for output stability.
    std::vector<ObjectiveDamageRecord> objective_damage_;
    /// Duplicated VU set for last-write-wins lookups.
    std::vector<std::uint32_t> objective_vus_;

    int air_losses_ = 0;
    int air_kills_attributed_ = 0;
    int air_losses_unattributed_ = 0;
    int ag_kills_ = 0;
    int features_destroyed_ = 0;
    // --- C2 tasking-side totals ---
    int mission_draws_ = 0;          // draw EVENTS (packages)
    int mission_draw_aircraft_ = 0;  // aircraft committed
    int draws_unmatched_ = 0;        // draws against unknown squadrons
    int reinforcement_fires_ = 0;    // cadence fires (delivered or not)
    int aircraft_reinforced_ = 0;    // aircraft delivered
};

} // namespace f4::campaign
