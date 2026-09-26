// f4-campaign/src/result_ledger.cpp
//
// CampaignResultLedger implementation — see result_ledger.hpp for the
// tranche rationale and the FreeFalcon correspondence notes.

#include <f4/campaign/result_ledger.hpp>

#include <f4/json/writer.hpp>
#include <f4/json/scope.hpp>

#include "squadron_snapshot.hpp"

#include <algorithm>
#include <charconv>
#include <string>
#include <unordered_map>

namespace f4::campaign {

using f4::json::Array;
using f4::json::Members;

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
        // DOM-2: the strategic reserve's seed (decoded since C2,
        // consumed since DOM-2 — apply_reinforcements' stock flow).
        tl.replacements_initial = static_cast<int>(teams.replacements_avail(t));
        tl.replacements_avail = tl.replacements_initial;
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
        sl.reinforce_initial = s.reinforce_pending;
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

// DOM-3 — the personnel book's per-slot row lookup (first-touch order;
// the roster is 48 slots so the linear scan is trivially cheap).
PilotDelta* find_pilot_delta_(std::vector<PilotDelta>& book,
                              std::uint8_t slot) {
    for (auto& d : book) {
        if (d.slot == slot) return &d;
    }
    return nullptr;
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

    // DOM-3 — the crew: a crewed flight's loss consumes a roster slot
    // (the deterministic subset: losses eat the crew IN PICK ORDER —
    // the lead's slot first; the reference tracks per-aircraft pilots
    // in the flight tail our aggregate does not carry). The consumed
    // slot dies ONCE (a crew caps its flight's pilot losses); flights
    // without a crew book no personnel movement.
    const auto cit = flight_crews_.find(victim_flight);
    if (cit != flight_crews_.end()) {
        FlightCrew& fc = cit->second;
        for (const auto slot : fc.crew) {
            auto& book = personnel_[fc.squadron];
            PilotDelta* d = find_pilot_delta_(book, slot);
            if (d == nullptr) {
                PilotDelta nd;
                nd.slot = slot;
                nd.out = true;   // drawn with the flight
                book.push_back(nd);
                d = &book.back();
            }
            if (d->dead) continue;
            d->dead = true;
            if (auto* sq = find_squadron_(fc.squadron)) {
                ++sq->run_pilot_losses;
            }
            PilotLossRecord lrec;
            lrec.t_s = sim_time_s;
            lrec.team = fc.team;
            lrec.squadron = fc.squadron;
            lrec.flight = victim_flight;
            lrec.slot = slot;
            pilot_losses_.push_back(lrec);
            break;
        }
    }

    ++air_losses_;
    losses_.push_back(rec);
}

void CampaignResultLedger::apply_mission_draw(
        double t_s, std::uint8_t team, std::uint32_t squadron_vu,
        int count) {
    apply_mission_draw(t_s, team, squadron_vu, count, 0, {});
}

void CampaignResultLedger::apply_mission_draw(
        double t_s, std::uint8_t team, std::uint32_t squadron_vu,
        int count, std::uint32_t flight_vu,
        const std::vector<std::uint8_t>& crew) {
    if (count <= 0 && crew.empty()) return;

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

    // DOM-3 — the crew: the flight's roster slots go OUT (drawn onto
    // the booked flight), the assignment log records the pick, and the
    // flight→crew map arms the loss/recovery paths. An unknown
    // squadron still books the flight map (the crew is flight-keyed);
    // the personnel deltas need the squadron's own row.
    if (crew.empty()) return;
    PilotAssignmentRecord arec;
    arec.t_s = t_s;
    arec.team = team;
    arec.squadron = squadron_vu;
    arec.flight = flight_vu;
    arec.crew = crew;
    pilot_assignments_.push_back(std::move(arec));

    FlightCrew fc;
    fc.team = team;
    fc.squadron = squadron_vu;
    fc.crew = crew;
    flight_crews_[flight_vu] = std::move(fc);

    if (sq != nullptr) {
        auto& book = personnel_[squadron_vu];
        for (const auto slot : crew) {
            PilotDelta* d = find_pilot_delta_(book, slot);
            if (d == nullptr) {
                PilotDelta nd;
                nd.slot = slot;
                book.push_back(nd);
                d = &book.back();
            }
            d->out = true;
        }
    }
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

    // DOM-3 — the crew returns: surviving slots come OFF the books
    // (out = false) and each flew its sortie (missions credited, the
    // recovery log records the face the event family rides). Dead
    // slots stay dead — the flight's losses are spent. The map entry
    // goes with the flight.
    const auto cit = flight_crews_.find(flight_vu);
    if (cit != flight_crews_.end()) {
        FlightCrew fc = cit->second;
        flight_crews_.erase(cit);
        for (const auto slot : fc.crew) {
            auto& book = personnel_[fc.squadron];
            PilotDelta* d = find_pilot_delta_(book, slot);
            if (d == nullptr) continue;
            d->out = false;
            if (d->dead) continue;
            ++d->missions_added;
            if (auto* fc_sq = find_squadron_(fc.squadron)) {
                ++fc_sq->run_pilot_sorties;
            }
            PilotRecoveryRecord rrec;
            rrec.t_s = t_s;
            rrec.team = fc.team;
            rrec.squadron = fc.squadron;
            rrec.flight = flight_vu;
            rrec.slot = slot;
            rrec.missions_run = d->missions_added;
            pilot_recoveries_.push_back(rrec);
        }
    }
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

void CampaignResultLedger::sync_squadron_ratings(
        std::uint32_t squadron_vu,
        const std::array<std::uint8_t, 16>& ratings) {
    // DOM-3 — last-write-wins per squadron (the fstatus discipline):
    // the ATM pushes its live view after each decay fire; the
    // write-back reads the final face, the fires counter is the
    // activity marker (a run the decay never touched writes nothing).
    if (auto* sq = find_squadron_(squadron_vu)) {
        sq->role_ratings = ratings;
        ++sq->ratings_fires;
    }
}

const std::vector<PilotDelta>*
CampaignResultLedger::squadron_personnel(std::uint32_t squadron_vu) const {
    const auto it = personnel_.find(squadron_vu);
    return it == personnel_.end() ? nullptr : &it->second;
}

const FlightCrew*
CampaignResultLedger::flight_crew(std::uint32_t flight_vu) const {
    const auto it = flight_crews_.find(flight_vu);
    return it == flight_crews_.end() ? nullptr : &it->second;
}

int CampaignResultLedger::apply_reinforcements(double t_s,
                                                bool stock_flow) {
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

    // DOM-2 — the strategic reserve keeps the order books full: each
    // team's consumed budgets refill toward their wire snapshot out of
    // replacements_avail, slot order (the teams_ vector) then wire
    // order (the squadrons_ vector), the reserve draining as it gives.
    // OFF (default) = the C2 shape: budgets consumed, never
    // replenished, the reserve untouched.
    if (stock_flow) {
        for (auto& tl : teams_) {
            if (tl.replacements_avail <= 0) continue;
            for (auto& sq : squadrons_) {
                if (sq.owner != tl.slot) continue;
                const int want = sq.reinforce_initial - sq.reinforce_pending;
                if (want <= 0) continue;
                const int take = std::min(want, tl.replacements_avail);
                sq.reinforce_pending += take;
                tl.replacements_avail -= take;
                tl.replacements_spent += take;
            }
        }
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

void CampaignResultLedger::apply_objective_repair(
        const ObjectiveRepairRecord& rec) {
    if (rec.objective == 0 || rec.features_repaired <= 0) return;
    repairs_.push_back(rec);
    features_repaired_ += rec.features_repaired;

    // The repaired face rides the damage-state map (the write-back's
    // own source): replace the objective's entry, else create one —
    // the same last-write-wins rule apply_objective_damage keeps, and
    // the destroyed counters move by its replace-the-entry arithmetic.
    ObjectiveDamageRecord face;
    face.objective = rec.objective;
    face.features_total = static_cast<int>(rec.fstatus.size()) * 4;
    face.features_destroyed = rec.features_destroyed;
    face.destroyed_pct = face.features_total > 0
        ? (100 * rec.features_destroyed) / face.features_total : 0;
    face.fstatus = rec.fstatus;
    for (auto& existing : objective_damage_) {
        if (existing.objective == rec.objective) {
            features_destroyed_ += face.features_destroyed
                                   - existing.features_destroyed;
            existing = face;
            return;
        }
    }
    objective_damage_.push_back(face);
    objective_vus_.push_back(face.objective);
    features_destroyed_ += face.features_destroyed;
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

void CampaignResultLedger::apply_action_filing(
        double t_s,
        std::uint8_t team,
        std::uint8_t mission,
        std::uint8_t action_type,
        std::uint8_t context,
        std::uint32_t objective_vu,
        int damage_pct) {
    // CAMP-ATM-1 — pure observation: the ACTION filing moved no books
    // (the request rides the ATM's own pipeline; a filled sortie draws
    // through apply_mission_draw as any other). Log + counter only.
    ActionFilingRecord rec;
    rec.t_s = t_s;
    rec.team = team;
    rec.mission = mission;
    rec.action_type = action_type;
    rec.context = context;
    rec.objective = objective_vu;
    rec.damage_pct = damage_pct < 0 ? 0 : (damage_pct > 100 ? 100
                                                            : damage_pct);
    action_filings_.push_back(rec);
    ++actions_filed_;
}

void CampaignResultLedger::apply_slot_denial(double t_s,
                                             std::uint8_t team,
                                             std::uint32_t airbase_vu,
                                             std::uint8_t reason) {
    // CAMP-DOM-4 — pure observation: the denial moved no books (the
    // request fell to the next-best squadron or kept its estimate).
    // Log only — the totals answer is the log's size (the honest 0 is
    // the arms-off answer).
    SlotDenialRecord rec;
    rec.t_s = t_s;
    rec.team = team;
    rec.airbase = airbase_vu;
    rec.reason = reason;
    slot_denials_.push_back(rec);
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
    Members m(w, ",\n    ");
    m.key("air_losses", air_losses_);
    w.put(",\n    ");
    m.key("air_kills_attributed", air_kills_attributed_);
    w.put(",\n    ");
    m.key("air_losses_unattributed", air_losses_unattributed_);
    w.put(",\n    ");
    m.key("ag_kills", ag_kills_);
    w.put(",\n    ");
    m.key("bomb_impacts", static_cast<std::int64_t>(impacts_.size()));
    w.put(",\n    ");
    w.number_key("objectives_damaged",
                 static_cast<std::int64_t>(objective_damage_.size()));
    w.put(",\n    ");
    m.key("features_destroyed", features_destroyed_);
    w.put(",\n    ");
    // C2 — the tasking side (one pool: draws, losses, resupply).
    m.key("mission_draws", mission_draws_);
    w.put(",\n    ");
    m.key("mission_draw_aircraft", mission_draw_aircraft_);
    w.put(",\n    ");
    m.key("draws_unmatched", draws_unmatched_);
    w.put(",\n    ");
    m.key("reinforcement_fires", reinforcement_fires_);
    w.put(",\n    ");
    m.key("aircraft_reinforced", aircraft_reinforced_);
    w.put(",\n    ");
    // C4 — mission recovery (drawn aircraft that completed and
    // returned). Emitted ALWAYS (the C2 keys above are): a 0 is the
    // honest "no ATM pipeline / no completions yet" answer, and the
    // QC's recovery gate reads the totals block.
    m.key("mission_recoveries", mission_recoveries_);
    w.put(",\n    ");
    m.key("aircraft_recovered", aircraft_recovered_);
    w.put(",\n    ");
    // CAMP-DOM-3 — the personnel totals (the logs' sizes: one crew per
    // assignment record, one slot per loss/recovery record). The 0 is
    // the honest "arms off / no rosters" answer.
    w.number_key("pilot_assignments",
                 static_cast<std::int64_t>(pilot_assignments_.size()));
    w.put(",\n    ");
    w.number_key("pilot_losses",
                 static_cast<std::int64_t>(pilot_losses_.size()));
    w.put(",\n    ");
    w.number_key("pilot_sorties",
                 static_cast<std::int64_t>(pilot_recoveries_.size()));
    w.put(",\n    ");
    // CAMP-DOM-4 — the scheduling books' total (the log's size; the
    // honest 0 is the arms-off answer).
    w.number_key("slot_denials",
                 static_cast<std::int64_t>(slot_denials_.size()));
    w.put("\n  }");

    // Teams: slot order (the snapshot's order), initial + remaining +
    // losses — the existence picture — plus the C2 tasking view
    // (drawn/reinforced/aircraft_tasking) the availability gate reads.
    Array teams_arr{w, ",\n  \"teams\": [", "\n    ", ",\n    ", "\n  ]", "]"};
    for (std::size_t i = 0; i < teams_.size(); ++i) {
        const auto& t = teams_[i];
        teams_arr.element_prefix();
        w.put("{");
        Members em(w, ", ");
        em.key("slot", t.slot);
        em.key("name", t.name);
        em.key("aircraft_initial", t.aircraft_initial);
        em.key("aircraft_remaining", t.aircraft_remaining);
        em.key("air_losses", t.losses);
        em.key("aircraft_drawn", t.drawn);
        em.key("aircraft_reinforced", t.reinforced);
        em.key("aircraft_tasking", team_aircraft_tasking(t.slot));
        // DOM-2: the strategic reserve's books, only when the stock
        // flow moved anything (a pristine ledger emits byte-identical
        // team rows).
        if (t.replacements_spent != 0 || t.replacements_avail != t.replacements_initial) {
            em.key("replacements_initial", t.replacements_initial);
            em.key("replacements_avail", t.replacements_avail);
            em.key("replacements_spent", t.replacements_spent);
        }
        w.put("}");
    }

    // Squadrons: only the ones with THIS-RUN activity (kills, losses,
    // draws, or reinforcement), VU-sorted for stability regardless of
    // arrival order.
    {
        std::vector<const SquadronLedger*> active;
        for (const auto& s : squadrons_) {
            // THIS-RUN deltas only — a mid-campaign save seeds non-zero
            // absolutes, and the artifact reports what happened THIS
            // run, not the save's own history. DOM-3: the personnel
            // books count as activity too.
            if (s.run_aa_kills != 0 || s.run_ag_kills != 0 ||
                s.run_losses != 0 || s.run_draws != 0 ||
                s.run_reinforced != 0 || s.run_recoveries != 0 ||
                s.run_pilot_losses != 0 || s.run_pilot_sorties != 0 ||
                s.ratings_fires != 0) {
                active.push_back(&s);
            }
        }
        std::sort(active.begin(), active.end(),
                  [](const SquadronLedger* a, const SquadronLedger* b) {
                      return a->vu < b->vu;
                  });
    Array squadrons_arr{w, ",\n  \"squadrons\": [", "\n    ", ",\n    ", "\n  ]", "]"};
        for (std::size_t i = 0; i < active.size(); ++i) {
            const auto& s = *active[i];
        squadrons_arr.element_prefix();
        w.put("{");
        Members em(w, ", ");
            em.key("vu", s.vu);
            em.key("name", s.name);
            em.key("owner", s.owner);
            em.key("aa_kills", s.aa_kills);
            em.key("ag_kills", s.ag_kills);
            em.key("total_losses", s.total_losses);
            // The C2 tasking counters (one pool).
            em.key("aircraft_available", s.availability);
            w.number_key("aircraft_tasking",
                         squadron_tasking_available(s.vu));
            em.key("run_draws", s.run_draws);
            if (s.run_recoveries != 0) {
                em.key("run_recoveries", s.run_recoveries);
            }
            em.key("run_reinforced", s.run_reinforced);
            em.key("reinforce_budget", s.reinforce_pending);
            // DOM-3: the personnel books, only when the run moved the
            // roster or the decay fired (a pristine ledger emits
            // byte-identical squadron rows).
            if (s.run_pilot_losses != 0 || s.run_pilot_sorties != 0 ||
                s.ratings_fires != 0) {
                em.key("run_pilot_losses", s.run_pilot_losses);
                em.key("run_pilot_sorties", s.run_pilot_sorties);
                if (s.ratings_fires != 0) {
                    w.put("\"role_ratings\": [");
                    for (int ri = 0; ri < 16; ++ri) {
                        if (ri) w.put(", ");
                        w.number(
                            s.role_ratings[static_cast<std::size_t>(ri)]);
                    }
                    w.put("]");
                }
            }
            w.put("}");
        }
    }

    // Mission draws: arrival order (the tasking ledger's own log).
    Array mission_draws_arr{w, ",\n  \"mission_draws\": [", "\n    ", ",\n    ", "\n  ]", "]"};
    for (std::size_t i = 0; i < draws_.size(); ++i) {
        const auto& d = draws_[i];
        mission_draws_arr.element_prefix();
        Members em(w, ", ");
        w.put("{\"t_ms\": ");
        w.put(time_ms(d.t_s));
        em.key("team", d.team);
        em.key("squadron", d.squadron);
        em.key("aircraft", d.count);
        w.put("}");
    }

    // C4 — mission recoveries: arrival order (the draw's mirror log).
    // Only present when one exists — legacy runs stay byte-identical.
    if (!recoveries_.empty()) {
    Array mission_recoveries_arr{w, ",\n  \"mission_recoveries\": [", "\n    ", ",\n    ", "\n  ]", "]"};
        for (std::size_t i = 0; i < recoveries_.size(); ++i) {
            const auto& rc = recoveries_[i];
        mission_recoveries_arr.element_prefix();
        Members em(w, ", ");
            w.put("{\"t_ms\": ");
            w.put(time_ms(rc.t_s));
            em.key("team", rc.team);
            em.key("squadron", rc.squadron);
            em.key("flight", rc.flight);
            em.key("released", rc.released);
            w.put("}");
        }
        w.put("\n  ]");
    }

    // Reinforcement deliveries: arrival order, one record per
    // receiving squadron per fire.
    Array reinforcements_arr{w, ",\n  \"reinforcements\": [", "\n    ", ",\n    ", "\n  ]", "]"};
    for (std::size_t i = 0; i < reinforcements_.size(); ++i) {
        const auto& r = reinforcements_[i];
        reinforcements_arr.element_prefix();
        Members em(w, ", ");
        w.put("{\"t_ms\": ");
        w.put(time_ms(r.t_s));
        em.key("team", r.team);
        em.key("squadron", r.squadron);
        em.key("delivered", r.delivered);
        em.key("budget_left", r.budget_left);
        w.put("}");
    }

    // CAMP-DOM-3 — the personnel logs: arrival order, one record per
    // crew/loss/sortie. Only present when one exists — the arms-off
    // and no-roster runs stay byte-identical.
    if (!pilot_assignments_.empty()) {
    Array pilot_assignments_arr{w, ",\n  \"pilot_assignments\": [", "\n    ", ",\n    ", "\n  ]", "]"};
        for (std::size_t i = 0; i < pilot_assignments_.size(); ++i) {
            const auto& a = pilot_assignments_[i];
        pilot_assignments_arr.element_prefix();
        Members em(w, ", ");
            w.put("{\"t_ms\": ");
            w.put(time_ms(a.t_s));
            em.key("team", a.team);
            em.key("squadron", a.squadron);
            em.key("flight", a.flight);
            w.put(", \"crew\": [");
            for (std::size_t c = 0; c < a.crew.size(); ++c) {
                if (c) w.put(", ");
                w.number(a.crew[c]);
            }
            w.put("]}");
        }
        w.put("\n  ]");
    }
    if (!pilot_losses_.empty()) {
    Array pilot_losses_arr{w, ",\n  \"pilot_losses\": [", "\n    ", ",\n    ", "\n  ]", "]"};
        for (std::size_t i = 0; i < pilot_losses_.size(); ++i) {
            const auto& l = pilot_losses_[i];
        pilot_losses_arr.element_prefix();
        Members em(w, ", ");
            w.put("{\"t_ms\": ");
            w.put(time_ms(l.t_s));
            em.key("team", l.team);
            em.key("squadron", l.squadron);
            em.key("flight", l.flight);
            em.key("slot", l.slot);
            w.put("}");
        }
        w.put("\n  ]");
    }
    if (!pilot_recoveries_.empty()) {
    Array pilot_recoveries_arr{w, ",\n  \"pilot_recoveries\": [", "\n    ", ",\n    ", "\n  ]", "]"};
        for (std::size_t i = 0; i < pilot_recoveries_.size(); ++i) {
            const auto& rc = pilot_recoveries_[i];
        pilot_recoveries_arr.element_prefix();
        Members em(w, ", ");
            w.put("{\"t_ms\": ");
            w.put(time_ms(rc.t_s));
            em.key("team", rc.team);
            em.key("squadron", rc.squadron);
            em.key("flight", rc.flight);
            em.key("slot", rc.slot);
            em.key("missions_run", rc.missions_run);
            w.put("}");
        }
        w.put("\n  ]");
    }

    // CAMP-DOM-4 — the slot-denial log: arrival order, one record per
    // refused flight (the pick gate's skips and the horizon's
    // refusals). Only present when one exists — the arms-off runs
    // stay byte-identical.
    if (!slot_denials_.empty()) {
    Array slot_denials_arr{w, ",\n  \"slot_denials\": [", "\n    ", ",\n    ", "\n  ]", "]"};
        for (std::size_t i = 0; i < slot_denials_.size(); ++i) {
            const auto& d = slot_denials_[i];
        slot_denials_arr.element_prefix();
        Members em(w, ", ");
            w.put("{\"t_ms\": ");
            w.put(time_ms(d.t_s));
            em.key("team", d.team);
            em.key("airbase", d.airbase);
            em.key("reason", d.reason);
            w.put("}");
        }
        w.put("\n  ]");
    }

    // Air-loss events: arrival order (the log).
    Array air_losses_arr{w, ",\n  \"air_losses\": [", "\n    ", ",\n    ", "\n  ]", "]"};
    for (std::size_t i = 0; i < losses_.size(); ++i) {
        const auto& l = losses_[i];
        air_losses_arr.element_prefix();
        Members em(w, ", ");
        w.put("{\"t_ms\": ");
        w.put(time_ms(l.sim_time_s));
        em.key("victim_team", l.victim_team);
        em.key("victim_squadron", l.victim_squadron);
        em.key("victim_flight", l.victim_flight);
        em.key("killer_squadron", l.killer_squadron);
        w.put(l.attributed ? ", \"attributed\": true"
                           : ", \"attributed\": false");
        w.put("}");
    }

    // Bomb impacts: arrival order.
    Array bomb_impacts_arr{w, ",\n  \"bomb_impacts\": [", "\n    ", ",\n    ", "\n  ]", "]"};
    for (std::size_t i = 0; i < impacts_.size(); ++i) {
        const auto& im = impacts_[i];
        bomb_impacts_arr.element_prefix();
        Members em(w, ", ");
        w.put("{\"t_ms\": ");
        w.put(time_ms(im.sim_time_s));
        em.key("objective", im.objective);
        em.key("miss_ft", im.miss_distance_ft);
        em.key("features_destroyed", im.features_destroyed);
        w.put("}");
    }

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
    Array objectives_arr{w, ",\n  \"objectives\": [", "\n    ", ",\n    ", "\n  ]", "]"};
        for (std::size_t i = 0; i < sorted.size(); ++i) {
            const auto& o = *sorted[i];
        objectives_arr.element_prefix();
        w.put("{");
        Members om(w, ", ");
            om.key("vu", o.objective);
            om.key("features_total", o.features_total);
            om.key("features_destroyed", o.features_destroyed);
            om.key("destroyed_pct", o.destroyed_pct);
            w.put(", \"fstatus\": \"");
            static const char kHex[] = "0123456789abcdef";
            for (const auto b : o.fstatus) {
                w.put(kHex[(b >> 4) & 0x0F]);
                w.put(kHex[b & 0x0F]);
            }
            w.put("\"}");
        }
    }

    // G1 — the ground war block. OPTIONAL (the same discipline as
    // mission_recoveries): a run with no ground activity emits the
    // byte-identical pre-G1 document; a ground war emits its own books
    // here (totals, per-team ground rows, VU-sorted battalion final
    // states, arrival-ordered loss and capture events). No floats:
    // integer grid positions, whole-byte states, ms times.
    if (!ground_losses_.empty() || !captures_.empty() ||
        !ground_units_.empty() || !repairs_.empty()) {
        w.put(",\n  \"ground\": {");
        w.put("\n    ");
        Members gm(w, ",\n    ");
        gm.key("vehicle_losses", ground_vehicle_losses_);
        gm.key("battalions_destroyed", ground_battalions_destroyed_);
        gm.key("objectives_captured", ground_objectives_captured_);

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
                Array ground_teams_arr{w, ",\n    \"teams\": [",
                                       "\n      {", ",\n      {",
                                       "\n    ]", "\n    ]"};
                for (const auto& t : teams_) {
                    if (t.ground_losses == 0 && t.battalions_destroyed == 0 &&
                        t.objectives_captured == 0) {
                        continue;
                    }
                    Members em = ground_teams_arr.element();
                    em.key("slot", t.slot);
                    em.key("name", t.name);
                    em.key("vehicle_losses", t.ground_losses);
                    em.key("battalions_destroyed", t.battalions_destroyed);
                    em.key("objectives_captured", t.objectives_captured);
                    w.put("}");
                }
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
            Array units_arr{w, ",\n    \"units\": [", "\n      {",
                            ",\n      {", "\n    ]", "]"};
            for (std::size_t i = 0; i < sorted.size(); ++i) {
                const auto& g = *sorted[i];
                Members em = units_arr.element();
                em.key("vu", g.vu);
                em.key("owner", g.owner);
                em.key("strength_initial", g.strength_initial);
                em.key("strength", g.strength);
                em.key("run_losses", g.run_losses);
                em.key("x", g.x);
                em.key("y", g.y);
                em.key("supply", g.supply);
                em.key("morale", g.morale);
                em.key("fatigue", g.fatigue);
                w.put(g.destroyed ? ", \"destroyed\": true"
                                  : ", \"destroyed\": false");
                w.put("}");
            }
        }

        // Ground loss events: arrival order (the log).
        {
            Array losses_arr{w, ",\n    \"losses\": [", "\n      ",
                             ",\n      ", "\n    ]", "]"};
            for (const auto& l : ground_losses_) {
                Members em = losses_arr.element_opened("{\"t_ms\": ");
                w.put(time_ms(l.t_s));
                em.key("victim", l.victim);
                em.key("victim_team", l.victim_team);
                em.key("attacker", l.attacker);
                if (l.attacker_team != 0) {
                    em.key("attacker_team", l.attacker_team);
                }
                if (l.killer_squadron != 0) {
                    em.key("killer_squadron", l.killer_squadron);
                }
                em.key("kills", l.kills);
                w.put(l.air ? ", \"air\": true" : ", \"air\": false");
                w.put("}");
            }
        }

        // Captures: arrival order (the territorial log).
        {
            Array captures_arr{w, ",\n    \"captures\": [", "\n      ",
                               ",\n      ", "\n    ]", "]"};
            for (const auto& c : captures_) {
                Members em = captures_arr.element_opened("{\"t_ms\": ");
                w.put(time_ms(c.t_s));
                em.key("objective", c.objective);
                em.key("from_team", c.from_team);
                em.key("to_team", c.to_team);
                em.key("by_battalion", c.by_battalion);
                w.put("}");
            }
        }

        // DOM-2 — the repair log: arrival order (the objective_repaired
        // event family's source). Optional inside the block: a
        // resupply-only ground war emits byte-identical ground objects,
        // exactly the ground-quiet rule the block itself follows.
        if (!repairs_.empty()) {
            Array repairs_arr{w, ",\n    \"repairs\": [", "\n      ",
                              ",\n      ", "\n    ]", "\n    ]"};
            for (const auto& rp : repairs_) {
                Members em = repairs_arr.element_opened("{\"t_ms\": ");
                w.put(time_ms(rp.t_s));
                em.key("objective", rp.objective);
                em.key("owner", rp.owner);
                em.key("repaired", rp.features_repaired);
                em.key("destroyed", rp.features_destroyed);
                em.key("supply", rp.supply);
                em.key("last_repair", rp.last_repair);
                w.put(", \"fstatus\": \"");
                static const char kHex[] = "0123456789abcdef";
                for (const std::uint8_t b : rp.fstatus) {
                    w.put(kHex[(b >> 4) & 0x0F]);
                    w.put(kHex[b & 0x0F]);
                }
                w.put("\"}");
            }
        }

        w.put("\n  }");
    }

    // CAMP-ATM-1 — the ACTION tables' filing log. OPTIONAL (the same
    // discipline as mission_recoveries): a run with no ACTION filings
    // (every pre-ATM-1 run, every disarmed run) emits the
    // byte-identical document.
    if (!action_filings_.empty()) {
        Array actions_arr{w, ",\n  \"actions\": [", "\n    ", ",\n    ",
                          "\n  ]", "\n  ]"};
        for (const auto& a : action_filings_) {
            Members em = actions_arr.element_opened("{\"t_ms\": ");
            w.put(time_ms(a.t_s));
            em.key("team", a.team);
            em.key("mission", a.mission);
            em.key("action_type", a.action_type);
            em.key("context", a.context);
            em.key("objective", a.objective);
            em.key("damage_pct", a.damage_pct);
            w.put("}");
        }
    }


    w.put("\n}\n");
    return w.str();
}

} // namespace f4::campaign
