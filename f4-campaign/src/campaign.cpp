// f4-campaign/src/campaign.cpp
//
// Implementation of the headless campaign engine — see campaign.hpp for
// the M4.7-skeleton semantics (cycle cadence, availability gates, TOT
// rule, determinism contract).

#include <f4/campaign/campaign.hpp>

#include <f4/json/f4_json.hpp>

#include "squadron_snapshot.hpp"

#include <algorithm>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace f4::campaign {

namespace {

// Squared nothing, magic nothing — the cycle arithmetic works in whole
// seconds; the mid-window TOT rounds half a minute up deterministically.
CampaignTime midpoint_tot(CampaignTime now, int min_time_min, int max_time_min) {
    const CampaignTime mid_min =
        (static_cast<CampaignTime>(min_time_min) + max_time_min) / 2;
    return now + mid_min * 60;
}

} // namespace

Campaign::Campaign(const f4::world::ICampaignSource& camp,
                   const f4::world::ITeamSource& teams,
                   const f4::world::IUnitCoreSource& units,
                   const MissionProfileTable& profiles,
                   f4::messaging::MessageBus& bus,
                   const CampaignConfig& cfg)
    : camp_(camp)
    , teams_(teams)
    , units_(units)
    , profiles_(profiles)
    , bus_(bus)
    , cfg_(cfg) {
    if (cfg_.air_task_cycle_sec <= 0) {
        throw std::runtime_error(
            "campaign: air_task_cycle_sec must be positive (got " +
            std::to_string(cfg_.air_task_cycle_sec) + ")");
    }

    // C2: the reinforcement cadence's absolute-time base. The tick's
    // clock is campaign-RELATIVE; the .cmp timers are ABSOLUTE in the
    // save's epoch — epoch_ bridges the two (now = epoch_ + clock_).
    epoch_ = camp_.current_time();
    last_reinforce_ = camp_.last_reinforcement();

    // Snapshot the squadron force ONCE through the shared helper
    // (src/squadron_snapshot.hpp): the roster DECODED from the wire's
    // 2-bit group packing (the C2 fix — the raw u32 is 1.4 billion for
    // a 24-ship wing on any real v71 save), squadrons without a roster
    // sharing the team's te_number_aircraft pool, and the wire's own
    // reinforcement budget per squadron. The result ledger snapshots
    // through the SAME function — one rule, two consumers, zero drift
    // (the numbers agree by construction).
    const auto force = detail::snapshot_squadron_force(camp_, teams_, units_);
    pool_.assign(force.team_pool.begin(), force.team_pool.end());
    squadrons_.reserve(force.squadrons.size());
    for (const auto& s : force.squadrons) {
        SquadronRef ref;
        ref.id_num = s.vu;
        ref.owner = s.owner;
        ref.specialty = s.specialty;
        ref.name = s.name;
        ref.available = s.available;
        ref.airbase = s.airbase;
        squadrons_.push_back(std::move(ref));
    }

    // C4: construct the ATM pipeline when armed (the legacy ladder
    // otherwise — the goldens-pinned path). The id base matches the
    // Campaign's own package counter so intent ids stay in one
    // monotonic namespace either mode.
    if (cfg_.atm_pipeline) {
        atm_ = std::make_unique<AirTaskingManager>(
            profiles_, camp_, teams_, units_, nullptr, cfg_.atm);
        atm_->set_id_base(cfg_.first_package_id);
    }
    next_flight_id_ = cfg_.first_package_id;
}

std::vector<int> Campaign::belligerent_teams() const {
    // Named slots first (same rule the sim-side team resolution uses —
    // unnamed/neutral slots are not belligerents).
    struct SlotTeam {
        int slot;
        const std::vector<int16_t>* stance;
    };
    std::vector<SlotTeam> named;
    for (int t = 0; t < teams_.team_count(); ++t) {
        if (teams_.name(t).empty()) continue;
        named.push_back({teams_.slot(t), &teams_.stance(t)});
    }

    std::vector<int> out;
    for (const auto& a : named) {
        // Belligerence: a WAR row toward another named team (RelType 5 —
        // the vocabulary's own top rung, cmpglobl.h; the pre-C3 sign test
        // misread the -5141 garbage real saves carry toward unused slots
        // as war). relation_from_wire maps that garbage to NoRelations,
        // so phantom slots can never make a team a belligerent.
        const bool belligerent = std::any_of(
            named.begin(), named.end(), [&](const SlotTeam& b) {
                if (b.slot == a.slot) return false;
                const auto& st = *a.stance;
                return b.slot < static_cast<int>(st.size()) &&
                       f4::world::relation_from_wire(
                           st[static_cast<std::size_t>(b.slot)]) ==
                           f4::world::Relation::War;
            });
        if (belligerent) out.push_back(a.slot);
    }
    std::sort(out.begin(), out.end());
    return out;
}

