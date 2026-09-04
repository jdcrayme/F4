// f4-campaign/src/result_ledger.cpp
//
// CampaignResultLedger implementation — see result_ledger.hpp for the
// tranche rationale and the FreeFalcon correspondence notes.

#include <f4/campaign/result_ledger.hpp>

#include <f4/json/writer.hpp>

#include "squadron_snapshot.hpp"

#include <algorithm>
#include <charconv>
#include <string>
#include <unordered_map>

namespace f4::campaign {

// ============================================================================
// Construction — the snapshot
// ============================================================================

CampaignResultLedger::CampaignResultLedger(
        const f4::world::ICampaignSource& camp,
        const f4::world::ITeamSource& teams,
        const f4::world::IUnitCoreSource& units) {
    // Teams: one entry per team slot, name + the campaign source's
    // per-team aircraft pool (te_number_aircraft — the same seed the
    // Campaign constructor reads; a zero-event ledger and a zero-cycle
    // Campaign therefore agree on every number).
    for (int t = 0; t < teams.team_count() && t < 8; ++t) {
        TeamLedger tl;
        tl.slot = teams.slot(t);
        tl.name = teams.name(t);
        if (tl.slot >= 0 && tl.slot < 8) {
            const auto& pools = camp.te_number_aircraft();
            if (static_cast<std::size_t>(tl.slot) < pools.size()) {
                tl.aircraft_initial = pools[static_cast<std::size_t>(tl.slot)];
            }
        }
        tl.aircraft_remaining = tl.aircraft_initial;
        teams_.push_back(std::move(tl));
    }

    // Squadrons: identity + the save's OWN kill/loss history (a
    // mid-campaign save like TestCamp carries non-zero counters — the
    // ledger builds on the save's numbers, it does not zero them),
    // plus the C2 tasking snapshot: availability and the wire
    // reinforcement budget, through the SAME shared force snapshot the
    // Campaign constructor uses (src/squadron_snapshot.hpp — the
    // numbers agree by construction, not by convention). Wire order
    // (deterministic — the source's unit order).
    struct History {
        std::int16_t aa;
        std::int16_t ag;
        std::uint8_t total_losses;
    };
    std::unordered_map<std::uint32_t, History> history;
    for (int i = 0; i < units.unit_count(); ++i) {
        if (units.unit_class(i) != f4::entities::UnitClass::Squadron) continue;
        const auto* sq = units.as_squadron(i);
        if (sq == nullptr) continue;   // inconsistent source; skip defensively
        history.emplace(units.id_num(i),
                        History{sq->aa_kills(i), sq->ag_kills(i),
                                sq->total_losses(i)});
    }
    const auto force = detail::snapshot_squadron_force(camp, teams, units);
    for (const auto& s : force.squadrons) {
        SquadronLedger sl;
        sl.vu = s.vu;
        sl.owner = s.owner;
        sl.name = s.name;
        const auto it = history.find(s.vu);
        if (it != history.end()) {
            sl.aa_kills = it->second.aa;
            sl.ag_kills = it->second.ag;
            sl.total_losses = it->second.total_losses;
        }
        sl.availability = s.available;
        sl.reinforce_pending = s.reinforce_pending;
        squadrons_.push_back(std::move(sl));
    }
}

// ============================================================================
// Event application
// ============================================================================

SquadronLedger* CampaignResultLedger::find_squadron_(std::uint32_t vu) {
    const auto it = std::find_if(
        squadrons_.begin(), squadrons_.end(),
        [vu](const SquadronLedger& s) { return s.vu == vu; });
    return it == squadrons_.end() ? nullptr : &*it;
}

const SquadronLedger*
CampaignResultLedger::find_squadron_(std::uint32_t vu) const {
    const auto it = std::find_if(
        squadrons_.begin(), squadrons_.end(),
        [vu](const SquadronLedger& s) { return s.vu == vu; });
    return it == squadrons_.end() ? nullptr : &*it;
}

TeamLedger*
CampaignResultLedger::find_team_(int slot) {
    const auto it = std::find_if(
        teams_.begin(), teams_.end(),
        [slot](const TeamLedger& t) { return t.slot == slot; });
    return it == teams_.end() ? nullptr : &*it;
}

void CampaignResultLedger::apply_air_loss(
        double sim_time_s,
        std::uint8_t victim_team,
        std::uint32_t victim_squadron,
        std::uint32_t victim_flight,
        std::uint32_t killer_squadron) {
    AirLossRecord rec;
    rec.sim_time_s = sim_time_s;
    rec.victim_team = victim_team;
    rec.victim_squadron = victim_squadron;
    rec.victim_flight = victim_flight;
    rec.killer_squadron = killer_squadron;

    // Team pool: floor at zero (a save can undercount; the pool is an
    // availability heuristic, never negative).
    if (auto* team = find_team_(victim_team)) {
        if (team->aircraft_remaining > 0) --team->aircraft_remaining;
        ++team->losses;
    }

    // Victim squadron losses: saturate at the wire's uchar limit — the
    // reference's own total_losses is a uchar; overflow is not an option.
    // run_losses is the THIS-RUN delta (the seed's uchar saturation is
    // accounted: a saturated absolute does not keep counting, but the
    // run delta still feeds the availability gate).
    if (victim_squadron != 0) {
        if (auto* sq = find_squadron_(victim_squadron)) {
            if (sq->total_losses < 255) ++sq->total_losses;
            ++sq->run_losses;

            // C2 netting: aircraft the tasking cycle already drew are
            // OUT of the pool — their deaths consume a draw slot
            // instead of debiting the pool again. The existence
            // counters above still count every death.
            if (sq->drawn_deaths < sq->run_draws) {
                ++sq->drawn_deaths;
                if (auto* team = find_team_(sq->owner)) {
                    ++team->drawn_deaths;
                }
            }
        }
    }

    // Killer credit: only when the killer resolved to a known squadron.
    // Int16 saturating, same rule as the wire; run_aa_kills carries the
    // THIS-RUN delta (apply_to writes only deltas — a mid-campaign
    // save's seed history is not "activity").
    rec.attributed = false;
    if (killer_squadron != 0) {
        if (auto* sq = find_squadron_(killer_squadron)) {
            if (sq->aa_kills < 32767) ++sq->aa_kills;
            ++sq->run_aa_kills;
            rec.attributed = true;
            ++air_kills_attributed_;
        }
    }
    if (!rec.attributed) ++air_losses_unattributed_;

    ++air_losses_;
    losses_.push_back(rec);
}

void CampaignResultLedger::apply_mission_draw(
        double t_s, std::uint8_t team, std::uint32_t squadron_vu, int count) {
    if (count <= 0) return;

    MissionDrawRecord rec;
    rec.t_s = t_s;
    rec.team = team;
    rec.squadron = squadron_vu;
    rec.count = count;

    // The squadron's tasking debit. Drawn aircraft still exist — the
    // team's EXISTENCE pool (aircraft_remaining) is untouched; only
    // the tasking view moves (draws are subtracted in
    // squadron_tasking_available()).
    SquadronLedger* sq = find_squadron_(squadron_vu);
    if (sq != nullptr) {
        sq->run_draws += count;
    } else {
        ++draws_unmatched_;   // loud, never a silent drop
    }
    if (auto* tl = find_team_(team)) {
        tl->drawn += count;
    }

    ++mission_draws_;
    mission_draw_aircraft_ += count;
    draws_.push_back(rec);
}

void CampaignResultLedger::apply_mission_recovery(
        double t_s, std::uint8_t team, std::uint32_t squadron_vu,
        std::uint32_t flight_vu, int count) {
    if (count <= 0) return;

    // The release: survivors of a completing flight. Clamped at the
    // squadron's outstanding draws — an over-report (a caller that
    // never saw the draws) books what the pool can actually return,
    // never a negative draw count.
    int released = count;
    SquadronLedger* sq = find_squadron_(squadron_vu);
    if (sq != nullptr) {
        released = std::min(count, sq->run_draws);
        sq->run_draws -= released;
        sq->run_recoveries += released;
    } else {
        released = 0;
        ++recoveries_unmatched_;   // loud, never a silent drop
    }
    if (auto* tl = find_team_(team)) {
        tl->drawn -= released;
        if (tl->drawn < 0) tl->drawn = 0;
        tl->recovered += released;
    }

    MissionRecoveryRecord rec;
    rec.t_s = t_s;
    rec.team = team;
    rec.squadron = squadron_vu;
    rec.flight = flight_vu;
    rec.released = released;

    ++mission_recoveries_;
    aircraft_recovered_ += released;
    recoveries_.push_back(rec);
}

int CampaignResultLedger::flight_air_losses(
        std::uint32_t flight_vu, std::uint32_t squadron_vu) const {
    // The air-loss log IS the per-flight loss record — the sink booked
    // every kill with its victim flight id; counting matching entries
    // is the whole query (arrival order, linear scan: the log is small
    // and this is called once per completing package).
    int n = 0;
    for (const auto& l : losses_) {
        if (l.victim_flight == flight_vu &&
            l.victim_squadron == squadron_vu) {
            ++n;
        }
    }
    return n;
}

int CampaignResultLedger::apply_reinforcements(double t_s) {
    ++reinforcement_fires_;

    int delivered_total = 0;
    for (auto& sq : squadrons_) {
        // Deficit: how far below the snapshot the tasking pool sits.
        // (Non-drawn losses debit the pool; draws do too — a replacement
        // can fill either. Reinforcement never exceeds the snapshot.)
        const int tasking = sq.availability - sq.run_draws
                            - (sq.run_losses - sq.drawn_deaths)
                            + sq.run_reinforced;
        const int deficit = sq.availability - tasking;
        if (deficit <= 0 || sq.reinforce_pending <= 0) continue;

        const int delivered = std::min(deficit, sq.reinforce_pending);
        sq.reinforce_pending -= delivered;
        sq.run_reinforced += delivered;
        delivered_total += delivered;

        // Team existence view gains real aircraft (capped at the team's
        // initial pool; a zero-seed TE block stays zero — TestCamp's
        // shape — the squadron-level tasking numbers carry the story).
        // tl->reinforced tracks the DELIVERED total (the tasking-side
        // view); the existence cap applies only to aircraft_remaining.
        if (auto* tl = find_team_(sq.owner)) {
            tl->reinforced += delivered;
            tl->aircraft_remaining = std::min(
                tl->aircraft_remaining + delivered, tl->aircraft_initial);
        }

        ReinforcementRecord rec;
        rec.t_s = t_s;
        rec.team = sq.owner;
        rec.squadron = sq.vu;
        rec.delivered = delivered;
        rec.budget_left = sq.reinforce_pending;
        reinforcements_.push_back(rec);
    }

    aircraft_reinforced_ += delivered_total;
    return delivered_total;
}

void CampaignResultLedger::apply_ag_kill(double /*sim_time_s*/,
                                         std::uint32_t killer_squadron) {
    if (killer_squadron == 0) return;
    if (auto* sq = find_squadron_(killer_squadron)) {
        if (sq->ag_kills < 32767) ++sq->ag_kills;
        ++sq->run_ag_kills;
        ++ag_kills_;
    }
}

// ============================================================================
// G1 — the ground war's write side
// ============================================================================

GroundUnitLedger* CampaignResultLedger::find_ground_unit_(
        std::uint32_t vu) {
    const auto it = std::find_if(
        ground_units_.begin(), ground_units_.end(),
        [vu](const GroundUnitLedger& g) { return g.vu == vu; });
    return it == ground_units_.end() ? nullptr : &*it;
}

void CampaignResultLedger::apply_ground_loss(
        double t_s,
        std::uint32_t victim_battalion,
        std::uint8_t victim_team,
        std::uint32_t attacker_battalion,
        std::uint8_t attacker_team,
        int kills,
        bool air_source,
        std::uint32_t killer_squadron) {
    if (victim_battalion == 0 || kills <= 0) return;

    GroundLossRecord rec;
    rec.t_s = t_s;
    rec.victim = victim_battalion;
    rec.victim_team = victim_team;
    rec.attacker = attacker_battalion;
    rec.attacker_team = attacker_team;
    rec.killer_squadron = killer_squadron;
    rec.kills = kills;
    rec.air = air_source;
    ground_losses_.push_back(rec);

    // The battalion's run book (lazily created — a loss arriving before
    // the engine's first sync is still booked; the sync then fills the
    // state fields on top).
    GroundUnitLedger* unit = find_ground_unit_(victim_battalion);
    if (unit == nullptr) {
        GroundUnitLedger fresh;
        fresh.vu = victim_battalion;
        fresh.owner = victim_team;
        ground_units_.push_back(fresh);
        unit = &ground_units_.back();
    }
    unit->run_losses += kills;
    unit->strength = std::max(0, unit->strength - kills);

    // The team's ground book (existence counters only — strength views
    // are the engine's).
    if (auto* tl = find_team_(victim_team)) {
        tl->ground_losses += kills;
    } else {
        ++ground_draws_unmatched_;
    }

    ground_vehicle_losses_ += kills;
    if (air_source) ground_vehicle_losses_air_ += kills;
}

void CampaignResultLedger::apply_objective_capture(
        double t_s,
        std::uint32_t objective_vu,
        std::uint8_t from_team,
        std::uint8_t to_team,
        std::uint32_t by_battalion) {
    if (objective_vu == 0 || from_team == to_team) return;

    ObjectiveCaptureRecord rec;
    rec.t_s = t_s;
    rec.objective = objective_vu;
    rec.from_team = from_team;
    rec.to_team = to_team;
    rec.by_battalion = by_battalion;
    captures_.push_back(rec);

    if (auto* tl = find_team_(to_team)) {
        ++tl->objectives_captured;
    } else {
        ++ground_draws_unmatched_;
    }
    ++ground_objectives_captured_;
}

void CampaignResultLedger::sync_ground_unit(
        const GroundUnitLedger& unit) {
    if (unit.vu == 0) return;
    if (auto* existing = find_ground_unit_(unit.vu)) {
        // Last write wins on the STATE fields; run_losses is
        // event-derived and monotone (apply_ground_loss owns it) — a
        // sync never adds to or erases booked kills. Destruction
        // transitions book once (the false→true edge).
        const int booked = existing->run_losses;
        const bool was_destroyed = existing->destroyed;
        const std::uint32_t vu = existing->vu;
        *existing = unit;
        existing->vu = vu;
        existing->run_losses = booked;
        if (!was_destroyed && existing->destroyed) {
            ++ground_battalions_destroyed_;
            if (auto* tl = find_team_(existing->owner)) {
                ++tl->battalions_destroyed;
            }
        }
    } else {
        GroundUnitLedger fresh = unit;
        fresh.run_losses = 0;   // kills book through events only
        ground_units_.push_back(fresh);
        if (fresh.destroyed) {
            ++ground_battalions_destroyed_;
            if (auto* tl = find_team_(fresh.owner)) {
                ++tl->battalions_destroyed;
            }
        }
    }
}

const GroundUnitLedger*
CampaignResultLedger::ground_unit(std::uint32_t vu) const {
    const auto it = std::find_if(
        ground_units_.begin(), ground_units_.end(),
        [vu](const GroundUnitLedger& g) { return g.vu == vu; });
    return it == ground_units_.end() ? nullptr : &*it;
}

void CampaignResultLedger::apply_objective_damage(
        const ObjectiveDamageRecord& rec) {
    if (rec.objective == 0) return;

    // Last write wins: replace an existing entry, else append. The
    // sync sends FINAL states, so a replace is exactly "the world says
    // this now".
    for (auto& existing : objective_damage_) {
        if (existing.objective == rec.objective) {
            features_destroyed_ += rec.features_destroyed
                                   - existing.features_destroyed;
            existing = rec;
            return;
        }
    }
    objective_damage_.push_back(rec);
    objective_vus_.push_back(rec.objective);
    features_destroyed_ += rec.features_destroyed;
}

void CampaignResultLedger::apply_bomb_impact(
        double sim_time_s,
        std::uint32_t objective_vu,
        double miss_distance_ft,
        int features_destroyed) {
    BombImpactRecord rec;
    rec.sim_time_s = sim_time_s;
    rec.objective = objective_vu;
    // Whole feet — the document carries no floats (the determinism
    // discipline; %.1f would be prettier but 1 ft is finer than any
    // CEP anyone will ever QC against this file).
    rec.miss_distance_ft = static_cast<std::int64_t>(miss_distance_ft + 0.5);
    if (rec.miss_distance_ft < 0) rec.miss_distance_ft = 0;
    rec.features_destroyed = features_destroyed;
    impacts_.push_back(rec);
}

// ============================================================================
// Queries
// ============================================================================

int CampaignResultLedger::team_aircraft_remaining(int slot) const {
    const auto it = std::find_if(
        teams_.begin(), teams_.end(),
        [slot](const TeamLedger& t) { return t.slot == slot; });
    return it == teams_.end() ? 0 : it->aircraft_remaining;
}

const SquadronLedger*
CampaignResultLedger::squadron(std::uint32_t vu) const {
    return find_squadron_(vu);
}

int CampaignResultLedger::squadron_run_losses(std::uint32_t vu) const {
    const auto* sq = find_squadron_(vu);
    return sq == nullptr ? 0 : sq->run_losses;
}

int CampaignResultLedger::squadron_tasking_available(
        std::uint32_t vu) const {
    const auto* sq = find_squadron_(vu);
    if (sq == nullptr) return 0;
    // One pool: snapshot − draws − NON-DRAWN losses + reinforcements.
    // A death netted against a draw (drawn_deaths) does not debit —
    // the draw already removed that aircraft. C4: recovery DECREMENTS
    // run_draws, so the released aircraft rejoin the pool through the
    // same subtraction — no extra term. drawn_deaths is clamped to
    // run_losses (a recovery that released a dead-drawn aircraft would
    // otherwise double-credit through a negative loss term).
    int avail = sq->availability - sq->run_draws
                - (sq->run_losses -
                   std::min(sq->run_losses, sq->drawn_deaths))
                + sq->run_reinforced;
    return avail < 0 ? 0 : avail;
}

int CampaignResultLedger::team_aircraft_tasking(int slot) const {
    const auto it = std::find_if(
        teams_.begin(), teams_.end(),
        [slot](const TeamLedger& t) { return t.slot == slot; });
    if (it == teams_.end()) return 0;
    // The team-level view of the same netting: initial − draws −
    // non-drawn losses + reinforcements (existence view adjusted by
    // the tasking-side counters; C4 recovery decrements drawn the same
    // way). Floored at zero; drawn_deaths clamped (same defensive rule
    // as the squadron view).
    int avail = it->aircraft_initial - it->drawn
                - (it->losses -
                   std::min(it->losses, it->drawn_deaths)) +
                it->reinforced;
    return avail < 0 ? 0 : avail;
}

// ============================================================================
// to_json — the result document
// ============================================================================

namespace {

// Fixed-point time: whole milliseconds keeps ordering visible without
// floating point in the document.
std::string time_ms(double sim_time_s) {
    const std::int64_t ms = static_cast<std::int64_t>(sim_time_s * 1000.0 + 0.5);
    return std::to_string(ms);
}

} // namespace

std::string CampaignResultLedger::to_json() const {
    f4::json::Writer w;
    w.put("{\n  \"format\": \"f4-campaign-result\",\n  \"version\": 2");

    // Totals first — the QC gates and the human both read these.
    w.put(",\n  \"totals\": {\n    ");
    w.number_key("air_losses", air_losses_);
    w.put(",\n    ");
    w.number_key("air_kills_attributed", air_kills_attributed_);
    w.put(",\n    ");
    w.number_key("air_losses_unattributed", air_losses_unattributed_);
    w.put(",\n    ");
    w.number_key("ag_kills", ag_kills_);
    w.put(",\n    ");
    w.number_key("bomb_impacts", static_cast<std::int64_t>(impacts_.size()));
    w.put(",\n    ");
    w.number_key("objectives_damaged",
                 static_cast<std::int64_t>(objective_damage_.size()));
    w.put(",\n    ");
    w.number_key("features_destroyed", features_destroyed_);
    w.put(",\n    ");
    // C2 — the tasking side (one pool: draws, losses, resupply).
    w.number_key("mission_draws", mission_draws_);
    w.put(",\n    ");
    w.number_key("mission_draw_aircraft", mission_draw_aircraft_);
    w.put(",\n    ");
    w.number_key("draws_unmatched", draws_unmatched_);
    w.put(",\n    ");
    w.number_key("reinforcement_fires", reinforcement_fires_);
    w.put(",\n    ");
    w.number_key("aircraft_reinforced", aircraft_reinforced_);
    w.put(",\n    ");
    // C4 — mission recovery (drawn aircraft that completed and
    // returned). Emitted ALWAYS (the C2 keys above are): a 0 is the
    // honest "no ATM pipeline / no completions yet" answer, and the
    // QC's recovery gate reads the totals block.
    w.number_key("mission_recoveries", mission_recoveries_);
    w.put(",\n    ");
    w.number_key("aircraft_recovered", aircraft_recovered_);
    w.put("\n  }");

    // Teams: slot order (the snapshot's order), initial + remaining +
    // losses — the existence picture — plus the C2 tasking view
    // (drawn/reinforced/aircraft_tasking) the availability gate reads.
    w.put(",\n  \"teams\": [");
    for (std::size_t i = 0; i < teams_.size(); ++i) {
        const auto& t = teams_[i];
        w.put(i ? ",\n    " : "\n    ");
        w.put("{");
        w.number_key("slot", t.slot);
        w.put(", \"name\": ");
        w.string(t.name);
        w.put(", ");
        w.number_key("aircraft_initial", t.aircraft_initial);
        w.put(", ");
        w.number_key("aircraft_remaining", t.aircraft_remaining);
        w.put(", ");
        w.number_key("air_losses", t.losses);
        w.put(", ");
        w.number_key("aircraft_drawn", t.drawn);
        w.put(", ");
        w.number_key("aircraft_reinforced", t.reinforced);
        w.put(", ");
        w.number_key("aircraft_tasking", team_aircraft_tasking(t.slot));
        w.put("}");
    }
    w.put(teams_.empty() ? "]" : "\n  ]");

    // Squadrons: only the ones with THIS-RUN activity (kills, losses,
    // draws, or reinforcement), VU-sorted for stability regardless of
    // arrival order.
    {
        std::vector<const SquadronLedger*> active;
        for (const auto& s : squadrons_) {
            // THIS-RUN deltas only — a mid-campaign save seeds non-zero
            // absolutes, and the artifact reports what happened THIS
            // run, not the save's own history.
            if (s.run_aa_kills != 0 || s.run_ag_kills != 0 ||
                s.run_losses != 0 || s.run_draws != 0 ||
                s.run_reinforced != 0 || s.run_recoveries != 0) {
                active.push_back(&s);
            }
        }
        std::sort(active.begin(), active.end(),
                  [](const SquadronLedger* a, const SquadronLedger* b) {
                      return a->vu < b->vu;
                  });
        w.put(",\n  \"squadrons\": [");
        for (std::size_t i = 0; i < active.size(); ++i) {
            const auto& s = *active[i];
            w.put(i ? ",\n    " : "\n    ");
            w.put("{");
            w.number_key("vu", s.vu);
            w.put(", \"name\": ");
            w.string(s.name);
            w.put(", ");
            w.number_key("owner", s.owner);
            w.put(", ");
            w.number_key("aa_kills", s.aa_kills);
            w.put(", ");
            w.number_key("ag_kills", s.ag_kills);
            w.put(", ");
            w.number_key("total_losses", s.total_losses);
            w.put(", ");
            // The C2 tasking counters (one pool).
            w.number_key("aircraft_available", s.availability);
            w.put(", ");
            w.number_key("aircraft_tasking",
                         squadron_tasking_available(s.vu));
            w.put(", ");
            w.number_key("run_draws", s.run_draws);
            w.put(", ");
            if (s.run_recoveries != 0) {
                w.number_key("run_recoveries", s.run_recoveries);
                w.put(", ");
            }
            w.number_key("run_reinforced", s.run_reinforced);
            w.put(", ");
            w.number_key("reinforce_budget", s.reinforce_pending);
            w.put("}");
        }
        w.put(active.empty() ? "]" : "\n  ]");
    }

    // Mission draws: arrival order (the tasking ledger's own log).
    w.put(",\n  \"mission_draws\": [");
    for (std::size_t i = 0; i < draws_.size(); ++i) {
        const auto& d = draws_[i];
        w.put(i ? ",\n    " : "\n    ");
        w.put("{\"t_ms\": ");
        w.put(time_ms(d.t_s));
        w.put(", ");
        w.number_key("team", d.team);
        w.put(", ");
        w.number_key("squadron", d.squadron);
        w.put(", ");
        w.number_key("aircraft", d.count);
        w.put("}");
    }
    w.put(draws_.empty() ? "]" : "\n  ]");

    // C4 — mission recoveries: arrival order (the draw's mirror log).
    // Only present when one exists — legacy runs stay byte-identical.
    if (!recoveries_.empty()) {
        w.put(",\n  \"mission_recoveries\": [");
        for (std::size_t i = 0; i < recoveries_.size(); ++i) {
            const auto& rc = recoveries_[i];
            w.put(i ? ",\n    " : "\n    ");
            w.put("{\"t_ms\": ");
            w.put(time_ms(rc.t_s));
            w.put(", ");
            w.number_key("team", rc.team);
            w.put(", ");
            w.number_key("squadron", rc.squadron);
            w.put(", ");
            w.number_key("flight", rc.flight);
            w.put(", ");
            w.number_key("released", rc.released);
            w.put("}");
        }
        w.put("\n  ]");
    }

    // Reinforcement deliveries: arrival order, one record per
    // receiving squadron per fire.
    w.put(",\n  \"reinforcements\": [");
    for (std::size_t i = 0; i < reinforcements_.size(); ++i) {
        const auto& r = reinforcements_[i];
        w.put(i ? ",\n    " : "\n    ");
        w.put("{\"t_ms\": ");
        w.put(time_ms(r.t_s));
        w.put(", ");
        w.number_key("team", r.team);
        w.put(", ");
        w.number_key("squadron", r.squadron);
        w.put(", ");
        w.number_key("delivered", r.delivered);
        w.put(", ");
        w.number_key("budget_left", r.budget_left);
        w.put("}");
    }
    w.put(reinforcements_.empty() ? "]" : "\n  ]");

    // Air-loss events: arrival order (the log).
    w.put(",\n  \"air_losses\": [");
    for (std::size_t i = 0; i < losses_.size(); ++i) {
        const auto& l = losses_[i];
        w.put(i ? ",\n    " : "\n    ");
        w.put("{\"t_ms\": ");
        w.put(time_ms(l.sim_time_s));
        w.put(", ");
        w.number_key("victim_team", l.victim_team);
        w.put(", ");
        w.number_key("victim_squadron", l.victim_squadron);
        w.put(", ");
        w.number_key("victim_flight", l.victim_flight);
        w.put(", ");
        w.number_key("killer_squadron", l.killer_squadron);
        w.put(l.attributed ? ", \"attributed\": true"
                           : ", \"attributed\": false");
        w.put("}");
    }
    w.put(losses_.empty() ? "]" : "\n  ]");

    // Bomb impacts: arrival order.
    w.put(",\n  \"bomb_impacts\": [");
    for (std::size_t i = 0; i < impacts_.size(); ++i) {
        const auto& im = impacts_[i];
        w.put(i ? ",\n    " : "\n    ");
        w.put("{\"t_ms\": ");
        w.put(time_ms(im.sim_time_s));
        w.put(", ");
        w.number_key("objective", im.objective);
        w.put(", ");
        w.number_key("miss_ft", im.miss_distance_ft);
        w.put(", ");
        w.number_key("features_destroyed", im.features_destroyed);
        w.put("}");
    }
    w.put(impacts_.empty() ? "]" : "\n  ]");

    // Objective damage: VU-sorted final states, the fstatus bitmap in
    // the wire's own packing (2 bits per feature, hex bytes).
    {
        std::vector<const ObjectiveDamageRecord*> sorted;
        sorted.reserve(objective_damage_.size());
        for (const auto& rec : objective_damage_) sorted.push_back(&rec);
        std::sort(sorted.begin(), sorted.end(),
                  [](const ObjectiveDamageRecord* a,
                     const ObjectiveDamageRecord* b) {
                      return a->objective < b->objective;
                  });
        w.put(",\n  \"objectives\": [");
        for (std::size_t i = 0; i < sorted.size(); ++i) {
            const auto& o = *sorted[i];
            w.put(i ? ",\n    " : "\n    ");
            w.put("{");
            w.number_key("vu", o.objective);
            w.put(", ");
            w.number_key("features_total", o.features_total);
            w.put(", ");
            w.number_key("features_destroyed", o.features_destroyed);
            w.put(", ");
            w.number_key("destroyed_pct", o.destroyed_pct);
            w.put(", \"fstatus\": \"");
            static const char kHex[] = "0123456789abcdef";
            for (const auto b : o.fstatus) {
                w.put(kHex[(b >> 4) & 0x0F]);
                w.put(kHex[b & 0x0F]);
            }
            w.put("\"}");
        }
        w.put(sorted.empty() ? "]" : "\n  ]");
    }

    // G1 — the ground war block. OPTIONAL (the same discipline as
    // mission_recoveries): a run with no ground activity emits the
    // byte-identical pre-G1 document; a ground war emits its own books
    // here (totals, per-team ground rows, VU-sorted battalion final
    // states, arrival-ordered loss and capture events). No floats:
    // integer grid positions, whole-byte states, ms times.
    if (!ground_losses_.empty() || !captures_.empty() ||
        !ground_units_.empty()) {
        w.put(",\n  \"ground\": {");
        w.put("\n    ");
        w.number_key("vehicle_losses", ground_vehicle_losses_);
        w.put(",\n    ");
        w.number_key("battalions_destroyed",
                     ground_battalions_destroyed_);
        w.put(",\n    ");
        w.number_key("objectives_captured", ground_objectives_captured_);

        // Per-team ground rows: only teams with ground activity, slot
        // order (the snapshot's own order).
        {
            bool has_rows = false;
            for (const auto& t : teams_) {
                if (t.ground_losses != 0 || t.battalions_destroyed != 0 ||
                    t.objectives_captured != 0) {
                    has_rows = true;
                    break;
                }
            }
            if (has_rows) {
                w.put(",\n    \"teams\": [");
                bool first = true;
                for (const auto& t : teams_) {
                    if (t.ground_losses == 0 && t.battalions_destroyed == 0 &&
                        t.objectives_captured == 0) {
                        continue;
                    }
                    w.put(first ? "\n      {" : ",\n      {");
                    first = false;
                    w.number_key("slot", t.slot);
                    w.put(", \"name\": ");
                    w.string(t.name);
                    w.put(", ");
                    w.number_key("vehicle_losses", t.ground_losses);
                    w.put(", ");
                    w.number_key("battalions_destroyed",
                                 t.battalions_destroyed);
                    w.put(", ");
                    w.number_key("objectives_captured",
                                 t.objectives_captured);
                    w.put("}");
                }
                w.put("\n    ]");
            }
        }

        // Battalions: VU-sorted final states (movement, attrition, and
        // the destroyed flag — the ground war's existence picture).
        {
            std::vector<const GroundUnitLedger*> sorted;
            sorted.reserve(ground_units_.size());
            for (const auto& g : ground_units_) sorted.push_back(&g);
            std::sort(sorted.begin(), sorted.end(),
                      [](const GroundUnitLedger* a,
                         const GroundUnitLedger* b) {
                          return a->vu < b->vu;
                      });
            w.put(",\n    \"units\": [");
            for (std::size_t i = 0; i < sorted.size(); ++i) {
                const auto& g = *sorted[i];
                w.put(i ? ",\n      {" : "\n      {");
                w.number_key("vu", g.vu);
                w.put(", ");
                w.number_key("owner", g.owner);
                w.put(", ");
                w.number_key("strength_initial", g.strength_initial);
                w.put(", ");
                w.number_key("strength", g.strength);
                w.put(", ");
                w.number_key("run_losses", g.run_losses);
                w.put(", ");
                w.number_key("x", g.x);
                w.put(", ");
                w.number_key("y", g.y);
                w.put(", ");
                w.number_key("supply", g.supply);
                w.put(", ");
                w.number_key("morale", g.morale);
                w.put(", ");
                w.number_key("fatigue", g.fatigue);
                w.put(g.destroyed ? ", \"destroyed\": true"
                                  : ", \"destroyed\": false");
                w.put("}");
            }
            w.put(sorted.empty() ? "]" : "\n    ]");
        }

        // Ground loss events: arrival order (the log).
        w.put(",\n    \"losses\": [");
        for (std::size_t i = 0; i < ground_losses_.size(); ++i) {
            const auto& l = ground_losses_[i];
            w.put(i ? ",\n      " : "\n      ");
            w.put("{\"t_ms\": ");
            w.put(time_ms(l.t_s));
            w.put(", ");
            w.number_key("victim", l.victim);
            w.put(", ");
            w.number_key("victim_team", l.victim_team);
            w.put(", ");
            w.number_key("attacker", l.attacker);
            if (l.attacker_team != 0) {
                w.put(", ");
                w.number_key("attacker_team", l.attacker_team);
            }
            if (l.killer_squadron != 0) {
                w.put(", ");
                w.number_key("killer_squadron", l.killer_squadron);
            }
            w.put(", ");
            w.number_key("kills", l.kills);
            w.put(l.air ? ", \"air\": true" : ", \"air\": false");
            w.put("}");
        }
        w.put(ground_losses_.empty() ? "]" : "\n    ]");

        // Captures: arrival order (the territorial log).
        w.put(",\n    \"captures\": [");
        for (std::size_t i = 0; i < captures_.size(); ++i) {
            const auto& c = captures_[i];
            w.put(i ? ",\n      " : "\n      ");
            w.put("{\"t_ms\": ");
            w.put(time_ms(c.t_s));
            w.put(", ");
            w.number_key("objective", c.objective);
            w.put(", ");
            w.number_key("from_team", c.from_team);
            w.put(", ");
            w.number_key("to_team", c.to_team);
            w.put(", ");
            w.number_key("by_battalion", c.by_battalion);
            w.put("}");
        }
        w.put(captures_.empty() ? "]" : "\n    ]");

        w.put("\n  }");
    }

    w.put("\n}\n");
    return w.str();
}

} // namespace f4::campaign
