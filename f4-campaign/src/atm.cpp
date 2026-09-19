// f4-campaign/src/atm.cpp
//
// AirTaskingManager — the 7-phase tasking pipeline (C4). See atm.hpp
// for the phase map, the FindBestAir term list, and the documented
// simplifications. This file is the mechanics.

#include <f4/campaign/atm.hpp>
#include <f4/campaign/ground_war.hpp>    // G2: the shared FLOT + ranking
#include <f4/campaign/naval_tasking.hpp>  // DOM-5: the naval target pool
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

// CAMP-ATM-1 — the ACTION tables' constants (the deterministic subset
// documented at ActionSystemType; the reference reads its real table
// from aiinput.dat values our sources cannot see):
//   the heavy-damage threshold that adds the garrison BARCAP to the
//   Defend filing, and the priority bonus that puts an ACTION filing
//   ahead of the routine ladder walk in the tempo budget (the
//   reference's ACTION filings outrank routine tasking).
constexpr int kActionHeavyDamagePct = 25;
constexpr int kActionPriorityBonus = 25;

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

/// G2 — resolve a target VU's grid position: objectives first, then —
/// only when `allow_units` (the unit_strike arm; keeps the flag-off
/// walk byte-identical for corner-case target ids that happen to
/// match a unit VU) — UNITS (an aggregate battalion, the CAS family's
/// target). The world loader's own resolution order. Returns false
/// when neither resolves.
bool resolve_target_xy(const f4::world::IObjectiveSource* objectives,
                        const f4::world::IUnitCoreSource& units,
                        bool allow_units,
                        std::uint32_t vu, int& tx, int& ty) {
    if (vu == 0) return false;
    if (objectives != nullptr) {
        for (int i = 0; i < objectives->objective_count(); ++i) {
            if (objectives->id_num(i) != vu) continue;
            tx = objectives->x(i);
            ty = objectives->y(i);
            return true;
        }
    }
    if (!allow_units) return false;
    for (int i = 0; i < units.unit_count(); ++i) {
        if (units.id_num(i) != vu) continue;
        tx = units.x(i);
        ty = units.y(i);
        return true;
    }
    return false;
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

void AirbaseSchedule::release(int minute, int aircraft, int plan_block_min,
                              int max_cycles) noexcept {
    if (minute < 0) return;
    const int horizon = max_cycles * plan_block_min;
    const auto clear = [&](int m) {
        if (m < 0 || m >= horizon) return;
        const int block = m / plan_block_min;
        const int slot = m % plan_block_min;
        blocks_[static_cast<std::size_t>(block)] &=
            static_cast<std::uint8_t>(~(0x01 << slot));
    };
    clear(minute);
    // The same slot in the next block — fill's fudge time, unmarked.
    clear(minute + plan_block_min);
    // Flights larger than 2 ships gave the next minute back too.
    if (aircraft > 2) {
        clear(minute + 1);
        clear(minute + 1 + plan_block_min);
    }
}

void AirbaseSchedule::sync(int now_min, int plan_block_min) noexcept {
    if (plan_block_min <= 0 || now_min < 0) return;
    // Block 0 should start at (or before) now, aligned to the block
    // grid — the epoch the wire seeds have been sliding against since
    // the campaign began.
    const int target = (now_min / plan_block_min) * plan_block_min;
    if (target <= epoch_min_) return;
    const int shift = (target - epoch_min_) / plan_block_min;
    if (shift >= static_cast<int>(blocks_.size())) {
        // Everything the grid held is past — the slide clears it.
        blocks_.fill(0);
        epoch_min_ = target;
        return;
    }
    for (int s = 0; s < shift; ++s) {
        for (std::size_t b = 0; b + 1 < blocks_.size(); ++b) {
            blocks_[b] = blocks_[b + 1];
        }
        blocks_.back() = 0;
    }
    epoch_min_ += shift * plan_block_min;
}

int AirbaseSchedule::booked() const noexcept {
    int bits = 0;
    for (const auto b : blocks_) {
        for (int s = 0; s < 8; ++s) {
            if ((b & (0x01 << s)) != 0) ++bits;
        }
    }
    return bits;
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
            // DOM-3 — the personnel seat: the unit source index (the
            // crew pick reads the roster through as_squadron) and the
            // wire's per-role ratings. The live decay view seeds from
            // the wire's own table when it carries one, else from the
            // UCD Scores (the same chain rating_ reads) — a squadron
            // with neither keeps the static specialty fallback, which
            // never decays (documented at rating_).
            st.unit_index = i;
            if (const auto* sq = units.as_squadron(i)) {
                st.wire_ratings = sq->role_ratings(i);
            }
            bool any_wire = false;
            bool any_ucd = false;
            for (const auto r : st.wire_ratings) {
                if (r != 0) any_wire = true;
            }
            for (const auto r : st.scores) {
                if (r != 0) any_ucd = true;
            }
            if (any_wire) {
                st.live_ratings = st.wire_ratings;
                st.ratings_live = true;
            } else if (any_ucd) {
                st.live_ratings = st.scores;
                st.ratings_live = true;
            }
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

AirbaseSchedule*
AirTaskingManager::find_schedule_(std::uint32_t airbase_vu) noexcept {
    for (auto& s : schedules_) {
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

void AirTaskingManager::draw_(SquadronState& sq, int count,
                              const std::vector<std::uint8_t>& crew) {
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
    // DOM-3 — the crew's slots go out (the pick-time out-set; the
    // recovery/scrub release brings them home).
    for (const auto slot : crew) {
        sq.crew_out_.push_back(slot);
    }
}

// ============================================================================
// DOM-3 — the personnel helpers (AssignPilots + rating decay)
// ============================================================================

std::vector<std::uint8_t> AirTaskingManager::free_pilot_slots_(
        const SquadronState& sq) const {
    // The pick's raw material: the wire roster's available pilots
    // (status 0 — PILOT_AVAILABLE) minus the LEDGER's dead deltas and
    // the ATM's own out-set. No ledger → deaths are not tracked (the
    // no-ledger mode's honest shape: the books ARE the ledger).
    std::vector<std::uint8_t> free;
    if (sq.unit_index < 0) return free;
    const auto* sqs = units_.as_squadron(sq.unit_index);
    if (sqs == nullptr) return free;
    const auto& roster = sqs->pilots(sq.unit_index);
    const auto n = static_cast<int>(roster.size());

    // The ledger's dead set for this squadron (a small delta vector —
    // first-touch order).
    const std::vector<PilotDelta>* deltas = nullptr;
    if (ledger_ != nullptr) deltas = ledger_->squadron_personnel(sq.vu);

    for (int i = 0; i < n; ++i) {
        const auto slot = static_cast<std::uint8_t>(i);
        if (roster[i].status != 0) continue;   // dead / leave / hospital
        bool out = false;
        for (const auto o : sq.crew_out_) {
            if (o == slot) { out = true; break; }
        }
        if (out) continue;
        if (deltas != nullptr) {
            for (const auto& d : *deltas) {
                if (d.slot == slot && d.dead) { out = true; break; }
            }
        }
        if (out) continue;
        free.push_back(slot);
    }
    return free;
}

std::vector<std::uint8_t> AirTaskingManager::pick_crew_(
        const SquadronState& sq, int aircraft) const {
    // FreeFalcon's AssignPilots (FlightClass::BuildMission's tail):
    // slot 0 scans the roster's FRONT THIRD for the first available
    // pilot (the commanders' seats); the remaining slots scan BACKWARD
    // from the tail (the wingmen); a slot with no pilot fails the
    // flight. The scan axis is the ROSTER ORDER (the wire's own 48
    // slots), gated on free-ness — the same walk the pick gate ran.
    std::vector<std::uint8_t> crew;
    if (!cfg_.pilot_assignment || aircraft <= 0) return crew;
    const auto free = free_pilot_slots_(sq);
    if (static_cast<int>(free.size()) < aircraft) return crew;

    if (sq.unit_index < 0) return crew;
    const auto* sqs = units_.as_squadron(sq.unit_index);
    if (sqs == nullptr) return crew;
    const int n = static_cast<int>(sqs->pilots(sq.unit_index).size());
    if (n == 0) return crew;

    const auto is_free = [&free](std::uint8_t slot) {
        for (const auto f : free) {
            if (f == slot) return true;
        }
        return false;
    };

    // The lead: the first free slot in the front third.
    const int third = std::max(1, n / 3);
    for (int i = 0; i < third; ++i) {
        const auto slot = static_cast<std::uint8_t>(i);
        if (is_free(slot)) {
            crew.push_back(slot);
            break;
        }
    }
    if (crew.empty()) return crew;   // no commander — the flight fails

    // The wingmen: backward from the tail.
    for (int i = n - 1;
         i >= 0 && static_cast<int>(crew.size()) < aircraft; --i) {
        const auto slot = static_cast<std::uint8_t>(i);
        if (is_free(slot)) crew.push_back(slot);
    }
    if (static_cast<int>(crew.size()) < aircraft) return {};
    return crew;
}

void AirTaskingManager::decay_rating_(SquadronState& sq,
                                      const MissionProfile& profile) {
    // The reference's post-assignment tuning row: new_rating =
    // (int)(0.75 × rating) + 1 — the ~25% hit spreads the wing's
    // sorties (FindBestAir's base score reads the same table). Integer
    // math, truncating (positive values: truncation == floor); the +1
    // floors the decay's fixed point at 4 — a rating never decays to
    // zero. The decayed view rides the flight (the Campaign syncs the
    // ledger — its write domain).
    if (!sq.ratings_live) return;
    const int idx = ref_aro_index(profile);
    if (idx < 0 || idx >= static_cast<int>(sq.live_ratings.size())) return;
    auto& r = sq.live_ratings[static_cast<std::size_t>(idx)];
    if (r == 0) return;   // no rating for the role — nothing to decay
    r = static_cast<std::uint8_t>((3 * r) / 4 + 1);
    ++stats_.ratings_decayed;
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
            // P7 — the request's own RoE (the wire roe_check byte,
            // carried verbatim; 0 = weapons free on every record that
            // predates the field).
            req.roe = wreq.roe_check;
            // CAMP-ATM-1 — the decoded ACTION bytes ride as telemetry
            // (seeded requests are never booked as THIS run's filings
            // — the save's own ACTION state is its history, not news).
            req.action_type = wreq.action_type;
            req.context = wreq.context;

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

    // P7 — the strategy layer's RequestEnemyMission filings (a strike
    // package's ADDBARCAP filed these for THIS team's last cycle's
    // compose). They ride ahead of the ladder walk — the defender
    // responds to the threat before its own routine tasking. A filing
    // whose window slipped gets one 30-minute push (the reference's
    // delay arithmetic, one push — these are fresh, not backlog
    // survivors).
    {
        auto& pending = pending_enemy_[team];
        for (auto& preq : pending) {
            if (preq.tot < now) preq.tot = now + 1800;
            out.push_back(std::move(preq));
        }
        pending.clear();
    }

    // CAMP-ATM-1 — the ACTION tables: scan the war's damage (own
    // objectives → Defend, enemy objectives → Punish) into this team's
    // pending queue, then drain the queue ahead of the ladder walk —
    // the war's reactions task before its routine. The scan is a
    // no-op disarmed (the golden identity never walks the objectives).
    if (cfg_.strategy && objectives_ != nullptr) {
        scan_actions_(team, now);
    }
    {
        auto& pending = pending_actions_[team];
        for (auto& preq : pending) {
            if (preq.tot < now) preq.tot = now + 1800;
            out.push_back(std::move(preq));
        }
        pending.clear();
    }

    // The profile ladder (the C3 walk, now emitting requests): wire
    // byte order, capability + mission-priority gating, deterministic
    // target rotation for the delivery family.
    const auto enemy = enemy_objectives_(team);

    // G2 — the interdiction family's target list: enemy battalions
    // front-line ranked (distance to the contested FLOT between the
    // belligerent pair), ledger-destroyed skipped. Computed only when
    // the unit_strike arm is on (the golden identity otherwise — the
    // ranking walk never runs, the requests never change shape).
    std::vector<std::uint32_t> unit_targets;
    if (cfg_.unit_strike && objectives_ != nullptr) {
        const auto pair = f4::campaign::belligerent_pair(teams_);
        if (pair.size() == 2) {
            const auto front =
                f4::campaign::front_columns_from_objectives(
                    f4::campaign::front_objective_view(*objectives_),
                    pair[0], pair[1]);
            unit_targets = f4::campaign::rank_battalion_targets(
                units_, teams_, front, team, ledger_);
        }
    }

    // DOM-5 — the naval family's target list: enemy TASK FORCES
    // (domain 4, the wire's own naval aggregate) ranked by distance to
    // the requesting team's own-held objectives (the fleet off the
    // coast is struck first), wire-order ties. Computed only when the
    // naval arm is on (the golden identity otherwise — the ranking
    // walk never runs, the anti-ship requests never change shape).
    std::vector<std::uint32_t> naval_targets;
    if (cfg_.naval_tasking && objectives_ != nullptr) {
        naval_targets = f4::campaign::rank_taskforce_targets(
            units_, teams_, *objectives_, team);
    }

    // P7 — the CAP family's station pool: the team's own objectives,
    // value-ranked (the defensive CAP orbit flies over what the team
    // values — the reference's strategy layer files its BARCAPs
    // against the same picture). Computed once per call, only when
    // the strategy arm is on (the golden identity never walks it).
    std::vector<std::uint32_t> own_stations;
    if (cfg_.strategy && objectives_ != nullptr) {
        own_stations = own_objectives_(team);
    }

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

        // G2 — the interdiction family: UNIT-targeted delivery profiles
        // (CAS) rotate across the front-line-ranked enemy battalions.
        // Off (or no ranked targets): target-less, exactly the C3
        // shape. A separate cursor — the two families' spreads stay
        // decoupled (one CAS package per cycle walks the line).
        if (cfg_.unit_strike && profile_flies_unit_delivery_route(profile)
                && !unit_targets.empty()) {
            const auto idx = static_cast<std::size_t>(
                                 unit_target_cursor_[team]) %
                             unit_targets.size();
            req.target_id = unit_targets[idx];
            unit_target_cursor_[team] =
                (unit_target_cursor_[team] + 1) %
                static_cast<int>(unit_targets.size());
        }

        // DOM-5 — the naval family (AMIS_ASHIP): the anti-ship mission
        // rotates across the ranked enemy task forces. Off (or no
        // ranked task forces): target-less, exactly the pre-DOM-5
        // shape (ASW's pool is the honest empty set — no submarines on
        // the wire — and TANK stays target-less, the ground pool's
        // business). A separate cursor — the families' spreads stay
        // decoupled (one naval package per cycle walks the coast).
        if (cfg_.naval_tasking && mission_is_naval_strike(req.mission)
                && !naval_targets.empty()) {
            const auto idx = static_cast<std::size_t>(
                                 naval_target_cursor_[team]) %
                             naval_targets.size();
            req.target_id = naval_targets[idx];
            naval_target_cursor_[team] =
                (naval_target_cursor_[team] + 1) %
                static_cast<int>(naval_targets.size());
            ++stats_.naval_requests;
        }

        // P7 — the CAP family: TPROF_LOITER + WP_CAP profiles (BARCAP,
        // TARCAP, ALERT...) station over a ranked OWN objective — the
        // rotation cursor spreads successive CAPs across the value
        // list (one station per request, deterministic). Off (or no
        // own objectives): target-less, exactly the C4 shape.
        if (cfg_.strategy && !own_stations.empty() &&
            profile.target_profile == "TPROF_LOITER" &&
            profile.targetwp == "WP_CAP") {
            const auto idx = static_cast<std::size_t>(
                                 station_cursor_[team]) %
                             own_stations.size();
            req.target_id = own_stations[idx];
            station_cursor_[team] =
                (station_cursor_[team] + 1) %
                static_cast<int>(own_stations.size());
            ++stats_.stations_targeted;
        }

        // CAMP-ATM-1 — the SWEEP family: the contested-air profile
        // (LOCATION-targeted TPROF_ATTACK, WP_SWEEP) finally gets a
        // real target — a ranked enemy objective, its own rotation
        // cursor (a sweep walks the enemy's territory, decoupled from
        // the strike rotation) — and the ACTION tag (kActionSweep).
        // Off (or no enemy objectives): target-less, exactly the
        // pre-ATM-1 shape (route-less — the C3 documented gap this
        // tranche closes under the arm).
        if (cfg_.strategy && !enemy.empty() &&
            profile_flies_sweep_line(profile)) {
            const auto idx = static_cast<std::size_t>(
                                 sweep_cursor_[team]) %
                             enemy.size();
            req.target_id = enemy[idx];
            sweep_cursor_[team] =
                (sweep_cursor_[team] + 1) %
                static_cast<int>(enemy.size());
            req.action_type = kActionSweep;
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
        // priority). G2: the target position resolves OBJECTIVES then
        // UNITS (a CAS package's battalion target reads the same
        // threat map — front-line battalions sit in the AD envelope).
        // The objectives_ gate keeps the null-source corner identical
        // to the pre-G2 walk (unit targets only exist with objectives
        // attached — generate_requests' own gate).
        bool need_sead = false;
        if (threat_ != nullptr && objectives_ != nullptr &&
            req.target_id != 0) {
            int tx = 0, ty = 0;
            if (resolve_target_xy(objectives_, units_,
                                  allow_unit_targets(),
                                  req.target_id, tx, ty)) {
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
        main.roe = req.roe;   // P7 — the request's RoE rides the flight

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

        // DOM-3 — the crew and the decay: the main flight's crew rides
        // the pick (the find_best_air_ gate guarantees it — the second
        // walk sees the same free slots), the draw claims the slots
        // into the out-set, and the role's rating decays once per
        // assignment (the reference's tuning row — sortie-spreading,
        // the rotation pressure).
        if (cfg_.pilot_assignment) {
            main.crew = pick_crew_(*main_pick.squadron, main.aircraft);
            if (!main.crew.empty()) ++stats_.crews_assigned;
        }
        draw_(*main_pick.squadron, main.aircraft, main.crew);
        if (cfg_.rating_decay) {
            decay_rating_(*main_pick.squadron, profile);
            if (main_pick.squadron->ratings_live) {
                main.squadron_ratings = main_pick.squadron->live_ratings;
                main.ratings_valid = true;
            }
        }

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

        // --- P7 — the strategy layer (all of it deterministic, all of
        // it behind cfg_.strategy) --------------------------------
        if (cfg_.strategy) {
            // FindSupportFlights: the ADDAWACS/ADDTANKER/ADDECM flags
            // share-or-file the support family over the package's
            // target area (AWACS/tanker/ECM racetrack stations). The
            // support-flight routes are the TPROF_LOITER racetracks —
            // the Campaign builds them per flight (role Support).
            file_support_flight_(flights, main, "ADDAWACS", "AMIS_AWACS",
                                 team, now, package_id);
            file_support_flight_(flights, main, "ADDTANKER", "AMIS_TANKER",
                                 team, now, package_id);
            file_support_flight_(flights, main, "ADDECM", "AMIS_ECM",
                                 team, now, package_id);

            // RequestEnemyMission: a delivery package over an enemy
            // objective prompts the DEFENDER — a BARCAP files for the
            // defender's next cycle (the pending queue).
            if (profile_flies_delivery_route(profile) &&
                req.target_id != 0) {
                file_enemy_barcap_(team, req.target_id, now);
            }
        }
    }
    return flights;
}

bool AirTaskingManager::build_support_flight_(
        FlightTasking& out, std::uint8_t team, CampaignTime now,
        std::uint32_t package_id, const FlightTasking& main,
        std::string_view support_name, int aircraft,
        const SquadronState* main_sq, std::uint32_t target_vu_override) {
    const auto& sprof = profiles_.for_name(support_name);

    // The support request: the package target (or the P7 station
    // override — the support filings orbit their OWN station), TOT =
    // main TOT + separation (the reference's separation arithmetic —
    // package.cpp's "newmis.tot = mis_request.tot + separation").
    const std::uint32_t station_vu =
        target_vu_override != 0 ? target_vu_override : main.target_vu;
    MissionRequest sreq;
    sreq.mission = sprof.mission_byte;
    sreq.team = team;
    sreq.target_id = station_vu;
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
    out.target_vu = station_vu;
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

    // DOM-3 — the support flight crews and decays exactly like the
    // main: the reference's AssignPilots runs per flight build and the
    // decay is per assignment, support filings included. The pick
    // precedes the draw (the claimed slots join the out-set).
    if (cfg_.pilot_assignment) {
        out.crew = pick_crew_(*pick.squadron, out.aircraft);
        if (!out.crew.empty()) ++stats_.crews_assigned;
    }
    draw_(*pick.squadron, out.aircraft, out.crew);
    if (cfg_.rating_decay) {
        decay_rating_(*pick.squadron, sprof);
        if (pick.squadron->ratings_live) {
            out.squadron_ratings = pick.squadron->live_ratings;
            out.ratings_valid = true;
        }
    }
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
    // G2: UNIT targets (CAS battalions) resolve through the units
    // source — objectives first, the loader's own order.
    int tx = 0, ty = 0;
    const bool have_target =
        resolve_target_xy(objectives_, units_, allow_unit_targets(),
                          req.target_id, tx, ty);

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
        // DOM-4 (the arm): the grid syncs to NOW first — the sliding
        // anchor makes the now-relative block index the grid's own —
        // and the reference's own rule applies: the start block OR the
        // previous one full denies the base (a base still launching the
        // previous block's queue cannot take this flight). The denial
        // is counted and queued for the ledger; disarmed, the single-
        // block skip stays exactly the pre-DOM-4 shape.
        if (const AirbaseSchedule* sched = airbase_schedule(sq.airbase)) {
            const CampaignTime to = req.tot - travel;
            const CampaignTime rel = to > now ? to - now : 0;
            const int block =
                static_cast<int>((rel / 60) / cfg_.plan_block_min);
            bool full = false;
            AirbaseSchedule* mut = nullptr;
            if (cfg_.airbase_scheduling) {
                mut = find_schedule_(sq.airbase);
                if (mut != nullptr) {
                    mut->sync(static_cast<int>(now / 60),
                              cfg_.plan_block_min);
                    full = block < cfg_.max_cycles &&
                           (mut->block_full(block, cfg_.plan_block_min) ||
                            (block > 0 &&
                             mut->block_full(block - 1,
                                             cfg_.plan_block_min)));
                }
            } else if (block < cfg_.max_cycles) {
                full = sched->block_full(block, cfg_.plan_block_min);
            }
            if (full) {
                if (cfg_.airbase_scheduling) {
                    ++stats_.schedule_denials;
                    if (mut != nullptr) mut->count_denial();
                    denials_.push_back(SlotDenial{req.team, sq.airbase,
                                                  kSlotDeniedPickFull});
                }
                continue;
            }
        }

        // DOM-3 — the crew gate (AssignPilots as a pick-time rule): a
        // squadron that cannot crew the request never enters the
        // comparison — the scored walk falls to the next-best, the
        // reference's flight-fails rule reshaped (its AssignPilots
        // aborts the flight INSIDE the chosen squadron's build; the
        // same squadrons fly, the request fills from the runner-up).
        // Counted when every other gate passed — the honest denial
        // number.
        if (cfg_.pilot_assignment) {
            const auto crew = pick_crew_(sq, req.aircraft);
            if (crew.empty()) {
                ++stats_.crew_denials;
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
    //
    // DOM-3 — the decay arm reads the LIVE view first: the seeded
    // table (wire rating[16], else the UCD Scores) decayed per
    // assignment. A live view entry of 0 falls through to the
    // existing chain (a role the tables never rated); a squadron
    // with no tables at all never decays (the fallback is the
    // fixture's own artifact, not a rating the wire carries).
    const int idx = ref_aro_index(profile);
    if (cfg_.rating_decay && sq.ratings_live &&
        idx >= 0 && idx < static_cast<int>(sq.live_ratings.size())) {
        const int live = sq.live_ratings[static_cast<std::size_t>(idx)];
        if (live != 0) return live;
    }
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

CampaignTime AirTaskingManager::schedule_takeoff(
    FlightTasking& flight, [[maybe_unused]] CampaignTime now) {
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

    // The requested takeoff minute. DOM-4 (the arm): the grid's anchor
    // syncs to the takeoff's block first — the slide keeps every
    // requested minute inside the grid no matter how late the war runs
    // (the epoch only moves forward, past bits fall off with their
    // time); disarmed, block 0 stays the campaign's start.
    const int minute =
        flight.takeoff > 0 ? static_cast<int>(flight.takeoff / 60) : 0;
    if (cfg_.airbase_scheduling) {
        sched->sync(minute, cfg_.plan_block_min);
    }
    const int rel_minute =
        cfg_.airbase_scheduling ? minute - sched->epoch_min() : minute;
    const int slot =
        sched->find_slot(rel_minute, cfg_.plan_block_min, cfg_.max_cycles);
    if (slot < 0) {
        // Horizon exhausted: keep the estimate (the reference cancels
        // at 0xFFFFFFFF; the QC's slot telemetry sees the unscheduled
        // flight — the same spirit as the route builder's documented
        // direct-fallback deviation). Still booked for recovery.
        // DOM-4 (the arm): the refusal is COUNTED and queued — the
        // saturated grid is a books fact, not a silent one.
        if (cfg_.airbase_scheduling) {
            ++stats_.slot_overflows;
            sched->count_overflow();
            denials_.push_back(SlotDenial{flight.team, flight.airbase_vu,
                                          kSlotDeniedHorizon});
        }
        booked_.push_back(flight);
        return 0;
    }

    // The snapped takeoff: the slot is grid-RELATIVE — the absolute
    // campaign minute rides the grid's anchor (epoch 0 disarmed = the
    // pre-DOM-4 arithmetic verbatim).
    const int abs_slot = slot + sched->epoch_min();
    const CampaignTime snapped =
        static_cast<CampaignTime>(abs_slot) * 60;
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
        // books apply_mission_recovery — one booking site). DOM-3: the
        // flight's crew comes home (the dead slots stay dead — the
        // ledger's books own that face).
        for (auto& sq : squadrons_) {
            if (sq.vu != ft.squadron_vu) continue;
            sq.drawn_outstanding = std::max(
                0, sq.drawn_outstanding - ft.aircraft);
            if (ledger_ == nullptr) {
                sq.available += survivors;
            }
            for (const auto slot : ft.crew) {
                std::erase(sq.crew_out_, slot);
            }
            break;
        }
    }
    booked_ = std::move(still);
    return out;
}

// ============================================================================
// CAMP-CMD-2 — the booked-flight interventions
// ============================================================================

std::optional<RecoveryRelease>
AirTaskingManager::scrub_flight(std::uint32_t flight_id, CampaignTime now) {
    for (std::size_t i = 0; i < booked_.size(); ++i) {
        if (booked_[i].flight_id != flight_id) continue;
        const FlightTasking ft = booked_[i];
        booked_.erase(booked_.begin() + static_cast<std::ptrdiff_t>(i));

        // DOM-4 (the arm): a scrubbed flight holds its takeoff slot no
        // longer — the still-future bits go back to the grid so the
        // next filing can take them. A past slot no-ops (its time is
        // gone; the slide already dropped it or no forward search can
        // reach it — clearing it could only invite a backward snap into
        // a departed minute).
        if (cfg_.airbase_scheduling && ft.takeoff > now &&
            ft.airbase_vu != 0) {
            if (AirbaseSchedule* sched = find_schedule_(ft.airbase_vu)) {
                const int minute = static_cast<int>(ft.takeoff / 60);
                if (minute >= sched->epoch_min()) {
                    sched->release(minute - sched->epoch_min(), ft.aircraft,
                                   cfg_.plan_block_min, cfg_.max_cycles);
                    ++stats_.slot_releases;
                }
            }
        }

        // Survivors: the recover_completed formula verbatim — drawn −
        // the flight's booked losses (the ledger's per-flight log; 0
        // when no ledger or no deaths).
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
        ++stats_.flights_scrubbed;
        stats_.aircraft_scrubbed += survivors;

        // The ATM's own bookkeeping, the recovery shape verbatim: the
        // outstanding draws drop by the flight's complement; the
        // no-ledger mode ALSO refills its own pool (the ledger mode's
        // refill happens when the caller books apply_mission_recovery —
        // one booking site, the same rule recover_completed obeys).
        // DOM-3: the scrubbed flight's crew comes home too.
        for (auto& sq : squadrons_) {
            if (sq.vu != ft.squadron_vu) continue;
            sq.drawn_outstanding = std::max(
                0, sq.drawn_outstanding - ft.aircraft);
            if (ledger_ == nullptr) {
                sq.available += survivors;
            }
            for (const auto slot : ft.crew) {
                std::erase(sq.crew_out_, slot);
            }
            break;
        }
        return rel;
    }
    return std::nullopt;
}

bool AirTaskingManager::reschedule_flight(std::uint32_t flight_id,
                                          std::uint8_t mission,
                                          std::uint32_t target_vu,
                                          CampaignTime tot,
                                          CampaignTime mission_over) {
    for (auto& ft : booked_) {
        if (ft.flight_id != flight_id) continue;
        ft.mission = mission;
        ft.target_vu = target_vu;
        ft.tot = tot;
        ft.mission_over = mission_over;
        return true;
    }
    return false;
}

// ============================================================================
// P7 — the strategy layer (own_objectives_ / nearest_own_objective_ /
//      file_enemy_barcap_ / file_support_flight_)
// ============================================================================

std::vector<std::uint32_t>
AirTaskingManager::own_objectives_(std::uint8_t team) const {
    // The team's own objectives, value-ranked: the same arithmetic the
    // target term of request_priority_ uses (objtype_priority/2 +
    // the objective's own priority scaling), stable in wire order.
    std::vector<std::uint32_t> out;
    if (objectives_ == nullptr) return out;
    struct Entry {
        std::uint32_t vu;
        int score;
    };
    std::vector<Entry> entries;
    for (int i = 0; i < objectives_->objective_count(); ++i) {
        if (objectives_->owner(i) != team) continue;
        int ot_prio = 0;
        for (int t = 0; t < teams_.team_count(); ++t) {
            if (teams_.slot(t) != static_cast<int>(team)) continue;
            ot_prio = teams_.objtype_priority(
                t, static_cast<int>(objectives_->objective_type(i)));
            break;
        }
        int score = ot_prio / 2;
        score += (score * static_cast<int>(objectives_->priority(i))) / 100;
        entries.push_back({objectives_->id_num(i), score});
    }
    std::stable_sort(entries.begin(), entries.end(),
                     [](const Entry& a, const Entry& b) {
                         return a.score > b.score;   // wire-order ties
                     });
    out.reserve(entries.size());
    for (const auto& e : entries) out.push_back(e.vu);
    return out;
}

std::uint32_t AirTaskingManager::nearest_own_objective_(
        std::uint8_t team, int x, int y) const {
    if (objectives_ == nullptr) return 0;
    std::uint32_t best_vu = 0;
    long long best_d2 = 0;
    for (int i = 0; i < objectives_->objective_count(); ++i) {
        if (objectives_->owner(i) != team) continue;
        const long long dx = objectives_->x(i) - x;
        const long long dy = objectives_->y(i) - y;
        const long long d2 = dx * dx + dy * dy;
        // Strict less-than keeps wire order on ties.
        if (best_vu == 0 || d2 < best_d2) {
            best_vu = objectives_->id_num(i);
            best_d2 = d2;
        }
    }
    return best_vu;
}

void AirTaskingManager::file_enemy_barcap_(
        std::uint8_t attacker_team, std::uint32_t target_vu,
        CampaignTime now) {
    // RequestEnemyMission's deterministic subset: the threatened
    // objective gets a DEFENDER BARCAP request for the other
    // belligerent's next cycle. Dedup (one identical pending) + cap
    // (max_pending_enemy_requests) keep the queue bounded; no
    // belligerent pair (peace / odd worlds) files nothing.
    const auto barcap = mission_type_byte("AMIS_BARCAP");
    if (!barcap || target_vu == 0 || objectives_ == nullptr) return;
    const auto pair = f4::campaign::belligerent_pair(teams_);
    if (pair.size() != 2) return;
    const std::uint8_t defender =
        pair[0] == attacker_team ? pair[1] : pair[0];
    if (defender >= 8) return;   // the pending queues are slot-indexed
    auto& queue = pending_enemy_[defender];
    for (const auto& r : queue) {
        if (r.mission == *barcap && r.target_id == target_vu) return;
    }
    if (static_cast<int>(queue.size()) >=
        std::max(1, cfg_.max_pending_enemy_requests)) {
        return;
    }
    const auto& profile = profiles_.for_mission(*barcap);
    MissionRequest req;
    req.mission = *barcap;
    req.team = defender;
    req.target_id = target_vu;
    req.enemy_filed = true;
    req.tot_type = TotType::LE;
    req.tot = now + static_cast<CampaignTime>(profile.min_time) * 60;
    req.priority = request_priority_(defender, profile, target_vu);
    req.aircraft = profile.str;
    queue.push_back(req);
    ++stats_.enemy_caps_filed;
}

// ============================================================================
// CAMP-ATM-1 — the ACTION tables (scan_actions_ / file_action_ /
//               objective_damage_pct_)
// ============================================================================

int AirTaskingManager::objective_damage_pct_(int index) const {
    // The fstatus bitmap: 2 bits per feature, 0 = intact, 1 = damaged,
    // 2 = destroyed, 3 = unknown (the reference's no-data nibble —
    // ignored). The effective feature count: the decoded count, or the
    // bitmap's own capacity when the save carries none (the kunsan
    // shape — real features, features_count 0).
    if (objectives_ == nullptr || !objectives_->has_fstatus(index)) {
        return 0;
    }
    const auto& fs = objectives_->fstatus(index);
    const int count = std::max<int>(
        objectives_->features_count(index), static_cast<int>(fs.size()) * 4);
    if (count <= 0) return 0;
    int destroyed = 0;
    bool any_damage = false;
    for (std::size_t b = 0; b < fs.size(); ++b) {
        for (int nib = 0; nib < 4; ++nib) {
            const int field = (fs[b] >> (nib * 2)) & 0x03;
            if (field == 1) any_damage = true;
            if (field == 2) {
                any_damage = true;
                ++destroyed;
            }
        }
    }
    if (!any_damage || destroyed == 0) return 0;
    return (destroyed * 100) / count;
}

bool AirTaskingManager::file_action_(
        std::uint8_t team, std::uint8_t mission, std::uint8_t action_type,
        std::uint8_t context, std::uint32_t objective_vu, int damage_pct,
        CampaignTime now) {
    if (objective_vu == 0 || objectives_ == nullptr) return false;
    if (team >= 8) return false;   // the pending queues are slot-indexed
    auto& queue = pending_actions_[team];
    // Dedup: one identical pending filing (the queue is drained each
    // cycle, so this is the within-scan guard — CAS and BARCAP for the
    // same objective are different missions and both file).
    for (const auto& r : queue) {
        if (r.mission == mission && r.target_id == objective_vu) {
            return false;
        }
    }
    // The service rule: a standing garrison does not re-file — a
    // booked flight of the same mission byte over the same target IS
    // the earlier filing, still flying (the reference's request queue
    // holds until serviced; ours holds until the package recovers).
    for (const auto& ft : booked_) {
        if (ft.mission == mission && ft.target_vu == objective_vu &&
            ft.team == team) {
            return false;
        }
    }
    if (static_cast<int>(queue.size()) >=
        std::max(1, cfg_.max_pending_action_requests)) {
        return false;
    }
    const auto& profile = profiles_.for_mission(mission);
    MissionRequest req;
    req.mission = mission;
    req.team = team;
    req.target_id = objective_vu;
    req.tot_type = TotType::LE;
    req.tot = now + static_cast<CampaignTime>(profile.min_time) * 60;
    req.priority =
        request_priority_(team, profile, objective_vu) + kActionPriorityBonus;
    req.aircraft = profile.str;
    req.action_type = action_type;
    req.context = context;
    req.damage_pct = damage_pct < 0 ? 0 : (damage_pct > 100 ? 100
                                                            : damage_pct);
    queue.push_back(req);
    ++stats_.actions_filed;
    // The ledger's booking happens at the CAMPAIGN (the one ledger
    // writer): the filing rides the drained request's ACTION bytes
    // (run_tasking_cycle_atm_ books apply_action_filing from them).
    // The counter here is the filing's own truth — the request was
    // filed even if the tempo budget later drops it.
    return true;
}

void AirTaskingManager::scan_actions_(std::uint8_t team, CampaignTime now) {
    // The ACTION tables' scan, wire order (deterministic — the filing
    // order IS the wire order of the objectives that drove them):
    //
    //   own objective damaged      → Defend: CAS over it; heavy damage
    //                                adds the garrison BARCAP station.
    //   enemy objective damaged    → Punish: SEADSTRIKE against it
    //                                (its defenses are alive — they
    //                                shot back).
    //
    // The context byte is the driving objective's own type byte (the
    // wire vocabulary — a consumer sees WHICH kind of site spawned the
    // filing). Missions resolve through the profile table by name —
    // data-driven, never a byte switch.
    const auto cas = mission_type_byte("AMIS_CAS");
    const auto barcap = mission_type_byte("AMIS_BARCAP");
    const auto sead = mission_type_byte("AMIS_SEADSTRIKE");
    if (!cas || !barcap || !sead) return;
    for (int i = 0; i < objectives_->objective_count(); ++i) {
        const int pct = objective_damage_pct_(i);
        if (pct <= 0) continue;
        const std::uint32_t vu = objectives_->id_num(i);
        const std::uint8_t owner = objectives_->owner(i);
        const std::uint8_t otype = objectives_->objective_type(i);
        if (owner == team) {
            // Defend — the owner helps its own ground defenders.
            (void)file_action_(team, *cas, kActionDefend, otype, vu, pct,
                               now);
            if (pct >= kActionHeavyDamagePct) {
                (void)file_action_(team, *barcap, kActionDefend, otype, vu,
                                   pct, now);
            }
        } else if (at_war_with(teams_, team, owner)) {
            // Punish — the enemy's defenses there drew blood; suppress.
            (void)file_action_(team, *sead, kActionPunish, otype, vu, pct,
                               now);
        }
    }
}

void AirTaskingManager::file_support_flight_(
        std::vector<FlightTasking>& flights, const FlightTasking& main,
        std::string_view flag_name, std::string_view support_name,
        std::uint8_t team, CampaignTime now, std::uint32_t package_id) {
    const auto& profile = profiles_.for_mission(main.mission);
    if (!profile.has_flag(flag_name)) return;
    const auto self_byte = mission_type_byte(support_name);
    if (!self_byte || main.mission == *self_byte) return;
    const auto& sprof = profiles_.for_mission(*self_byte);

    // The package's target area (the station pick measures against it).
    int px = 0, py = 0;
    const bool have_pkg = resolve_target_xy(
        objectives_, units_, allow_unit_targets(), main.target_vu, px, py);

    // The FILE half first (the share check needs the station): the
    // station is the own objective nearest the package target (the
    // orbit parks just behind the threatened area). No own territory —
    // nothing to orbit, nothing files.
    std::uint32_t station = 0;
    int stx = 0, sty = 0;
    if (have_pkg) station = nearest_own_objective_(team, px, py);
    if (station == 0) return;
    if (!resolve_target_xy(objectives_, units_, allow_unit_targets(),
                           station, stx, sty)) {
        return;   // unreachable — the station came from the objectives
    }

    // FindSupportFlights' SHARE half: any booked or on-cycle flight of
    // the same support byte, same team, STATION within the share
    // radius of THIS request's station, TOT inside the support window
    // (on station from its TOT through its loitertime, with a
    // 30-minute early allowance) COVERS the package — one tanker
    // feeds a whole raid, no new flight. The radius measures the two
    // ORBITS against each other (the stations serve the same target
    // area when they park together), not the orbit-to-target leg.
    int sx = 0, sy = 0;
    auto covers = [&](const FlightTasking& f) {
        if (f.mission != sprof.mission_byte || f.team != main.team) {
            return false;
        }
        if (main.tot < f.tot - 1800) return false;
        if (main.tot >
            f.tot + static_cast<CampaignTime>(sprof.loitertime) * 60) {
            return false;
        }
        if (!resolve_target_xy(objectives_, units_, allow_unit_targets(),
                               f.target_vu, sx, sy)) {
            return false;
        }
        const long long dx = sx - stx, dy = sy - sty;
        const long long r = cfg_.support_share_distance_grid;
        return dx * dx + dy * dy <= r * r;
    };
    for (const auto& f : booked_) {
        if (covers(f)) { ++stats_.supports_shared; return; }
    }
    for (const auto& f : flights) {
        if (covers(f)) { ++stats_.supports_shared; return; }
    }

    FlightTasking sup;
    if (!build_support_flight_(sup, team, now, package_id, main,
                               support_name, sprof.str,
                               /*main_sq=*/nullptr, station)) {
        return;   // nobody can fly it — the package still flies
    }
    sup.role = FlightRole::Support;   // build_ defaults Escort
    sup.escorted_flight_id = 0;       // a station, not an escort
    flights.push_back(sup);
    ++stats_.supports_filed;

    // The support profile's own fighter escort (AWACS/JSTAR/TANKER
    // carry ADDESCORT — the big, slow orbit gets its cover). The
    // escort links to the support flight (build_ sets it) and its
    // own FindBestAir has no package-lead preference either.
    if (sprof.has_flag("ADDESCORT")) {
        const auto escort_byte = sprof.escort_type != 0
                                     ? sprof.escort_type
                                     : mission_type_byte("AMIS_ESCORT")
                                           .value_or(10);
        FlightTasking esc;
        if (build_support_flight_(esc, team, now, package_id, sup,
                                  mission_type_name(escort_byte),
                                  profiles_.for_mission(escort_byte).str,
                                  nullptr)) {
            esc.role = FlightRole::Escort;
            flights.push_back(esc);
            ++stats_.escorts_built;
        }
    }
}

} // namespace f4::campaign