int Campaign::team_aircraft_pool_(int team_slot) const {
    if (team_slot < 0 || team_slot >= 8) return 0;
    return pool_[static_cast<std::size_t>(team_slot)];
}

void Campaign::tick(CampaignTime delta_sec) {
    if (delta_sec < 0) {
        throw std::runtime_error("campaign: negative tick delta (" +
                                 std::to_string(delta_sec) + ")");
    }
    clock_ += delta_sec;

    // Fire every cycle that has come due, in order — each at its OWN
    // due time (next_cycle_, already advanced to this cycle's slot):
    // a delta spanning several cycles fires them all in order, and a
    // big tick now produces EXACTLY what N small ticks produce (the
    // pre-C4 code fired them all at the advanced clock — equivalent
    // only for single-cycle horizons, and C4's slot scheduling needs
    // the true cycle time: takeoff estimates past the 160-minute
    // schedule horizon can never slot).
    while (next_cycle_ + cfg_.air_task_cycle_sec <= clock_) {
        next_cycle_ += cfg_.air_task_cycle_sec;
        ++cycles_fired_;
        run_tasking_cycle_();
    }

    // C2: the reinforcement cadence rides the same tick (after the
    // tasking fires — a boundary that lands mid-tick sees the depleted
    // pools first, FreeFalcon's own ordering). C4: mission recovery
    // rides with it — completed flights' survivors return BEFORE the
    // next cycle's draws (the same order the reference's squadron
    // schedule bookkeeping produces).
    recover_missions_();
    fire_reinforcements_();
}

void Campaign::recover_missions_() {
    if (atm_ == nullptr) return;
    const auto releases = atm_->recover_completed(clock_);
    for (const auto& rel : releases) {
        if (result_ledger_ != nullptr) {
            result_ledger_->apply_mission_recovery(
                static_cast<double>(clock_), rel.team, rel.squadron_vu,
                rel.flight_id, rel.survivors);
        }
        // No-ledger mode: the ATM refilled its own counters (the
        // releases are telemetry only).
    }
}

void Campaign::fire_reinforcements_() {
    if (cfg_.reinforcement_period_sec <= 0) return;
    if (result_ledger_ == nullptr) return;   // legacy mode: pure B.3

    // Absolute-time gate: now > anchor + period (FreeFalcon's own
    // shape). On fire the anchor JUMPS to now — a stale save timer
    // (TestCamp carries 0 against a 38.5M-second epoch) fires exactly
    // ONCE, not the years it is behind.
    while (epoch_ + clock_ >
           last_reinforce_ + cfg_.reinforcement_period_sec) {
        last_reinforce_ = std::max(epoch_ + clock_,
                                   last_reinforce_ +
                                       cfg_.reinforcement_period_sec);
        ++reinforcement_fires_;
        // The delivery: deficits refilled from the wire's per-squadron
        // budgets, into the ledger (the write model — write-back and
        // artifacts see it). Campaign-relative seconds in the log.
        (void)result_ledger_->apply_reinforcements(
            static_cast<double>(clock_));
    }
}

