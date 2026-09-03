// f4-campaign/src/atm.cpp
//
// AirTaskingManager — the 7-phase tasking pipeline (C4). See atm.hpp
// for the phase map, the FindBestAir term list, and the documented
// simplifications. This file is the mechanics.

#include <f4/campaign/atm.hpp>
#include <f4/campaign/route_builder.hpp>   // profile_flies_delivery_route

#include "squadron_snapshot.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <utility>

namespace f4::campaign {

namespace {

// ATM_HIGH_PRIORITY (package.cpp:71) — the reference's threshold for
// threat escalation (priority + both bands nonzero → SEAD).
constexpr int kAtmHighPriority = 150;

// The reference's delay cap: a request delayed more than 8 times
// (8 x 30-minute TOT pushes) times out.
constexpr int kMaxDelays = 8;

// Default mission priority for teams whose .tea priority table is
// absent (kunsan-style fixtures, hand-built test sources): 50 —
// taskable, middle of the road. A PRESENT table with a 0 entry drops
// the request (GetPriority's own "player specified 0" rule).
constexpr int kDefaultMissionPriority = 50;

// The reference ARO enum indices (mission.h MissionRollEnum) for the
// profile vocabulary's six names — the UCD Scores[] column per role.
constexpr int kRefAroCa = 1;     // ARO_CA
constexpr int kRefAroS = 3;      // ARO_S
constexpr int kRefAroGa = 4;     // ARO_GA
constexpr int kRefAroSb = 5;     // ARO_SB
constexpr int kRefAroRec = 10;   // ARO_REC
constexpr int kRefAroOther = 16; // ARO_SUPPORT (no single column — OTHER)

/// Profile aro name → the reference's UCD Scores index.
int ref_aro_index(const MissionProfile& profile) {
    const std::string& a = profile.aro;
    if (a == "ARO_CA")  return kRefAroCa;
    if (a == "ARO_S")   return kRefAroS;
    if (a == "ARO_GA")  return kRefAroGa;
    if (a == "ARO_SB")  return kRefAroSb;
    if (a == "ARO_REC") return kRefAroRec;
    return kRefAroOther;   // ARO_SUPPORT and anything else
}

/// The role's specialty family (FindBestAir's `sc`): counter-air
/// roles pair with SQUADRON_SPECIALTY_AA(1), the ground-attack family
/// with SQUADRON_SPECIALTY_AG(2) — the reference's own mapping
/// (atm.cpp: FindBestAir's sc assignment).
int role_specialty_family(const MissionProfile& profile) {
    const std::string& a = profile.aro;
    if (a == "ARO_CA") return 1;   // SQUADRON_SPECIALTY_AA
    if (a == "ARO_S" || a == "ARO_GA" || a == "ARO_SB" || a == "ARO_REC")
        return 2;                  // SQUADRON_SPECIALTY_AG
    return 0;                      // support family: neither
}

/// Grid distance — the campaign's native measure (Distance()).
int grid_distance(int x1, int y1, int x2, int y2) {
    return std::max(std::abs(x1 - x2), std::abs(y1 - y2));
}

/// The enemy-of relation (either direction WAR — the symmetric
/// belligerence rule shared with Campaign::select_target_).
bool at_war_with(const f4::world::ITeamSource& teams, std::uint8_t a,
                 std::uint8_t b) {
    if (a == b) return false;
    for (int t = 0; t < teams.team_count(); ++t) {
        if (teams.slot(t) != static_cast<int>(a)) continue;
        const auto& row = teams.stance(t);
        const auto idx = static_cast<std::size_t>(b);
        if (idx < row.size() &&
            f4::world::relation_from_wire(row[idx]) ==
                f4::world::Relation::War)
            return true;
    }
    for (int t = 0; t < teams.team_count(); ++t) {
        if (teams.slot(t) != static_cast<int>(b)) continue;
        const auto& row = teams.stance(t);
        const auto idx = static_cast<std::size_t>(a);
        if (idx < row.size() &&
            f4::world::relation_from_wire(row[idx]) ==
                f4::world::Relation::War)
            return true;
    }
    return false;
}

} // namespace

// ============================================================================
// AirbaseSchedule
// ============================================================================

int AirbaseSchedule::find_slot(int minute, int plan_block_min,
                               int max_cycles) const noexcept {
    const int horizon = max_cycles * plan_block_min;
    if (minute < 0 || minute >= horizon) return -1;

    const auto occupied = [&](int m) {
        const int block = m / plan_block_min;
        const int slot = m % plan_block_min;
        return (blocks_[static_cast<std::size_t>(block)] &
                (0x01 << slot)) != 0;
    };

    // The exact slot, then the reference's two lookahead minutes.
    for (int m = minute; m < minute + 3 && m < horizon; ++m) {
        if (!occupied(m)) return m;
    }
    // Then up to 10 minutes backward ("find the next earliest slot
    // and add a timing leg", bounded the reference's way).
    for (int m = minute - 1; m >= 0 && m >= minute - 3 - 10; --m) {
        if (!occupied(m)) return m;
    }
    return -1;
}

void AirbaseSchedule::fill(int minute, int aircraft, int plan_block_min,
                           int max_cycles) noexcept {
    if (minute < 0) return;
    const int horizon = max_cycles * plan_block_min;
    const auto mark = [&](int m) {
        if (m < 0 || m >= horizon) return;
        const int block = m / plan_block_min;
        const int slot = m % plan_block_min;
        blocks_[static_cast<std::size_t>(block)] |=
            static_cast<std::uint8_t>(0x01 << slot);
    };
    mark(minute);
    // The same slot in the next block — the reference's fudge time.
    mark(minute + plan_block_min);
    // Flights larger than 2 ships take the next minute too.
    if (aircraft > 2) {
        mark(minute + 1);
        mark(minute + 1 + plan_block_min);
    }
}

bool AirbaseSchedule::block_full(int block,
                                 int plan_block_min) const noexcept {
    if (block < 0 || block >= 32) return true;
    std::uint8_t full = 0;
    for (int s = 0; s < plan_block_min && s < 8; ++s) {
        full = static_cast<std::uint8_t>(full | (0x01 << s));
    }
    return (blocks_[static_cast<std::size_t>(block)] & full) == full;
}

// ============================================================================
// Construction + state
// ============================================================================

AirTaskingManager::AirTaskingManager(
        const MissionProfileTable& profiles,
        const f4::world::ICampaignSource& camp,
        const f4::world::ITeamSource& teams,
        const f4::world::IUnitCoreSource& units,
        const f4::world::IObjectiveSource* objectives, AtmConfig cfg)
    : profiles_(profiles)
    , teams_(teams)
    , units_(units)
    , objectives_(objectives)
    , cfg_(cfg)
    , epoch_(camp.current_time()) {

    // The shared force snapshot (src/squadron_snapshot.hpp) — the same
    // rule the Campaign and the ledger snapshot through, so the three
    // agree by construction. The per-unit extras (grid position, wire
    // range, UCD scores) come from the unit records by VU lookup.
    const auto force = detail::snapshot_squadron_force(camp, teams, units);
    std::unordered_map<std::uint32_t, int> idx_by_vu;
    for (int i = 0; i < units.unit_count(); ++i) {
        if (units.unit_class(i) != f4::entities::UnitClass::Squadron) continue;
        idx_by_vu.emplace(units.id_num(i), i);
    }
    squadrons_.reserve(force.squadrons.size());
    for (const auto& s : force.squadrons) {
        SquadronState st;
        st.vu = s.vu;
        st.owner = s.owner;
        st.specialty = s.specialty;
        st.name = s.name;
        st.available = s.available;
        st.airbase = s.airbase;
        const auto it = idx_by_vu.find(s.vu);
        if (it != idx_by_vu.end()) {
            const int i = it->second;
            st.x = units.x(i);
            st.y = units.y(i);
            st.range = units.max_range(i);
            st.scores = units.unit_class_scores(i);
        }
        squadrons_.push_back(std::move(st));
    }

    // Seed the airbase schedules from the decoded wire bitmask (one
    // schedule per ATM airbase list entry, deduplicated by VU across
    // teams — a shared base keeps the first team's seed, wire order).
    std::unordered_map<std::uint32_t, bool> seen;
    for (int t = 0; t < teams_.team_count(); ++t) {
        for (const auto& ab : teams_.atm_airbases(t)) {
            if (ab.id_num == 0 || seen.count(ab.id_num) != 0) continue;
            seen.emplace(ab.id_num, true);
            AirbaseSchedule sched(ab.id_num);
            sched.seed(ab.schedule);
            schedules_.push_back(std::move(sched));
        }
    }
}

const AirbaseSchedule*
AirTaskingManager::airbase_schedule(std::uint32_t airbase_vu) const noexcept {
    for (const auto& s : schedules_) {
        if (s.airbase_vu() == airbase_vu) return &s;
    }
    return nullptr;
}

int AirTaskingManager::available_(const SquadronState& sq) const {
    if (ledger_ != nullptr) {
        return ledger_->squadron_tasking_available(sq.vu);
    }
    return sq.available;
}

void AirTaskingManager::draw_(SquadronState& sq, int count) {
    // Ledger mode: the CAMPAIGN books apply_mission_draw when it
    // publishes the intent (one booking site, no double counting);
    // the ATM only tracks outstanding for its own bookkeeping.
    // No-ledger mode: the ATM's own counter is the pool (the
    // Campaign's legacy counters are untouched in ATM mode).
    sq.drawn_outstanding += count;
    if (ledger_ == nullptr) {
        sq.available -= count;
        if (sq.available < 0) sq.available = 0;
    }
}

// ============================================================================
// PHASE 1 — request generation
// ============================================================================

void AirTaskingManager::seed_backlog_() {
    if (backlog_seeded_) return;
    backlog_seeded_ = true;

    // The decoded ATO backlog — the save's own queued requests (the
    // output of the reference ATM's last runs + the strategy layer's
    // filings). The wire TOTs are ABSOLUTE; rebase to the relative
    // clock (epoch = the save's current_time at construction). Past
    // TOTs take the reference's 30-minute delay pushes, capped at 8
    // (beyond that, timeout — counted).
    for (int t = 0; t < teams_.team_count(); ++t) {
        for (const auto& wreq : teams_.atm_requests(t)) {
            if (wreq.who >= 8) continue;
            if (!is_mission_tasked(wreq.mission) ||
                wreq.mission >= kMissionTypeCount) {
                continue;   // corrupt backlog byte — skip, not error
            }
            MissionRequest req;
            req.mission = wreq.mission;
            req.team = wreq.who;
            req.target_id = wreq.target_num;
            req.priority = wreq.priority;
            req.aircraft = wreq.aircraft;
            req.seeded = true;
            req.tot_type = TotType::LE;

            const CampaignTime rel = wreq.tot - epoch_;
            if (rel < 0) {
                const std::int64_t pushes = (-rel) / 1800 + 1;
                if (pushes > kMaxDelays) {
                    ++stats_.requests_timed_out;
                    continue;
                }
                req.delayed = static_cast<std::uint8_t>(pushes);
                req.tot = rel + pushes * 1800;
            } else {
                req.tot = rel;
            }
            backlog_[wreq.who].push_back(req);
            ++stats_.requests_seeded;
        }
    }
}

std::vector<MissionRequest>
AirTaskingManager::generate_requests(std::uint8_t team, CampaignTime now) {
    std::vector<MissionRequest> out;

    // The backlog seed (once — the cache is per team, so every
    // belligerent's first cycle sees its own queue).
    seed_backlog_();
    out = backlog_[team];
    backlog_[team].clear();
    // Backlog entries whose TOT is still ahead of `now` flow as-is;
    // anything the seed already pushed lands past now by construction
    // (delay pushes are ≥ now + 1 block). Entries that expired
    // between cycles take one more push or time out here.
    {
        std::vector<MissionRequest> keep;
        keep.reserve(out.size());
        for (auto& req : out) {
            if (req.tot < now) {
                if (req.delayed >= kMaxDelays) {
                    ++stats_.requests_timed_out;
                    continue;
                }
                ++req.delayed;
                req.tot = now + 1800 * req.delayed;
            }
            keep.push_back(std::move(req));
        }
        out = std::move(keep);
    }

    // The profile ladder (the C3 walk, now emitting requests): wire
    // byte order, capability + mission-priority gating, deterministic
    // target rotation for the delivery family.
    const auto enemy = enemy_objectives_(team);

    // The mission-priority table's presence: the emission writes the
    // whole row or nothing (world_state parses whole arrays), so ANY
    // nonzero entry in this team's row means the table is real.
    bool priority_table_present = false;
    for (int t = 0; t < teams_.team_count() && !priority_table_present; ++t) {
        if (teams_.slot(t) != static_cast<int>(team)) continue;
        for (std::uint8_t b = 1; b < kMissionTypeCount; ++b) {
            if (teams_.mission_priority(t, b) != 0) {
                priority_table_present = true;
                break;
            }
        }
    }

    for (std::uint8_t byte = 1; byte < kMissionTypeCount; ++byte) {
        const auto& profile = profiles_.for_mission(byte);

        // Capability gate (this slice cannot verify vehicle caps —
        // the same conservative default the legacy ladder applies).
        if (!profile.caps.empty()) continue;

        // Mission-priority gate (GetPriority's drop rule). Teams
        // without a priority table keep the default — fixture and
        // hand-built sources must still task.
        if (priority_table_present) {
            int mprio = 0;
            for (int t = 0; t < teams_.team_count(); ++t) {
                if (teams_.slot(t) != static_cast<int>(team)) continue;
                mprio = teams_.mission_priority(t, byte);
                break;
            }
            if (mprio <= 0) continue;   // the team never requests this
        }

        MissionRequest req;
        req.mission = byte;
        req.team = team;
        req.aircraft = profile.str;
        req.tot_type = TotType::LE;
        // TOT: the C3 midpoint rule (the strategy layer that feeds
        // the reference's request TOTs is a later tranche).
        const CampaignTime mid_min =
            (static_cast<CampaignTime>(profile.min_time) +
             profile.max_time) / 2;
        req.tot = now + mid_min * 60;

        // Target: the delivery family rotates across the ranked enemy
        // objectives (packages spread over the target list — one per
        // request, deterministic); everything else stays target-less
        // (route-less, exactly as before C4).
        if (!enemy.empty() && profile_flies_delivery_route(profile)) {
            req.target_id = enemy[static_cast<std::size_t>(
                                     target_cursor_[team]) %
                                 enemy.size()];
            target_cursor_[team] = (target_cursor_[team] + 1) %
                                   static_cast<int>(enemy.size());
        }

        req.priority = request_priority_(team, profile, req.target_id);
        out.push_back(req);
        ++stats_.requests_generated;
    }
    return out;
}

int AirTaskingManager::request_priority_(
        std::uint8_t team, const MissionProfile& profile,
        std::uint32_t target_vu) const {
    // GetPriority's deterministic subset (team.cpp): the mission term
    // plus the target term. The reference's PO/package/distance/random
    // terms need strategy-layer data — skipped, documented in the
    // header.
    int mission_prio = kDefaultMissionPriority;
    for (int t = 0; t < teams_.team_count(); ++t) {
        if (teams_.slot(t) != static_cast<int>(team)) continue;
        const int mp = teams_.mission_priority(t, profile.mission_byte);
        if (mp > 0) mission_prio = mp;   // a 0 row keeps the default
        break;                            // (the drop rule ran at
    }                                     // generation, not here)
    int target_prio = mission_prio;       // no target → mission term
    if (objectives_ != nullptr && target_vu != 0) {
        for (int i = 0; i < objectives_->objective_count(); ++i) {
            if (objectives_->id_num(i) != target_vu) continue;
            // The objective-type term (GetObjTypePriority/2) + the
            // objective's own priority scaling — the reference's own
            // arithmetic, both components wire data now.
            int ot_prio = 0;
            for (int t = 0; t < teams_.team_count(); ++t) {
                if (teams_.slot(t) != static_cast<int>(team)) continue;
                ot_prio = teams_.objtype_priority(
                    t, static_cast<int>(objectives_->objective_type(i)));
                break;
            }
            target_prio = ot_prio / 2;
            target_prio += (target_prio *
                            static_cast<int>(objectives_->priority(i))) / 100;
            break;
        }
    }
    return mission_prio + target_prio;
}

std::vector<std::uint32_t>
AirTaskingManager::enemy_objectives_(std::uint8_t team) const {
    // Priority-desc, wire order — the C3 select_target_ ranking, kept
    // as a list so generation rotates across it.
    if (objectives_ == nullptr) return {};
    std::vector<std::pair<std::pair<int, int>, std::uint32_t>> keyed;
    for (int i = 0; i < objectives_->objective_count(); ++i) {
        const std::uint8_t owner = objectives_->owner(i);
        if (owner == team) continue;
        if (!at_war_with(teams_, team, owner)) continue;
        // Negate priority for desc; the wire index breaks ties
        // (std::sort is not stable, so the key folds it in).
        keyed.emplace_back(
            std::make_pair(-static_cast<int>(objectives_->priority(i)), i),
            objectives_->id_num(i));
    }
    std::sort(keyed.begin(), keyed.end());
    std::vector<std::uint32_t> out;
    out.reserve(keyed.size());
    for (const auto& k : keyed) out.push_back(k.second);
    return out;
}

// ============================================================================
// PHASE 2 — prioritization
// ============================================================================

std::vector<MissionRequest>
AirTaskingManager::prioritize(std::vector<MissionRequest> requests) {
    // Stable priority sort: fold the arrival index (priority desc,
    // then generation order — seeds first, wire byte asc).
    using Keyed = std::pair<std::pair<int, std::size_t>, MissionRequest>;
    std::vector<Keyed> keyed;
    keyed.reserve(requests.size());
    for (std::size_t i = 0; i < requests.size(); ++i) {
        keyed.emplace_back(std::make_pair(-requests[i].priority, i),
                           std::move(requests[i]));
    }
    std::sort(keyed.begin(), keyed.end(),
              [](const Keyed& a, const Keyed& b) {
                  return a.first < b.first;
              });
    std::vector<MissionRequest> out;
    out.reserve(keyed.size());
    for (auto& k : keyed) out.push_back(std::move(k.second));

    // The tempo budget: only the first missions_per_cycle requests
    // (0 = unlimited). The reference counts FILLED missions against
    // missionsToFill; requests are regenerated every cycle, so a
    // dropped request re-enters the next cycle's list — counted, not
    // silently lost.
    if (cfg_.missions_per_cycle > 0 &&
        static_cast<int>(out.size()) > cfg_.missions_per_cycle) {
        stats_.requests_budget_dropped +=
            static_cast<int>(out.size()) - cfg_.missions_per_cycle;
        out.resize(static_cast<std::size_t>(cfg_.missions_per_cycle));
    }
    return out;
}

// ============================================================================
// PHASE 3 — deconfliction
// ============================================================================

std::vector<MissionRequest>
AirTaskingManager::deconflict(std::vector<MissionRequest> requests) {
    std::vector<MissionRequest> out;
    out.reserve(requests.size());
    for (auto& req : requests) {
        const auto& profile = profiles_.for_mission(req.mission);
        // The profile's own separation fields (0 on the shipped
        // table — the gate is a no-op there; tests pin the semantics).
        const int min_dist = profile.mindistance;
        const int min_time = profile.mintime;
        bool clash = false;
        if (min_dist > 0 || min_time > 0) {
            // The targets' grid positions (both resolve or neither —
            // same objectives source).
            int ax = 0, ay = 0, rx = 0, ry = 0;
            bool have_a = false, have_r = false;
            if (objectives_ != nullptr) {
                for (int i = 0; i < objectives_->objective_count(); ++i) {
                    const std::uint32_t vu = objectives_->id_num(i);
                    if (vu == req.target_id) {
                        rx = objectives_->x(i);
                        ry = objectives_->y(i);
                        have_r = true;
                    }
                }
            }
            for (const auto& act : booked_) {
                if (act.mission != req.mission || act.team != req.team)
                    continue;
                have_a = false;
                if (objectives_ != nullptr) {
                    for (int i = 0; i < objectives_->objective_count();
                         ++i) {
                        if (objectives_->id_num(i) == act.target_vu) {
                            ax = objectives_->x(i);
                            ay = objectives_->y(i);
                            have_a = true;
                            break;
                        }
                    }
                }
                const int dist =
                    (have_a && have_r) ? grid_distance(ax, ay, rx, ry) : 0;
                const CampaignTime dtot = std::abs(
                    static_cast<std::int64_t>(act.tot - req.tot));
                if ((min_dist > 0 && have_a && have_r && dist < min_dist) ||
                    (min_time > 0 &&
                     dtot < static_cast<CampaignTime>(min_time) * 60)) {
                    clash = true;
                    break;
                }
            }
        }
        if (clash) {
            ++stats_.requests_deconflicted;
            continue;
        }
        out.push_back(std::move(req));
    }
    return out;
}

// ============================================================================
// PHASE 4 + 5 — package building + support assignment
// ============================================================================

std::vector<FlightTasking>
AirTaskingManager::compose_packages(
        const std::vector<MissionRequest>& requests, std::uint8_t team,
        CampaignTime now) {
    std::vector<FlightTasking> flights;

    for (const auto& req : requests) {
        const auto& profile = profiles_.for_mission(req.mission);

        // --- Phase 4: target analysis (the threat half of package.cpp
        // BuildPackage stage 1): ScoreThreatFast at the profile's
        // min/max target altitudes → NEED_SEAD (the reference: either
        // band above MIN_SEADESCORT_THREAT, or both nonzero at high
        // priority).
        bool need_sead = false;
        if (threat_ != nullptr && objectives_ != nullptr &&
            req.target_id != 0) {
            for (int i = 0; i < objectives_->objective_count(); ++i) {
                if (objectives_->id_num(i) != req.target_id) continue;
                const int tx = objectives_->x(i);
                const int ty = objectives_->y(i);
                const int ls = threat_->score(
                    tx, ty, alt_band_from_feet(profile.minalt * 100), team);
                const int hs = threat_->score(
                    tx, ty, alt_band_from_feet(profile.maxalt * 100), team);
                if (ls > cfg_.min_seadescort_threat ||
                    hs > cfg_.min_seadescort_threat) {
                    need_sead = true;
                } else if (req.priority > kAtmHighPriority && ls > 0 &&
                           hs > 0) {
                    need_sead = true;
                }
                break;
            }
        }

        // --- Phase 4: the main flight (FindBestAir) --------------------
        SquadronPick main_pick =
            find_best_air_(req, profile, team, now, nullptr);
        if (main_pick.squadron == nullptr) {
            ++stats_.requests_unfilled;
            continue;
        }
        const std::uint32_t package_id = next_package_id_++;

        FlightTasking main;
        main.mission = req.mission;
        main.team = team;
        main.squadron_vu = main_pick.squadron->vu;
        main.squadron_name = main_pick.squadron->name;
        main.airbase_vu = main_pick.squadron->airbase;
        main.target_vu = req.target_id;
        main.aircraft = req.aircraft;
        main.tot = req.tot;
        main.package_id = package_id;
        main.flight_id = next_flight_id_++;
        main.role = FlightRole::Main;
        main.separation_sec = 0;

        // Takeoff estimate: TOT − travel (the reference's FindBestAir
        // arithmetic — the distance/speed estimate, not the route; the
        // waypoint-timing tranche owns per-leg times).
        const CampaignTime travel = main_pick.travel_sec;
        main.takeoff = req.tot - travel;
        if (main.takeoff < now + 60) {
            // Can't be there in time: TYPE_LE semantics — shift the
            // TOT so takeoff is a minute out (the reference shifts
            // its estimate and continues; EQ-type would drop here).
            main.tot += (now + 60) - main.takeoff;
            main.takeoff = now + 60;
        }
        // Mission over: out + loiter + back + the doubled reserve
        // (the reference's mission-length arithmetic).
        main.mission_over = main.tot + travel +
                            static_cast<CampaignTime>(profile.loitertime) *
                                60 + travel +
                            2 * static_cast<CampaignTime>(cfg_.reserve_min) *
                                60;

        draw_(*main_pick.squadron, main.aircraft);
        flights.push_back(main);
        ++stats_.packages_built;

        // --- Phase 5: support assignment (escort pairing) --------------
        // The reference's ADDSEAD (+ NEED_SEAD) and ADDESCORT (always —
        // the "Marco edit") blocks. Each support flight: its own
        // profile, its own FindBestAir (with the package-lead bonuses),
        // TOT = main TOT + the support profile's separation, size
        // min(support str, main aircraft) for SEAD / the support
        // profile's str for the fighter escort.
        if (profile.has_flag("ADDSEAD") && need_sead) {
            FlightTasking sead;
            if (build_support_flight_(
                    sead, team, now, package_id, main, "AMIS_SEADESCORT",
                    std::min(profiles_.for_name("AMIS_SEADESCORT").str,
                             main.aircraft),
                    main_pick.squadron)) {
                sead.role = FlightRole::SeadEscort;
                flights.push_back(sead);
                ++stats_.escorts_built;
            }
        }
        if (profile.has_flag("ADDESCORT")) {
            // The reference flips a coin when escort_type is 0;
            // determinism picks the fighter escort.
            const auto escort_byte = profile.escort_type != 0
                                         ? profile.escort_type
                                         : mission_type_byte("AMIS_ESCORT")
                                               .value_or(10);
            FlightTasking esc;
            if (build_support_flight_(
                    esc, team, now, package_id, main,
                    mission_type_name(escort_byte),
                    profiles_.for_mission(escort_byte).str,
                    main_pick.squadron)) {
                esc.role = FlightRole::Escort;
                flights.push_back(esc);
                ++stats_.escorts_built;
            }
        }
    }
    return flights;
}

bool AirTaskingManager::build_support_flight_(
        FlightTasking& out, std::uint8_t team, CampaignTime now,
        std::uint32_t package_id, const FlightTasking& main,
        std::string_view support_name, int aircraft,
        const SquadronState* main_sq) {
    const auto& sprof = profiles_.for_name(support_name);

    // The support request: same target, TOT = main TOT + separation
    // (the reference's separation arithmetic — package.cpp's
    // "newmis.tot = mis_request.tot + separation").
    MissionRequest sreq;
    sreq.mission = sprof.mission_byte;
    sreq.team = team;
    sreq.target_id = main.target_vu;
    sreq.priority = 100;   // support parity: high enough to clear
                           // the lowestScore gate (the reference files
                           // its support requests at priority 0 but
                           // then GetPriority re-scores them; ours keep
                           // a constant — deterministic, documented)
    sreq.aircraft = aircraft;
    sreq.tot_type = TotType::LE;
    sreq.tot = main.tot + sprof.separation;

    SquadronPick pick = find_best_air_(sreq, sprof, team, now, main_sq);
    if (pick.squadron == nullptr) {
        // The reference cancels the ESCORT FLIGHT, not the package —
        // the main flight still generates (counted, never silent).
        return false;
    }

    out.mission = sprof.mission_byte;
    out.team = team;
    out.squadron_vu = pick.squadron->vu;
    out.squadron_name = pick.squadron->name;
    out.airbase_vu = pick.squadron->airbase;
    out.target_vu = main.target_vu;
    out.aircraft = sreq.aircraft;
    out.tot = sreq.tot;
    out.takeoff = out.tot - pick.travel_sec;
    if (out.takeoff < now + 60) {
        out.tot += (now + 60) - out.takeoff;
        out.takeoff = now + 60;
    }
    out.mission_over = out.tot + pick.travel_sec +
                       static_cast<CampaignTime>(sprof.loitertime) * 60 +
                       pick.travel_sec +
                       2 * static_cast<CampaignTime>(cfg_.reserve_min) * 60;
    out.package_id = package_id;
    out.flight_id = next_flight_id_++;
    out.escorted_flight_id = main.flight_id;
    out.separation_sec = sprof.separation;
    out.role = FlightRole::Escort;

    draw_(*pick.squadron, out.aircraft);
    return true;
}

// ============================================================================
// FindBestAir (atm.cpp:1534)
// ============================================================================

AirTaskingManager::SquadronPick
AirTaskingManager::find_best_air_(const MissionRequest& req,
                                  const MissionProfile& profile,
                                  std::uint8_t team, CampaignTime now,
                                  const SquadronState* lead) {
    SquadronPick out;
    out.squadron = nullptr;
    out.travel_sec = 0;

    // The target position (grid) for range/arrival estimates; no
    // target → zero travel for everyone (the station-keeping family —
    // the first candidate holds the quickest bonus through ties).
    int tx = 0, ty = 0;
    bool have_target = false;
    if (objectives_ != nullptr && req.target_id != 0) {
        for (int i = 0; i < objectives_->objective_count(); ++i) {
            if (objectives_->id_num(i) == req.target_id) {
                tx = objectives_->x(i);
                ty = objectives_->y(i);
                have_target = true;
                break;
            }
        }
    }

    const int sc = role_specialty_family(profile);
    const int lowest_score = (255 - req.priority) / 25;
    const auto travel_of = [&](int x, int y) {
        if (!have_target) return static_cast<CampaignTime>(0);
        const int d = grid_distance(x, y, tx, ty);
        return static_cast<CampaignTime>(
                   (d + cfg_.cruise_grid_per_min - 1) /
                   cfg_.cruise_grid_per_min) * 60;
    };

    CampaignTime quickest = 0;
    bool have_quickest = false;
    bool quickest_has_bonus = false;   // the reference's `bq`
    SquadronState* best = nullptr;
    int best_score = 0;

    for (auto& sq : squadrons_) {
        if (sq.owner != team) continue;

        // Base score: the rating term (UCD scores when present, else
        // the specialty fallback — rating_()).
        int score = (rating_(sq, profile) + 4) / 5;

        // The specialty bonus/penalty (±5): the wire specialty byte
        // vs the role's family (SQUADRON_SPECIALTY_AA/AG).
        if (sq.specialty != 0 && sc != 0) {
            score += (sq.specialty == sc) ? 5 : -5;
        }

        // The lowestScore gate: hopeless squadrons never compare.
        if (score <= lowest_score) continue;

        // Capability gate — same conservative rule as the ladder.
        if (!profile.caps.empty()) continue;

        // Availability: one short is tolerated at a penalty; none at
        // all never (the reference's av < aircraft-1 / av < 1 rule —
        // REQF_USERESERVES would relax it; the one-pool number is
        // already net of losses, see the header).
        const int av = available_(sq);
        if (av < 1) continue;
        if (av < req.aircraft - 1) continue;

        const CampaignTime travel = travel_of(sq.x, sq.y);

        // The airbase schedule gate: the START block full → skip (the
        // reference checks the block and the previous one; the block
        // derives from the requested takeoff here).
        if (const AirbaseSchedule* sched = airbase_schedule(sq.airbase)) {
            const CampaignTime to = req.tot - travel;
            const CampaignTime rel = to > now ? to - now : 0;
            const int block =
                static_cast<int>((rel / 60) / cfg_.plan_block_min);
            if (block < cfg_.max_cycles &&
                sched->block_full(block, cfg_.plan_block_min)) {
                continue;
            }
        }

        // --- The bonuses -----------------------------------------------
        if (lead != nullptr && lead->vu == sq.vu) {
            score += 3;   // the reference's SetAssigned reuse bonus
        }
        if (lead != nullptr && lead->airbase == sq.airbase) {
            score += 2;   // same airbase as the package lead
        }
        if (have_target) {
            // The +2 "within 1/2 range" bonus: the reference's own
            // rule when the wire range is nonzero (d < range/2); the
            // fixture fallback (no range data) fires for near targets
            // (≤ 60 grid units — a theater-typical planning radius).
            const int d = grid_distance(sq.x, sq.y, tx, ty);
            const bool within = sq.range > 0
                                    ? d * 2 < sq.range
                                    : d <= 60;
            if (within) score += 2;
        }
        if (av < req.aircraft) {
            score -= 5;   // one short of the request
        }
        // Quickest arrival, with the reference's previous-best
        // rebalancing: a new quickest takes +2 and the former leader
        // loses its bonus (unless it ties — `bq`).
        if (!have_quickest || travel < quickest) {
            score += 2;
            if (quickest_has_bonus && best != nullptr) {
                best_score -= 2;
            }
            quickest = travel;
            have_quickest = true;
            quickest_has_bonus = true;
        } else if (travel == quickest) {
            quickest_has_bonus = true;
        }

        if (score <= best_score) continue;
        best_score = score;
        best = &sq;
    }

    if (best == nullptr) return out;
    out.squadron = best;
    out.travel_sec = travel_of(best->x, best->y);
    return out;
}

int AirTaskingManager::rating_(const SquadronState& sq,
                               const MissionProfile& profile) const {
    // UCD Scores[ref_aro] when the unit carries a nonzero table (the
    // theater DB resolved its type); else the specialty fallback:
    // AA-specialty squadrons rate 100 in the counter-air family and
    // 30 outside it; AG-specialty the mirror; unspecialized 60 —
    // taskable everywhere, specialists at their specialty. The exact
    // numbers are F4's (the reference reads the UCD's 0..100 tables;
    // a fixture without one needs SOME deterministic rating, and the
    // specialty byte is the only role signal the wire itself carries).
    const int idx = ref_aro_index(profile);
    const int ucd = idx < static_cast<int>(sq.scores.size())
                        ? sq.scores[static_cast<std::size_t>(idx)]
                        : 0;
    if (ucd != 0) return ucd;
    const bool ca_family = profile.aro == "ARO_CA";
    if (sq.specialty == 1) return ca_family ? 100 : 30;
    if (sq.specialty == 2) return ca_family ? 30 : 100;
    return 60;
}

// ============================================================================
// PHASE 7 — TOT slot scheduling
// ============================================================================

CampaignTime AirTaskingManager::schedule_takeoff(FlightTasking& flight) {
    // Fresh schedule for bases the decoded list never carried (the
    // reference adds airbases lazily in DoCalculations the same way).
    AirbaseSchedule* sched = nullptr;
    for (auto& s : schedules_) {
        if (s.airbase_vu() == flight.airbase_vu) {
            sched = &s;
            break;
        }
    }
    if (sched == nullptr) {
        if (flight.airbase_vu == 0) {
            // No base: no slotting — but the flight is still booked
            // (the commit point is HERE regardless).
            booked_.push_back(flight);
            return 0;
        }
        schedules_.emplace_back(flight.airbase_vu);
        sched = &schedules_.back();
    }

    // The requested takeoff minute on the run's block grid (block 0 =
    // campaign start — the alignment documented in the header).
    const int minute =
        flight.takeoff > 0 ? static_cast<int>(flight.takeoff / 60) : 0;
    const int slot =
        sched->find_slot(minute, cfg_.plan_block_min, cfg_.max_cycles);
    if (slot < 0) {
        // Horizon exhausted: keep the estimate (the reference cancels
        // at 0xFFFFFFFF; the QC's slot telemetry sees the unscheduled
        // flight — the same spirit as the route builder's documented
        // direct-fallback deviation). Still booked for recovery.
        booked_.push_back(flight);
        return 0;
    }

    const CampaignTime snapped = static_cast<CampaignTime>(slot) * 60;
    const CampaignTime delta = snapped - flight.takeoff;
    sched->fill(slot, flight.aircraft, cfg_.plan_block_min, cfg_.max_cycles);

    flight.takeoff = snapped;
    flight.tot += delta;
    // The mission-over deadline follows the shift (the loiter and
    // return legs ride the same clock).
    flight.mission_over += delta;

    // The commit point: the flight is booked for recovery here (after
    // the snap, so the booked copy carries the final TOT/deadline).
    booked_.push_back(flight);

    ++stats_.slot_snaps;
    stats_.slot_shifts_sec += static_cast<int>(delta);
    return delta;
}

// ============================================================================
// Mission recovery
// ============================================================================

std::vector<RecoveryRelease>
AirTaskingManager::recover_completed(CampaignTime now) {
    std::vector<RecoveryRelease> out;
    std::vector<FlightTasking> still;
    still.reserve(booked_.size());

    for (const auto& ft : booked_) {
        if (ft.mission_over > now) {
            still.push_back(ft);
            continue;
        }
        // Survivors: drawn − the flight's booked losses (the ledger's
        // per-flight log; 0 when no ledger or no deaths).
        int losses = 0;
        if (ledger_ != nullptr) {
            losses = ledger_->flight_air_losses(ft.flight_id,
                                                ft.squadron_vu);
        }
        const int survivors = std::max(0, ft.aircraft - losses);

        RecoveryRelease rel;
        rel.team = ft.team;
        rel.squadron_vu = ft.squadron_vu;
        rel.flight_id = ft.flight_id;
        rel.survivors = survivors;
        out.push_back(rel);
        ++stats_.recoveries;
        stats_.aircraft_recovered += survivors;

        // The ATM's own bookkeeping: outstanding draws drop by the
        // flight's complement; the no-ledger mode ALSO refills its own
        // pool (the ledger mode's refill happens when the Campaign
        // books apply_mission_recovery — one booking site).
        for (auto& sq : squadrons_) {
            if (sq.vu != ft.squadron_vu) continue;
            sq.drawn_outstanding = std::max(
                0, sq.drawn_outstanding - ft.aircraft);
            if (ledger_ == nullptr) {
                sq.available += survivors;
            }
            break;
        }
    }
    booked_ = std::move(still);
    return out;
}

} // namespace f4::campaign