std::uint32_t Campaign::select_target_(std::uint8_t team) const {
    if (objectives_ == nullptr) return 0;

    // Enemy-owned = either direction's relation is WAR (RelType 5 —
    // the symmetric belligerence rule; hostile-but-not-war (4) is not
    // a targeting relationship, the RoE table denies strike ROE).
    // Ranked priority-desc, wire-order-asc — fully deterministic.
    // (FreeFalcon's targets come from the strategy layer's mission
    // requests — objective scoring feeds them; that layer is the C4
    // ATM tranche. Priority + enmity is the honest deterministic
    // subset.)
    const auto hostile = [&](std::uint8_t other) {
        if (other == team) return false;
        for (int t = 0; t < teams_.team_count(); ++t) {
            if (teams_.slot(t) != static_cast<int>(other)) continue;
            const auto& row = teams_.stance(t);
            const auto idx = static_cast<std::size_t>(team);
            if (idx < row.size() &&
                f4::world::relation_from_wire(row[idx]) ==
                    f4::world::Relation::War)
                return true;
        }
        for (int t = 0; t < teams_.team_count(); ++t) {
            if (teams_.slot(t) != static_cast<int>(team)) continue;
            const auto& row = teams_.stance(t);
            const auto idx = static_cast<std::size_t>(other);
            if (idx < row.size() &&
                f4::world::relation_from_wire(row[idx]) ==
                    f4::world::Relation::War)
                return true;
        }
        return false;
    };

    std::uint32_t best = 0;
    int best_priority = -1;
    for (int i = 0; i < objectives_->objective_count(); ++i) {
        const std::uint8_t owner = objectives_->owner(i);
        if (!hostile(owner)) continue;
        const int priority = objectives_->priority(i);
        if (priority > best_priority) {
            best_priority = priority;
            best = objectives_->id_num(i);
        }
    }
    return best;
}

void Campaign::run_tasking_cycle_() {
    if (atm_ != nullptr) {
        run_tasking_cycle_atm_();
    } else {
        run_tasking_cycle_legacy_();
    }
}

void Campaign::run_tasking_cycle_legacy_() {
    // The cycle's own due time (next_cycle_ — see tick(): a big tick
    // fires each cycle at ITS slot, not the advanced clock; the
    // single-cycle goldens have clock_ == next_cycle_ and are
    // byte-identical).
    const CampaignTime now = next_cycle_;
    const auto war_teams = belligerent_teams();

    // Availability — ONE pool (C2):
    //   * Ledger attached: the LEDGER's tasking view (snapshot − draws
    //     − non-drawn losses + reinforcements). The Campaign's own
    //     counters are NOT touched in this mode; each generated
    //     mission debits the ledger (apply_mission_draw) so cycles,
    //     combat, and resupply deplete/refill the same numbers the
    //     write-back and the artifacts carry.
    //   * No ledger (legacy): the Campaign's own per-squadron counter,
    //     drawn down by each mission — B.3's shape, byte-identical
    //     goldens. A fresh ledger reports exactly these numbers (the
    //     shared force snapshot), so the two modes agree until events
    //     land — the golden identity.
    const auto effective_available = [&](const SquadronRef* sq) {
        if (result_ledger_ != nullptr) {
            return result_ledger_->squadron_tasking_available(sq->id_num);
        }
        return sq->available;
    };

    for (const int slot : war_teams) {
        // The team's squadrons, wire order (deterministic).
        std::vector<SquadronRef*> team_squadrons;
        for (auto& sq : squadrons_) {
            if (sq.owner == static_cast<std::uint8_t>(slot)) {
                team_squadrons.push_back(&sq);
            }
        }
        if (team_squadrons.empty()) continue;   // nobody to task

        // Walk the profile table in wire-byte order — the free,
        // data-driven mission-menu ordering.
        for (std::uint8_t byte = 1; byte < kMissionTypeCount; ++byte) {
            const auto& profile = profiles_.for_mission(byte);

            // Capability gate: this slice cannot verify vehicle
            // capabilities (VEH_STEALTH, VTOL, ...) — profiles that
            // require one don't generate (conservative default).
            if (!profile.caps.empty()) continue;

            // Role gate: some team squadron must fly this role. With
            // the C3 role FALLBACK armed (CampaignConfig — hosts
            // exercising generation-to-spawn; the reference's own
            // selection scores rather than gates), a team with no
            // exact-role squadron tasks its best-available wing
            // instead (TestCamp's ROK-DPRK war fields all-counter-air
            // squadrons — the strict gate would never generate a
            // delivery mission). The fallback preserves the strict
            // pick when a role match EXISTS (exact role first, the
            // same most-available tie-break).
            SquadronRef* lead = nullptr;
            for (auto* sq : team_squadrons) {
                if (aro_name(sq->specialty) != profile.aro) continue;
                // Pick the squadron with the most effectively-available
                // aircraft (post-loss when the ledger is attached);
                // tie -> wire-order-first (strictly greater keeps the
                // earlier one, which is the deterministic tie-break).
                if (!lead ||
                    effective_available(sq) > effective_available(lead))
                    lead = sq;
            }
            if (!lead && cfg_.tasking_role_fallback) {
                for (auto* sq : team_squadrons) {
                    if (!lead ||
                        effective_available(sq) > effective_available(lead))
                        lead = sq;
                }
            }
            if (!lead) continue;

            // Aircraft gate: the profile's default package size must be
            // coverable by the squadron's effectively-available aircraft.
            const int count = std::min(profile.str,
                                       effective_available(lead));
            if (count <= 0) continue;

            MissionIntent intent;
            intent.issued_time = now;
            intent.time_on_target =
                midpoint_tot(now, profile.min_time, profile.max_time);
            intent.team = static_cast<std::uint8_t>(slot);
            for (int t = 0; t < teams_.team_count(); ++t) {
                if (teams_.slot(t) == slot) {
                    intent.team_name = teams_.name(t);
                    break;
                }
            }
            intent.mission_byte = byte;
            intent.mission_name = std::string(mission_type_name(byte));
            intent.aircraft_count = count;
            intent.squadron_id = lead->id_num;
            intent.squadron_name = lead->name;
            intent.package_id = cfg_.first_package_id +
                                static_cast<std::uint32_t>(intents_.size());
            intent.flight_id = intent.package_id;

            // Draw the aircraft down — ONE pool: the ledger when
            // attached (apply_mission_draw books the tasking debit
            // where combat losses and reinforcement already live),
            // else the Campaign's own counter (the B.3 legacy).
            if (result_ledger_ != nullptr) {
                result_ledger_->apply_mission_draw(
                    static_cast<double>(now),
                    static_cast<std::uint8_t>(slot), lead->id_num, count);
            } else {
                lead->available -= count;
            }

            // C3: the route — generation-to-spawn. This slice arms the
            // END-TO-END-meaningful family: the A-G delivery profiles
            // (STRIKE/BOMB/GNDSTRIKE/NAVSTRIKE/SAD/SEAD over an
            // OBJECTIVE) get the team's selected enemy objective and
            // the airbase → target → airbase route through the threat
            // map. CAP-style profiles also target OBJECTIVE but the
            // objective is FRIENDLY airspace whose racetrack pattern
            // is the loiter tranche; UNIT/LOCATION targets wait for
            // their resolution tranches — both publish route-less,
            // exactly as before C3. Failures are loud counters, not
            // dropped missions — the QC gate reads them.
            if (route_planner_ != nullptr && lead->airbase != 0 &&
                profile_flies_delivery_route(profile)) {
                const std::uint32_t target_vu =
                    select_target_(static_cast<std::uint8_t>(slot));
                if (target_vu != 0) {
                    const auto rb = route_planner_->build(
                        static_cast<std::uint8_t>(slot), profile,
                        lead->airbase, target_vu);
                    if (rb.waypoints.size() >= 2) {
                        intent.target_objective_id = target_vu;
                        intent.route = rb.waypoints;
                        intent.synthetic = true;
                        ++routes_built_;
                        if (rb.safe_path_searched) ++route_safe_searches_;
                        if (rb.direct_fallback) ++route_fallbacks_;
                    } else {
                        ++routes_failed_;
                    }
                } else {
                    ++routes_failed_;
                }
            }

            // Publish + record (the campaign's only outward coupling).
            bus_.publish(intent);
            intents_.push_back(intent);
        }
    }
}

void Campaign::run_tasking_cycle_atm_() {
    // C4 — the ATM pipeline's campaign half: phases 1-5 run inside the
    // ATM; THIS function owns phase 6 (route planning — the RouteBuilder
    // attachment, package-shared) and phase 7 (slot scheduling via
    // atm_->schedule_takeoff), then publishes one intent per flight.
    // The cycle's own due time (see tick()).
    const CampaignTime now = next_cycle_;
    const auto war_teams = belligerent_teams();

    for (const int slot : war_teams) {
        const auto team = static_cast<std::uint8_t>(slot);

        // Phases 1-5: request generation, prioritization,
        // deconfliction, package building (FindBestAir), support
        // assignment (escort pairing) — composed in the reference's
        // order.
        auto requests = atm_->generate_requests(team, now);
        requests = atm_->prioritize(std::move(requests));
        requests = atm_->deconflict(std::move(requests));
        auto flights = atm_->compose_packages(requests, team, now);

        // The package's route: the MAIN flight's build, shared by the
        // package's escorts (package-shared ingress — the C3 deferral
        // this tranche closes). Main flights come first in compose
        // order, so a simple by-package cache suffices.
        std::unordered_map<std::uint32_t, std::vector<RouteWaypoint>>
            package_routes;

        for (auto& ft : flights) {
            const auto& profile = profiles_.for_mission(ft.mission);

            // PHASE 6 — the route (the C3 builder, now package-aware):
            // the MAIN flight's build is the package's route — the
            // escorts share it (package-shared ingress, the C3
            // deferral this tranche closes; TOTs differ by the
            // separation). Main flights come first in compose order.
            if (route_planner_ != nullptr && ft.role == FlightRole::Main &&
                ft.airbase_vu != 0 && ft.target_vu != 0 &&
                profile_flies_delivery_route(profile)) {
                const auto rb = route_planner_->build(
                    team, profile, ft.airbase_vu, ft.target_vu);
                if (rb.waypoints.size() >= 2) {
                    package_routes[ft.package_id] = rb.waypoints;
                    ++routes_built_;
                    if (rb.safe_path_searched) ++route_safe_searches_;
                    if (rb.direct_fallback) ++route_fallbacks_;
                } else {
                    ++routes_failed_;
                }
            }

            // PHASE 7 — the takeoff slot snap (also the recovery
            // booking — schedule_takeoff is the commit point).
            (void)atm_->schedule_takeoff(ft);

            // The intent — one per flight, the spawner's contract.
            MissionIntent intent;
            intent.issued_time = now;
            intent.time_on_target = ft.tot;   // post-snap
            intent.team = team;
            for (int t = 0; t < teams_.team_count(); ++t) {
                if (teams_.slot(t) == slot) {
                    intent.team_name = teams_.name(t);
                    break;
                }
            }
            intent.mission_byte = ft.mission;
            intent.mission_name = std::string(mission_type_name(ft.mission));
            intent.aircraft_count = ft.aircraft;
            intent.squadron_id = ft.squadron_vu;
            intent.squadron_name = ft.squadron_name;
            intent.package_id = ft.package_id;
            intent.flight_id = ft.flight_id;
            intent.target_objective_id = ft.target_vu;
            intent.flight_role = static_cast<std::uint8_t>(ft.role);
            intent.escorted_flight_id = ft.escorted_flight_id;

            // The route: the package's copy (main built it; escorts
            // carry the same shape with their own TOT).
            if (route_planner_ != nullptr) {
                const auto it = package_routes.find(ft.package_id);
                if (it != package_routes.end()) {
                    intent.route = it->second;
                    intent.synthetic = true;
                }
            }

            // Draw the aircraft — ONE pool (the ledger when attached;
            // the ATM's own counters otherwise — its draw_ already
            // handled the no-ledger bookkeeping at compose time).
            if (result_ledger_ != nullptr) {
                result_ledger_->apply_mission_draw(
                    static_cast<double>(now), team, ft.squadron_vu,
                    ft.aircraft);
            }

            // Publish + record (the campaign's only outward coupling).
            bus_.publish(intent);
            intents_.push_back(std::move(intent));
        }
    }
}

std::string Campaign::to_summary_json() const {
    // Deterministic JSON: fixed key order, slot-ordered teams,
    // publish-ordered intents. No floats (everything integral).
    f4::json::Writer w;
    w.put("{\n  \"format\": \"f4-campaign-summary\",\n  \"version\": 1");
    w.put(",\n  \"clock_sec\": ");
    w.number(clock_);
    w.put(",\n  \"cycles_fired\": ");
    w.number(cycles_fired_);
    w.put(",\n  \"task_cycle_sec\": ");
    w.number(cfg_.air_task_cycle_sec);

    // C2: the reinforcement cadence's own summary block — ONLY when it
    // fired. Legacy runs (no ledger / disabled cadence) never see it,
    // so their goldens stay byte-identical; the FreshLedger identity
    // test's horizon never reaches the first fire.
    if (reinforcement_fires_ > 0) {
        w.put(",\n  \"reinforcement\": {\n    ");
        w.number_key("period_sec", cfg_.reinforcement_period_sec);
        w.put(",\n    ");
        w.number_key("fires", reinforcement_fires_);
        w.put(",\n    ");
        w.number_key("aircraft_delivered",
                     result_ledger_ != nullptr
                         ? result_ledger_->aircraft_reinforced()
                         : 0);
        w.put("\n  }");
    }

    // C3: the route tranche's own summary block — only when a planner
    // was attached and built at least one route (no-planner runs keep
    // byte-identical goldens — the attachment contract).
    if (routes_built_ > 0) {
        w.put(",\n  \"routes\": {\n    ");
        w.number_key("built", routes_built_);
        w.put(",\n    ");
        w.number_key("failed", routes_failed_);
        w.put(",\n    ");
        w.number_key("safe_path_searches", route_safe_searches_);
        w.put(",\n    ");
        w.number_key("direct_fallbacks", route_fallbacks_);
        w.put("\n  }");
    }

    // C4: the ATM pipeline's own summary block — only when armed
    // (legacy runs keep byte-identical goldens; the mode switch IS an
    // opt-in).
    if (atm_ != nullptr) {
        const auto& a = atm_->stats();
        w.put(",\n  \"atm\": {\n    ");
        w.number_key("requests_generated", a.requests_generated);
        w.put(",\n    ");
        w.number_key("requests_seeded", a.requests_seeded);
        w.put(",\n    ");
        w.number_key("requests_timed_out", a.requests_timed_out);
        w.put(",\n    ");
        w.number_key("requests_budget_dropped", a.requests_budget_dropped);
        w.put(",\n    ");
        w.number_key("requests_deconflicted", a.requests_deconflicted);
        w.put(",\n    ");
        w.number_key("requests_unfilled", a.requests_unfilled);
        w.put(",\n    ");
        w.number_key("packages_built", a.packages_built);
        w.put(",\n    ");
        w.number_key("escorts_built", a.escorts_built);
        w.put(",\n    ");
        w.number_key("slot_snaps", a.slot_snaps);
        w.put(",\n    ");
        w.number_key("slot_shifts_sec", a.slot_shifts_sec);
        w.put(",\n    ");
        w.number_key("recoveries", a.recoveries);
        w.put(",\n    ");
        w.number_key("aircraft_recovered", a.aircraft_recovered);
        w.put("\n  }");
    }

    w.put(",\n  \"belligerent_teams\": [");
    {
        bool first = true;
        for (const int slot : belligerent_teams()) {
            if (!first) w.put(", ");
            first = false;
            std::string name;
            for (int t = 0; t < teams_.team_count(); ++t) {
                if (teams_.slot(t) == slot) {
                    name = teams_.name(t);
                    break;
                }
            }
            w.string(name);
        }
    }
    w.put("]");

    w.put(",\n  \"intents\": [");
    for (std::size_t i = 0; i < intents_.size(); ++i) {
        const auto& in = intents_[i];
        w.put(i ? ",\n    " : "\n    ");
        w.put("{\"time\": ");
        w.number(in.issued_time);
        w.put(", \"tot\": ");
        w.number(in.time_on_target);
        w.put(", \"team\": ");
        w.number(in.team);
        w.string_key(", team_name", in.team_name);
        w.put(", \"mission_byte\": ");
        w.number(in.mission_byte);
        w.string_key(", mission", in.mission_name);
        w.put(", \"aircraft\": ");
        w.number(in.aircraft_count);
        w.put(", \"squadron_id\": ");
        w.number(in.squadron_id);
        w.string_key(", squadron", in.squadron_name);
        w.put(", \"package_id\": ");
        w.number(in.package_id);
        w.put(", \"flight_id\": ");
        w.number(in.flight_id);
        if (in.target_objective_id != 0) {
            w.put(", \"target\": ");
            w.number(in.target_objective_id);
            w.put(", \"route_wps\": ");
            w.number(in.route.size());
        }
        // C4: the package composition (support flights only — legacy
        // intents and mains keep the byte-identical shape).
        if (in.flight_role != 0) {
            w.put(", \"flight_role\": ");
            w.number(in.flight_role);
            w.put(", \"escorts\": ");
            w.number(in.escorted_flight_id);
        }
        w.put("}");
    }
    w.put("\n  ]");

    w.put(",\n  \"totals\": {");
    {
        // Per-team, per-mission counts — team slot order, then byte order.
        bool first_team = true;
        for (const int slot : belligerent_teams()) {
            // Count this team's missions by byte.
            std::vector<int> by_byte(kMissionTypeCount, 0);
            for (const auto& in : intents_) {
                if (in.team == static_cast<std::uint8_t>(slot)) {
                    ++by_byte[in.mission_byte];
                }
            }
            for (std::size_t b = 1; b < kMissionTypeCount; ++b) {
                if (by_byte[b] == 0) continue;
                w.put(first_team ? "\n    " : ",\n    ");
                first_team = false;
                std::string name;
                for (int t = 0; t < teams_.team_count(); ++t) {
                    if (teams_.slot(t) == slot) {
                        name = teams_.name(t);
                        break;
                    }
                }
                w.string(name + "/" + std::string(mission_type_name(static_cast<std::uint8_t>(b))));
                w.put(": ");
                w.number(by_byte[b]);
            }
        }
    }
    w.put("\n  }\n}\n");
    return w.str();
}

// ============================================================================
// emit_flight_intents (B.3 tranche — live saved flights)
// ============================================================================
namespace {

/// Flight roster: the same 16-group 2-bit packing every roster on the
/// wire uses (shared decode in src/squadron_snapshot.hpp — 0xA0 = 4
/// aircraft). 0 is returned when the save carried nothing.
int flight_roster_aircraft(std::uint32_t roster) {
    return detail::roster_group_aircraft(roster);
}

} // namespace

std::vector<MissionIntent>
emit_flight_intents(const f4::world::IUnitCoreSource& units,
                    const f4::world::IFlightSource& flights,
                    f4::messaging::MessageBus& bus,
                    CampaignTime now,
                    const f4::world::ITeamSource* teams) {
    using namespace f4::world;

    // id_num → display name, for squadron_name resolution. One pass over
    // the units; flights reference squadrons by VU_ID.num.
    std::unordered_map<std::uint32_t, std::string> name_by_id;
    for (int i = 0; i < units.unit_count(); ++i) {
        const auto id = units.id_num(i);
        if (id != 0) {
            name_by_id.emplace(id, units.class_name(i));
        }
    }

    // team slot → name, when the team source is provided.
    std::unordered_map<int, std::string> team_name_by_slot;
    if (teams) {
        for (int t = 0; t < teams->team_count(); ++t) {
            team_name_by_slot.emplace(teams->slot(t), teams->name(t));
        }
    }

    std::vector<MissionIntent> intents;
    for (int i = 0; i < units.unit_count(); ++i) {
        if (units.unit_class(i) != f4::entities::UnitClass::Flight) continue;

        // Contract: `flights` is the IFlightSource view of the SAME source
        // (WorldStateAdapters.units exposes both views on the same indices;
        // unit_class(i) == Flight implies flights' subclass fields at i are
        // this unit's). Skip untasked flights — AMIS_NONE means the save
        // carries no tasking for them.
        const std::uint8_t mission = flights.mission(i);
        if (!is_mission_tasked(mission)) continue;

        MissionIntent in;
        in.issued_time = now;
        in.time_on_target = flights.time_on_target(i);
        in.team = units.owner(i);
        {
            auto it = team_name_by_slot.find(static_cast<int>(in.team));
            if (it != team_name_by_slot.end()) in.team_name = it->second;
        }
        in.mission_byte = mission;
        in.mission_name = std::string(mission_type_name(mission));
        in.aircraft_count = flight_roster_aircraft(units.roster(i));
        in.squadron_id = flights.squadron_id(i);
        {
            auto it = name_by_id.find(in.squadron_id);
            if (it != name_by_id.end()) in.squadron_name = it->second;
        }
        in.package_id = flights.package_id(i);
        in.flight_id = units.id_num(i);

        bus.publish(in);
        intents.push_back(std::move(in));
    }
    return intents;
}

} // namespace f4::campaign
