// f4-campaign/src/ground_war.cpp
//
// GroundWar implementation — see ground_war.hpp for the tranche
// rationale, the FreeFalcon correspondence, and the phase contracts.
//
// Determinism discipline (the whole file): NO RNG, NO wall clocks, NO
// iteration over unordered containers. Every walk is over wire-order
// vectors or row-major bucket cells; every arithmetic path is integer
// fixed-point (positions ×256, attrition ×256) except the two libm
// calls in the movement phase (sqrt for the step normalization,
// atan2 for the heading byte) — both pure functions of integer
// inputs, both exercised identically in every run, and neither value
// ever reaches the ledger document (heading is quantized to the
// wire's own byte).

#include <f4/campaign/ground_war.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace f4::campaign {

namespace {

// ---------------------------------------------------------------------------
// Constants — the wire's own vocabulary, referenced by value the same
// way threat_map.cpp references it (f4-campaign deliberately does not
// depend on f4-world-convert's enum headers).
// ---------------------------------------------------------------------------

/// Land-domain unit subtypes (classtbl.h STYPE_LAND_*).
constexpr std::uint8_t kStAirDefense = 1;
constexpr std::uint8_t kStAirmobile = 2;
constexpr std::uint8_t kStArmor = 3;
constexpr std::uint8_t kStArmoredCav = 4;
constexpr std::uint8_t kStEngineer = 5;
constexpr std::uint8_t kStHq = 6;
constexpr std::uint8_t kStInfantry = 7;
constexpr std::uint8_t kStMarine = 8;
constexpr std::uint8_t kStMechanized = 9;
constexpr std::uint8_t kStRocket = 10;
constexpr std::uint8_t kStSpArtillery = 11;
constexpr std::uint8_t kStSsMissile = 12;
constexpr std::uint8_t kStSupply = 13;
constexpr std::uint8_t kStTowedArtillery = 14;

/// DOMAIN_LAND.
constexpr std::uint8_t kDomainLand = 3;

/// Spatial bucket ratio (the threat map's own MAP_RATIO: 6 grid units
/// per cell). Contact range (2 grid) is well inside one cell, so the
/// 3x3 neighborhood around a unit's cell covers every possible pair.
constexpr int kGroundBucketRatio = 6;

/// Front-line column band: objectives within ±3 columns of x belong to
/// column x's front computation.
constexpr int kFrontBand = 3;

/// Default movement speeds by subtype family (kph), used when the wire
/// carries no UCD movement_speed (the fixture UCD is an 8-entry sample;
/// most saves carry no enrichment). Cross-country rates in the
/// reference's own scale (a mech battalion does not outpace its
/// support train).
int default_speed_kph(std::uint8_t st) noexcept {
    switch (st) {
        case kStArmor:         return 25;
        case kStArmoredCav:    return 30;
        case kStMechanized:    return 25;
        case kStInfantry:      return 5;
        case kStMarine:        return 8;
        case kStAirmobile:     return 40;
        case kStEngineer:      return 15;
        case kStHq:            return 20;
        case kStSpArtillery:   return 20;
        case kStRocket:        return 15;
        case kStTowedArtillery: return 10;
        default:               return 0;   // static: AD / SS / supply
    }
}

/// The capture ladder's mobility test (the reference's GTM class
/// distinction: AirDefense / Support / SS batteries never join the
/// offensive; artillery trails the line).
bool subtype_mobile(std::uint8_t st) noexcept {
    switch (st) {
        case kStArmor: case kStArmoredCav: case kStMechanized:
        case kStInfantry: case kStMarine: case kStAirmobile:
        case kStEngineer: case kStHq:
            return true;
        default:
            return false;
    }
}

bool subtype_artillery(std::uint8_t st) noexcept {
    return st == kStSpArtillery || st == kStRocket ||
           st == kStTowedArtillery;
}

/// Decode a battalion's vehicle count from the wire's 2-bit group
/// packing (GetNumVehicles port: sum of the 16 2-bit group counts).
int roster_strength(std::uint32_t roster) noexcept {
    int n = 0;
    for (int g = 0; g < 16; ++g) {
        n += static_cast<int>((roster >> (g * 2)) & 0x03);
    }
    return n;
}

/// Fixed-point helpers: positions live in 1/256 grid units.
constexpr int kFpOne = 256;

/// Chebyshev grid distance between a unit and an objective (integer,
/// deterministic — the movement/capture/garrison "at" test).
int chebyshev(std::int32_t ax, std::int32_t ay,
              std::int32_t bx, std::int32_t by) noexcept {
    const int dx = std::abs(ax - bx);
    const int dy = std::abs(ay - by);
    return dx > dy ? dx : dy;
}

/// Integer clamp.
int clamp_i(int v, int lo, int hi) noexcept {
    return v < lo ? lo : (v > hi ? hi : v);
}

} // namespace

// ============================================================================
// Construction — the snapshot
// ============================================================================

GroundWar::GroundWar(const f4::world::ICampaignSource& camp,
                     const f4::world::ITeamSource& teams,
                     const f4::world::IObjectiveSource& objectives,
                     const f4::world::IUnitCoreSource& units,
                     CampaignResultLedger* ledger,
                     const GroundWarConfig& cfg)
        : cfg_(cfg), ledger_(ledger) {
    // --- The war pair (Campaign::belligerent_teams' own rule: named
    // slots, RelType::War row toward another named slot, garbage wire
    // values decode NoRelations; first at-war pair in slot order).
    stance_by_slot_.assign(8, {});
    struct NamedSlot {
        int slot;
        const std::vector<int16_t>* stance;
    };
    std::vector<NamedSlot> named;
    for (int t = 0; t < teams.team_count() && t < 8; ++t) {
        const int slot = teams.slot(t);
        if (slot < 0 || slot >= 8) continue;
        if (teams.name(t).empty()) continue;   // unnamed = not a side
        stance_by_slot_[static_cast<std::size_t>(slot)] = teams.stance(t);
        named.push_back({slot, &teams.stance(t)});
    }
    for (std::size_t a = 0; a < named.size() && war_pair_.empty(); ++a) {
        for (std::size_t b = a + 1; b < named.size(); ++b) {
            const auto& st = *named[a].stance;
            const int other = named[b].slot;
            if (other >= static_cast<int>(st.size())) continue;
            if (f4::world::relation_from_wire(
                    st[static_cast<std::size_t>(other)]) ==
                f4::world::Relation::War) {
                war_pair_.push_back(
                    static_cast<std::uint8_t>(named[a].slot));
                war_pair_.push_back(
                    static_cast<std::uint8_t>(named[b].slot));
                break;
            }
        }
    }

    // --- The epoch + the resupply anchor (absolute campaign times,
    // the reinforcement cadence's own bridge through ICampaignSource).
    epoch_ = camp.current_time();
    last_resupply_ = camp.last_resupply();

    // --- The battalion snapshot (wire order preserved).
    const int precompute_fp = static_cast<int>(cfg_.update_sec) > 0
        ? static_cast<int>(cfg_.update_sec) : 1;
    for (int i = 0; i < units.unit_count(); ++i) {
        if (units.unit_class(i) != f4::entities::UnitClass::Battalion) {
            continue;
        }
        if (units.domain(i) != kDomainLand) continue;

        GroundUnitState u;
        u.vu = units.id_num(i);
        u.owner = units.owner(i);
        u.subtype = units.unit_subtype(i);
        u.x = units.x(i);
        u.y = units.y(i);
        u.dest_x = units.dest_x(i);
        u.dest_y = units.dest_y(i);
        u.roster = units.roster(i);
        u.strength = roster_strength(u.roster);
        u.strength_initial = u.strength;

        const auto* gu = units.as_ground_unit(i);
        if (gu != nullptr) {
            u.supply = gu->supply(i);
            u.morale = gu->morale(i);
            u.fatigue = gu->fatigue(i);
            u.last_move = gu->last_move(i);
            u.last_combat = gu->last_combat(i);
            u.heading = gu->heading(i);
        }

        u.mobile = subtype_mobile(u.subtype);
        u.artillery = subtype_artillery(u.subtype);
        u.speed_kph = units.movement_speed(i) > 0
            ? units.movement_speed(i)
            : default_speed_kph(u.subtype);
        if (!u.mobile) u.speed_kph = 0;
        if (u.artillery) u.speed_kph = u.speed_kph / 2;
        // Per-update movement step, grid units × 1/256.
        u.step_fp = static_cast<int>(
            (static_cast<std::int64_t>(u.speed_kph) * kFpOne *
             precompute_fp) / 3600);

        units_.push_back(u);
        unit_vus_.push_back(u.vu);
    }

    // --- The objective mirror (wire order preserved).
    min_x_ = 0;
    max_x_ = -1;
    for (int i = 0; i < objectives.objective_count(); ++i) {
        GroundObjectiveState o;
        o.vu = objectives.id_num(i);
        o.x = objectives.x(i);
        o.y = objectives.y(i);
        o.owner = objectives.owner(i);
        o.initial_owner = o.owner;
        o.priority = objectives.priority(i);
        objectives_.push_back(o);
        if (max_x_ < min_x_) { min_x_ = o.x; max_x_ = o.x; }
        else {
            min_x_ = std::min(min_x_, static_cast<std::int32_t>(o.x));
            max_x_ = std::max(max_x_, static_cast<std::int32_t>(o.x));
        }
    }

    stats_.battalions_alive = static_cast<int>(units_.size());
    stats_.battalions_mobile = 0;
    for (const auto& u : units_) {
        if (u.mobile && !u.destroyed) ++stats_.battalions_mobile;
    }
}

// ============================================================================
// tick — the update loop
// ============================================================================

void GroundWar::tick(CampaignTime delta_sec) {
    if (delta_sec <= 0) return;
    if (war_pair_.empty()) {
        // No war pair: the engine is deliberately inert (pinned by
        // test). The clock still advances so attach-time queries agree
        // with the ladder's.
        clock_ += delta_sec;
        return;
    }
    if (cfg_.update_sec <= 0) {
        clock_ += delta_sec;
        return;
    }

    clock_ += delta_sec;
    while (clock_ >= next_update_) {
        // Orders due? (The first cycle fires at clock 0 — the war
        // starts by planning, the ladder's own first-cycle rule.)
        while (next_orders_ <= next_update_) {
            fire_orders_();
            next_orders_ += std::max<CampaignTime>(1, cfg_.orders_sec);
        }

        move_phase_();
        engage_phase_();
        capture_phase_();
        resupply_phase_(next_update_);
        pull_air_losses_();
        sync_ledger_();

        ++stats_.updates;
        next_update_ += cfg_.update_sec;

        stats_.battalions_alive = 0;
        stats_.battalions_mobile = 0;
        for (const auto& u : units_) {
            if (u.destroyed) continue;
            ++stats_.battalions_alive;
            if (u.mobile) ++stats_.battalions_mobile;
        }
    }
}

// ============================================================================
// Orders (the GTM-lite) — score the enemy's objectives, assign mobile
// battalions, garrison holds.
// ============================================================================

void GroundWar::fire_orders_() {
    if (war_pair_.size() < 2) return;

    rebuild_front_();

    for (std::size_t side = 0; side < 2; ++side) {
        const std::uint8_t us = war_pair_[side];
        const std::uint8_t them = war_pair_[1 - side];

        // Candidates: the ENEMY's objectives, scored. Deterministic
        // order: score descending, then wire order.
        struct Candidate {
            const GroundObjectiveState* obj;
            int score;
        };
        std::vector<Candidate> candidates;
        candidates.reserve(objectives_.size() / 8);
        for (const auto& o : objectives_) {
            if (o.owner != them) continue;
            candidates.push_back({&o, objective_score_(o)});
        }
        std::stable_sort(candidates.begin(), candidates.end(),
                         [](const Candidate& a, const Candidate& b) {
                             return a.score > b.score;
                         });

        // Open garrison slots per candidate.
        std::vector<int> slots(candidates.size(), cfg_.objective_garrison);

        for (std::size_t ui = 0; ui < units_.size(); ++ui) {
            GroundUnitState& u = units_[ui];
            if (u.destroyed || !u.mobile || u.owner != us) continue;

            // Garrison hold: sitting on an OWN-held objective — keep
            // the prize (the capture rule's own doctrine).
            if (u.target != 0) {
                const auto held = std::find_if(
                    objectives_.begin(), objectives_.end(),
                    [&u](const GroundObjectiveState& o) {
                        return o.vu == u.target;
                    });
                if (held != objectives_.end() && held->owner == us &&
                    chebyshev(u.x, u.y, held->x, held->y) <=
                        cfg_.capture_range_grid) {
                    continue;
                }
            }

            // Pick the best candidate: objective score − distance/2,
            // open slot required. First match in the sorted order wins
            // ties (stable). The sentinel is a large negative, NOT -1:
            // distant objectives score NEGATIVE (score − distance/2),
            // and an army that "holds position" because every target
            // is 300 grid away is a stalled war, not a quiet one —
            // distance RANKS candidates, it never vetoes them.
            int best = -1;
            int best_score = -1'000'000;
            for (std::size_t c = 0; c < candidates.size(); ++c) {
                if (slots[c] <= 0) continue;
                const int pair_score = candidates[c].score -
                    distance_(u, *candidates[c].obj) / 2;
                if (pair_score > best_score) {
                    best_score = pair_score;
                    best = static_cast<int>(c);
                }
            }
            if (best < 0) {
                // Every objective garrisoned: hold position.
                continue;
            }
            const auto& obj = *candidates[static_cast<std::size_t>(best)].obj;
            u.target = obj.vu;
            u.dest_x = obj.x;
            u.dest_y = obj.y;
            --slots[static_cast<std::size_t>(best)];
        }
    }

    ++stats_.orders_fired;
}

// ============================================================================
// The front line — per column, between the sides' forward holdings
// ============================================================================

std::vector<FrontObjectiveView>
front_objective_view(const f4::world::IObjectiveSource& objectives) {
    std::vector<FrontObjectiveView> out;
    out.reserve(static_cast<std::size_t>(objectives.objective_count()));
    for (int i = 0; i < objectives.objective_count(); ++i) {
        FrontObjectiveView v;
        v.x = objectives.x(i);
        v.y = objectives.y(i);
        v.owner = objectives.owner(i);
        out.push_back(v);
    }
    return out;
}

std::vector<FrontColumn>
front_columns_from_objectives(
    const std::vector<FrontObjectiveView>& objectives,
    std::uint8_t side_a, std::uint8_t side_b) {
    std::vector<FrontColumn> front;
    if (side_a == side_b) return front;

    // The columns' x range over ALL objectives (the engine's own
    // min_x_..max_x_ snapshot rule — every column in the theater's
    // objective span, not just the contested ones).
    std::int32_t min_x = 0;
    std::int32_t max_x = -1;
    for (const auto& o : objectives) {
        if (max_x < min_x) { min_x = o.x; max_x = o.x; }
        else {
            min_x = std::min(min_x, o.x);
            max_x = std::max(max_x, o.x);
        }
    }
    if (max_x < min_x) return front;

    // Sides by territory centroid (deterministic): the side whose
    // held-objective mean y is smaller holds the south.
    const auto centroid_y = [&](std::uint8_t team) -> double {
        std::int64_t sum = 0;
        int n = 0;
        for (const auto& o : objectives) {
            if (o.owner != team) continue;
            sum += o.y;
            ++n;
        }
        return n == 0 ? 0.0 : static_cast<double>(sum) / n;
    };
    const std::uint8_t south = centroid_y(side_a) <= centroid_y(side_b)
        ? side_a : side_b;
    const std::uint8_t north = south == side_a ? side_b : side_a;

    for (std::int32_t x = min_x; x <= max_x; ++x) {
        bool have_s = false, have_n = false;
        std::int32_t s_fwd = 0, n_fwd = 0;
        for (const auto& o : objectives) {
            if (std::abs(o.x - x) > kFrontBand) continue;
            if (o.owner == south) {
                if (!have_s || o.y > s_fwd) { s_fwd = o.y; have_s = true; }
            } else if (o.owner == north) {
                if (!have_n || o.y < n_fwd) { n_fwd = o.y; have_n = true; }
            }
        }
        FrontColumn col;
        col.x = x;
        col.south_owner = south;
        col.north_owner = north;
        if (have_s && have_n) {
            col.y = (s_fwd + n_fwd) / 2;
            col.contested = true;
        }
        front.push_back(col);
    }
    return front;
}

std::vector<std::uint8_t>
belligerent_pair(const f4::world::ITeamSource& teams) {
    // The engine's own rule (the GroundWar ctor's derivation, extracted
    // so the tasking side shares it): named slots in slot order, the
    // first pair whose stance row toward the other is RelType::War.
    // Garbage wire values decode NoRelations, so phantom slots can
    // never pair up.
    struct NamedSlot {
        int slot;
        const std::vector<int16_t>* stance;
    };
    std::vector<NamedSlot> named;
    for (int t = 0; t < teams.team_count() && t < 8; ++t) {
        const int slot = teams.slot(t);
        if (slot < 0 || slot >= 8) continue;
        if (teams.name(t).empty()) continue;   // unnamed = not a side
        named.push_back({slot, &teams.stance(t)});
    }
    for (std::size_t a = 0; a < named.size(); ++a) {
        for (std::size_t b = a + 1; b < named.size(); ++b) {
            const auto& st = *named[a].stance;
            const int other = named[b].slot;
            if (other >= static_cast<int>(st.size())) continue;
            if (f4::world::relation_from_wire(
                    st[static_cast<std::size_t>(other)]) ==
                f4::world::Relation::War) {
                return {
                    static_cast<std::uint8_t>(named[a].slot),
                    static_cast<std::uint8_t>(named[b].slot)};
            }
        }
    }
    return {};
}

std::vector<std::uint32_t>
rank_battalion_targets(const f4::world::IUnitCoreSource& units,
                       const f4::world::ITeamSource& teams,
                       const std::vector<FrontColumn>& front,
                       std::uint8_t team,
                       const CampaignResultLedger* ledger) {
    // The symmetric belligerence rule (the same walk
    // Campaign::select_target_ / at_war_with keep — each module owns
    // its stance walk, the shared vocabulary is the rule).
    const auto at_war = [&](std::uint8_t other) {
        if (other == team) return false;
        for (int t = 0; t < teams.team_count(); ++t) {
            if (teams.slot(t) != static_cast<int>(other)) continue;
            const auto& row = teams.stance(t);
            const auto idx = static_cast<std::size_t>(team);
            if (idx < row.size() &&
                f4::world::relation_from_wire(row[idx]) ==
                    f4::world::Relation::War)
                return true;
        }
        for (int t = 0; t < teams.team_count(); ++t) {
            if (teams.slot(t) != static_cast<int>(team)) continue;
            const auto& row = teams.stance(t);
            const auto idx = static_cast<std::size_t>(other);
            if (idx < row.size() &&
                f4::world::relation_from_wire(row[idx]) ==
                    f4::world::Relation::War)
                return true;
        }
        return false;
    };

    // Squared distance to the nearest CONTESTED column (squared ranks
    // identically to euclidean — no sqrt, all integer).
    const auto front_dist2 = [&](std::int32_t x, std::int32_t y) {
        std::int64_t best = -1;
        for (const auto& col : front) {
            if (!col.contested) continue;
            const std::int64_t dx = x - col.x;
            const std::int64_t dy = y - col.y;
            const std::int64_t d2 = dx * dx + dy * dy;
            if (best < 0 || d2 < best) best = d2;
        }
        return best;   // -1: no contested columns at all
    };

    // Rank: front distance ascending, wire order breaking ties
    // (std::sort is not stable — the key folds the wire index in as
    // the tiebreak term of a pair, so the distance always dominates).
    // No contested columns at all: distance INT64_MAX (every candidate
    // ties, wire order decides — the honest degenerate case).
    std::vector<std::pair<std::pair<std::int64_t, int>,
                          std::uint32_t>> keyed;
    for (int i = 0; i < units.unit_count(); ++i) {
        if (units.domain(i) != 3 /* land */) continue;
        if (units.unit_class(i) !=
            f4::entities::UnitClass::Battalion) continue;
        const std::uint8_t owner = units.owner(i);
        if (!at_war(owner)) continue;
        if (units.roster(i) == 0) continue;
        const std::uint32_t vu = units.id_num(i);
        if (vu == 0) continue;
        if (ledger != nullptr) {
            const auto* rec = ledger->ground_unit(vu);
            if (rec != nullptr && rec->destroyed) continue;
        }
        const std::int64_t d2 =
            front_dist2(units.x(i), units.y(i));
        keyed.emplace_back(
            std::make_pair(d2 < 0
                               ? std::numeric_limits<std::int64_t>::max()
                               : d2,
                           i),
            vu);
    }
    std::sort(keyed.begin(), keyed.end());
    std::vector<std::uint32_t> out;
    out.reserve(keyed.size());
    for (const auto& k : keyed) out.push_back(k.second);
    return out;
}

void GroundWar::rebuild_front_() {
    front_.clear();
    stats_.front_columns = 0;
    stats_.front_mean_y_fp = 0;
    if (war_pair_.size() < 2) return;

    // G2: the shared FLOT math (front_columns_from_objectives) — this
    // engine used to own the only copy; the tasking side now needs the
    // same columns, so the computation lives once in the shared helper
    // and BOTH call it. The engine's LIVE objective mirror (captures
    // flip owners) is the projection here — the front still moves.
    // min_x_..max_x_ was snapshotted at construction over the same
    // objectives list, so the column span is identical by construction.
    std::vector<FrontObjectiveView> view;
    view.reserve(objectives_.size());
    for (const auto& o : objectives_) {
        view.push_back(FrontObjectiveView{o.x, o.y, o.owner});
    }
    front_ = front_columns_from_objectives(
        view, war_pair_[0], war_pair_[1]);

    std::int64_t sum_y = 0;
    for (const auto& col : front_) {
        if (!col.contested) continue;
        ++stats_.front_columns;
        sum_y += col.y;
    }
    stats_.front_mean_y_fp = stats_.front_columns > 0
        ? (sum_y * kFpOne) / stats_.front_columns
        : 0;
}

// ============================================================================
// Movement — mobile battalions walk toward their targets
// ============================================================================

void GroundWar::move_phase_() {
    bool any_moved = false;
    const std::int64_t now_abs = epoch_ + next_update_;

    for (auto& u : units_) {
        if (u.destroyed || !u.mobile || u.target == 0) continue;
        if (u.pinned) {
            // In contact: no advance (the reference's movement gate).
            // The flag stays until the next engage phase clears it.
            continue;
        }
        if (u.step_fp <= 0) continue;

        // Fatigue and supply gates (speed halves past each).
        int step = u.step_fp;
        if (u.supply < 25) step /= 2;
        if (u.fatigue > 75) step /= 2;
        if (step <= 0) step = 1;

        // Fixed-point position → target vector.
        const std::int64_t px = static_cast<std::int64_t>(u.x) * kFpOne + u.fx;
        const std::int64_t py = static_cast<std::int64_t>(u.y) * kFpOne + u.fy;
        const std::int64_t tx = static_cast<std::int64_t>(u.dest_x) * kFpOne;
        const std::int64_t ty = static_cast<std::int64_t>(u.dest_y) * kFpOne;
        const std::int64_t dx = tx - px;
        const std::int64_t dy = ty - py;
        const std::int64_t len = static_cast<std::int64_t>(
            std::sqrt(static_cast<double>(dx * dx + dy * dy)));
        if (len <= 0) continue;

        if (step >= len) {
            // Arrive: snap to the objective.
            u.x = u.dest_x;
            u.y = u.dest_y;
            u.fx = 0;
            u.fy = 0;
            stats_.army_distance_fp += static_cast<std::uint64_t>(len);
        } else {
            // Advance along the normalized vector (integer truncation —
            // deterministic; the lost fraction stays behind, exactly
            // like the wire's own position byte carries the remainder).
            const std::int64_t nx = px + (dx * step) / len;
            const std::int64_t ny = py + (dy * step) / len;
            u.x = static_cast<std::int32_t>(nx / kFpOne);
            u.y = static_cast<std::int32_t>(ny / kFpOne);
            u.fx = static_cast<std::int32_t>(nx % kFpOne);
            u.fy = static_cast<std::int32_t>(ny % kFpOne);
            stats_.army_distance_fp += static_cast<std::uint64_t>(step);
        }

        // Heading (the wire's byte convention: 0-255, ×1.40625 deg,
        // 0 = north).
        const double hdg_deg = std::atan2(
            static_cast<double>(dx), static_cast<double>(dy)) *
            (180.0 / 3.14159265358979323846);
        int hb = static_cast<int>(std::lround(hdg_deg / 1.40625)) % 256;
        if (hb < 0) hb += 256;
        u.heading = static_cast<std::uint8_t>(hb);

        u.last_move = now_abs;
        u.dirty = true;
        any_moved = true;
        ++u.move_ticks;
        // Fatigue accrues every 5 updates of movement; supply burns
        // every 30 (a day-long approach march tires and thins out).
        if (u.move_ticks % 5 == 0 && u.fatigue < 100) ++u.fatigue;
        if (u.move_ticks % 30 == 0 && u.supply > 0) --u.supply;
    }

    if (any_moved) ++stats_.moved_events;
}

// ============================================================================
// Engagement — bucketed contact detection + the exchange
// ============================================================================

void GroundWar::rebuild_buckets_() {
    // Extent over units AND objectives (the capture scan probes
    // buckets around objective cells).
    std::int32_t x0 = min_x_, x1 = max_x_;
    std::int32_t y0 = 0, y1 = -1;
    bool any = false;
    for (const auto& o : objectives_) {
        if (!any) { x0 = o.x; x1 = o.x; y0 = o.y; y1 = o.y; any = true; }
        else {
            x0 = std::min(x0, static_cast<std::int32_t>(o.x));
            x1 = std::max(x1, static_cast<std::int32_t>(o.x));
            y0 = std::min(y0, static_cast<std::int32_t>(o.y));
            y1 = std::max(y1, static_cast<std::int32_t>(o.y));
        }
    }
    for (const auto& u : units_) {
        if (!any) { x0 = u.x; x1 = u.x; y0 = u.y; y1 = u.y; any = true; }
        else {
            x0 = std::min(x0, u.x);
            x1 = std::max(x1, u.x);
            y0 = std::min(y0, u.y);
            y1 = std::max(y1, u.y);
        }
    }
    if (!any) { buckets_.clear(); bucket_cols_ = 0; bucket_rows_ = 0; return; }

    bucket_x0_ = x0 - kGroundBucketRatio;
    bucket_y0_ = y0 - kGroundBucketRatio;
    bucket_cols_ = (x1 - bucket_x0_) / kGroundBucketRatio + 1;
    bucket_rows_ = (y1 - bucket_y0_) / kGroundBucketRatio + 1;
    buckets_.assign(static_cast<std::size_t>(bucket_cols_) * bucket_rows_,
                    {});
    for (int i = 0; i < static_cast<int>(units_.size()); ++i) {
        const auto& u = units_[static_cast<std::size_t>(i)];
        if (u.destroyed) continue;
        buckets_[static_cast<std::size_t>(
            bucket_index_(u.x, u.y))].push_back(i);
    }
}

int GroundWar::bucket_index_(std::int32_t gx, std::int32_t gy) const {
    int cx = (gx - bucket_x0_) / kGroundBucketRatio;
    int cy = (gy - bucket_y0_) / kGroundBucketRatio;
    cx = clamp_i(cx, 0, bucket_cols_ - 1);
    cy = clamp_i(cy, 0, bucket_rows_ - 1);
    return cy * bucket_cols_ + cx;
}

void GroundWar::engage_phase_() {
    // Pinned flags from the previous update clear now — this phase
    // re-marks whoever is still in contact.
    for (auto& u : units_) u.pinned = false;

    rebuild_buckets_();

    // Contact detection: for each battalion (wire order), scan the 3x3
    // bucket neighborhood (row-major), pair with later-index hostile
    // battalions inside the contact range. Every pair recorded once.
    const std::int64_t range2 = static_cast<std::int64_t>(
        cfg_.contact_range_grid * kFpOne);
    const std::int64_t range2_sq = range2 * range2;
    engaged_a_.clear();
    engaged_b_.clear();
    for (int i = 0; i < static_cast<int>(units_.size()); ++i) {
        const auto& a = units_[static_cast<std::size_t>(i)];
        if (a.destroyed) continue;
        if (a.owner != war_pair_[0] && a.owner != war_pair_[1]) continue;

        const int cell = bucket_index_(a.x, a.y);
        const int cx = cell % bucket_cols_;
        const int cy = cell / bucket_cols_;
        const std::int64_t ax = static_cast<std::int64_t>(a.x) * kFpOne + a.fx;
        const std::int64_t ay = static_cast<std::int64_t>(a.y) * kFpOne + a.fy;

        for (int ny = std::max(0, cy - 1);
             ny <= std::min(bucket_rows_ - 1, cy + 1); ++ny) {
            for (int nx = std::max(0, cx - 1);
                 nx <= std::min(bucket_cols_ - 1, cx + 1); ++nx) {
                for (const int j :
                     buckets_[static_cast<std::size_t>(ny * bucket_cols_ + nx)]) {
                    if (j <= i) continue;   // each pair once
                    const auto& b = units_[static_cast<std::size_t>(j)];
                    if (b.destroyed) continue;
                    if (!hostile_(a.owner, b.owner)) continue;
                    const std::int64_t bx =
                        static_cast<std::int64_t>(b.x) * kFpOne + b.fx;
                    const std::int64_t by =
                        static_cast<std::int64_t>(b.y) * kFpOne + b.fy;
                    const std::int64_t ddx = bx - ax;
                    const std::int64_t ddy = by - ay;
                    if (ddx * ddx + ddy * ddy > range2_sq) continue;
                    engaged_a_.push_back(i);
                    engaged_b_.push_back(j);
                }
            }
        }
    }
    stats_.update_engaged = static_cast<int>(engaged_a_.size());
    stats_.engaged_pairs += stats_.update_engaged;
    if (engaged_a_.empty()) return;

    // The exchange: each pair resolves a linear attrition — the take
    // is proportional to the ENEMY's combat power (strength × supply ×
    // morale). Fractional kills accumulate per battalion (fixed point)
    // and land as whole vehicles through the one loss path.
    const std::int64_t rate_fp = static_cast<std::int64_t>(
        cfg_.exchange_vehicles_per_hour) * kFpOne *
        static_cast<std::int64_t>(cfg_.update_sec) / 3600;
    const std::int64_t now_abs = epoch_ + next_update_;

    for (std::size_t p = 0; p < engaged_a_.size(); ++p) {
        const std::size_t ia = static_cast<std::size_t>(engaged_a_[p]);
        const std::size_t ib = static_cast<std::size_t>(engaged_b_[p]);
        GroundUnitState& a = units_[ia];
        GroundUnitState& b = units_[ib];
        if (a.destroyed || b.destroyed) continue;

        const auto power = [](const GroundUnitState& u) noexcept {
            return static_cast<std::int64_t>(u.strength) *
                   (u.supply + 100) * (u.morale + 100) / 40000;
        };
        const std::int64_t pa = power(a);
        const std::int64_t pb = power(b);
        const std::int64_t total = pa + pb;
        if (total <= 0 || rate_fp <= 0) {
            // Contact with no combat power on either side: pinned, no
            // exchange (two spent formations glaring at each other).
            a.pinned = true;
            b.pinned = true;
            continue;
        }

        // Fixed-point takes for this pair.
        const std::int64_t take_a_fp = rate_fp * pb / total;
        const std::int64_t take_b_fp = rate_fp * pa / total;
        a.loss_acc = static_cast<std::uint32_t>(
            a.loss_acc + static_cast<std::uint32_t>(take_a_fp));
        b.loss_acc = static_cast<std::uint32_t>(
            b.loss_acc + static_cast<std::uint32_t>(take_b_fp));
        const int kills_a = static_cast<int>(a.loss_acc / kFpOne);
        const int kills_b = static_cast<int>(b.loss_acc / kFpOne);
        a.loss_acc %= kFpOne;
        b.loss_acc %= kFpOne;

        if (kills_a > 0) {
            apply_vehicle_loss_(ia, kills_a, b.vu, b.owner, false, 0, true);
        }
        if (kills_b > 0) {
            apply_vehicle_loss_(ib, kills_b, a.vu, a.owner, false, 0, true);
        }

        // The fighting's own costs (even kill-less ticks): fatigue,
        // morale erosion with losses taken, contact bookkeeping.
        if (a.fatigue < 100) ++a.fatigue;
        if (b.fatigue < 100) ++b.fatigue;
        if (kills_a > 0) {
            a.morale = static_cast<std::uint8_t>(
                std::max(0, static_cast<int>(a.morale) - 2 * kills_a));
        }
        if (kills_b > 0) {
            b.morale = static_cast<std::uint8_t>(
                std::max(0, static_cast<int>(b.morale) - 2 * kills_b));
        }
        a.last_combat = now_abs;
        b.last_combat = now_abs;
        a.pinned = true;
        b.pinned = true;
        a.dirty = true;
        b.dirty = true;
    }
}

// ============================================================================
// Capture — flip undefended enemy objectives
// ============================================================================

void GroundWar::capture_phase_() {
    const std::int64_t range_obj = cfg_.capture_range_grid;
    const std::int64_t contact = cfg_.contact_range_grid;

    for (auto& obj : objectives_) {
        if (war_pair_.size() < 2) break;
        if (obj.owner != war_pair_[0] && obj.owner != war_pair_[1]) {
            continue;   // neutral territory is not the ground war's
        }
        const std::uint8_t holder = obj.owner;
        const std::uint8_t taker_side =
            holder == war_pair_[0] ? war_pair_[1] : war_pair_[0];

        // First eligible taker in wire order.
        for (std::size_t i = 0; i < units_.size(); ++i) {
            GroundUnitState& u = units_[i];
            if (u.destroyed || !u.mobile || u.owner != taker_side) continue;
            if (u.strength < cfg_.capture_min_strength) continue;
            if (chebyshev(u.x, u.y, obj.x, obj.y) > range_obj) continue;

            // Defender check: any holder-side battalion in contact
            // with the objective itself contests the flip.
            bool contested = false;
            const int cell = bucket_index_(obj.x, obj.y);
            const int cx = cell % bucket_cols_;
            const int cy = cell / bucket_cols_;
            for (int ny = std::max(0, cy - 1);
                 ny <= std::min(bucket_rows_ - 1, cy + 1) && !contested;
                 ++ny) {
                for (int nx = std::max(0, cx - 1);
                     nx <= std::min(bucket_cols_ - 1, cx + 1) && !contested;
                     ++nx) {
                    for (const int j :
                         buckets_[static_cast<std::size_t>(
                             ny * bucket_cols_ + nx)]) {
                        const auto& d = units_[static_cast<std::size_t>(j)];
                        if (d.destroyed || d.owner != holder) continue;
                        if (chebyshev(d.x, d.y, obj.x, obj.y) <= contact) {
                            contested = true;
                            break;
                        }
                    }
                }
            }
            if (contested) break;   // this objective fights on

            // Flip: the ledger books the capture; the objective mirror
            // carries the new owner (the write-back lands it).
            const std::uint8_t from = obj.owner;
            obj.owner = u.owner;
            u.target = obj.vu;       // garrison the prize
            u.dest_x = obj.x;
            u.dest_y = obj.y;
            ++stats_.captures;
            if (ledger_ != nullptr) {
                ledger_->apply_objective_capture(
                    static_cast<double>(clock_), obj.vu, from, u.owner,
                    u.vu);
            }
            break;
        }
    }
}

// ============================================================================
// Resupply — the last_resupply cadence (catch-up-once)
// ============================================================================

void GroundWar::resupply_phase_(CampaignTime t) {
    if (cfg_.resupply_period_sec <= 0) return;
    const std::int64_t now_abs = epoch_ + t;
    if (now_abs <= last_resupply_ + cfg_.resupply_period_sec) return;

    for (auto& u : units_) {
        if (u.destroyed) continue;
        u.supply = static_cast<std::uint8_t>(
            std::min(100, static_cast<int>(u.supply) + 25));
        u.fatigue = static_cast<std::uint8_t>(
            std::max(0, static_cast<int>(u.fatigue) - 25));
        u.morale = static_cast<std::uint8_t>(
            std::min(100, static_cast<int>(u.morale) + 10));
        u.dirty = true;
    }
    ++stats_.resupply_fires;
    // Catch-up-once: the anchor jumps to now (the reinforcement
    // cadence's own shape — a stale .cmp timer fires ONE tick).
    last_resupply_ = now_abs;
}

// ============================================================================
// Air-caused losses — the sink's AG bookings applied to the line
// ============================================================================

void GroundWar::pull_air_losses_() {
    if (ledger_ == nullptr) return;
    const auto& log = ledger_->ground_loss_log();
    for (std::size_t k = air_loss_cursor_; k < log.size(); ++k) {
        const auto& e = log[k];
        if (!e.air) continue;
        const auto it = std::find(unit_vus_.begin(), unit_vus_.end(),
                                  e.victim);
        if (it == unit_vus_.end()) continue;
        const auto idx = static_cast<std::size_t>(
            it - unit_vus_.begin());
        // book=false: the LEDGER already carries these kills (the
        // sink booked them); only the engine's state moves.
        apply_vehicle_loss_(idx, e.kills, 0, 0, true, e.killer_squadron,
                            false);
    }
    air_loss_cursor_ = log.size();
}

// ============================================================================
// The one loss path — roster decay, counters, death
// ============================================================================

void GroundWar::apply_vehicle_loss_(std::size_t idx, int kills,
                                    std::uint32_t attacker_battalion,
                                    std::uint8_t attacker_team,
                                    bool air,
                                    std::uint32_t killer_squadron,
                                    bool book) {
    GroundUnitState& u = units_[idx];
    if (kills <= 0 || u.destroyed) return;
    if (kills > u.strength) kills = u.strength;
    if (kills <= 0) return;

    // Roster decay: vehicles leave the highest-index group first
    // (deterministic; the formation's tail thins before its core).
    int remaining = kills;
    while (remaining > 0) {
        int group = -1;
        for (int g = 15; g >= 0; --g) {
            if (((u.roster >> (g * 2)) & 0x03) != 0) { group = g; break; }
        }
        if (group < 0) break;
        const int have = static_cast<int>((u.roster >> (group * 2)) & 0x03);
        const int take = std::min(have, remaining);
        u.roster -= static_cast<std::uint32_t>(take) << (group * 2);
        remaining -= take;
    }

    u.strength -= kills;
    u.run_losses += kills;
    u.dirty = true;
    stats_.vehicle_losses += kills;

    if (book && ledger_ != nullptr) {
        ledger_->apply_ground_loss(
            static_cast<double>(clock_), u.vu, u.owner,
            attacker_battalion, attacker_team, kills, air,
            killer_squadron);
    }

    if (u.strength <= 0) mark_destroyed_(idx);
}

void GroundWar::mark_destroyed_(std::size_t idx) {
    GroundUnitState& u = units_[idx];
    if (u.destroyed) return;
    u.destroyed = true;
    u.dirty = true;
    u.pinned = false;
    ++stats_.battalions_destroyed;
    // The ledger's team-side destroyed counter books on the SYNC
    // (destruction transition detection lives there); the engine's
    // own stat mirrors it for the harness's per-sample columns.
}

// ============================================================================
// Ledger sync — dirty battalions push their final state
// ============================================================================

void GroundWar::sync_ledger_() {
    if (ledger_ == nullptr) return;
    for (const auto& u : units_) {
        if (!u.dirty) continue;
        GroundUnitLedger g;
        g.vu = u.vu;
        g.owner = u.owner;
        g.strength_initial = u.strength_initial;
        g.strength = u.strength;
        g.x = u.x;
        g.y = u.y;
        g.supply = u.supply;
        g.morale = u.morale;
        g.fatigue = u.fatigue;
        g.destroyed = u.destroyed;
        ledger_->sync_ground_unit(g);
    }
    // The dirty flags clear AFTER the whole walk — a battalion dirtied
    // by the same update's earlier phases is synced once (this pass),
    // and the next update starts clean.
    for (auto& u : units_) u.dirty = false;
}

// ============================================================================
// Helpers
// ============================================================================

bool GroundWar::hostile_(std::uint8_t a, std::uint8_t b) const {
    // The war pair is the pair — the ground war is a two-side machine
    // this slice (a third armed team's stance row is the C6 known-gap
    // story, shared with the air side).
    if (war_pair_.size() < 2) return false;
    return (a == war_pair_[0] && b == war_pair_[1]) ||
           (a == war_pair_[1] && b == war_pair_[0]);
}

int GroundWar::objective_score_(const GroundObjectiveState& o) const {
    // The reference's DoCalculations terms, integer, no RNG:
    //   front proximity: (200 − dist) × 0.2, capped ±30
    //   priority bonus:  +50 above 95, +20 above 90
    //   (random(5) dropped — the determinism contract)
    int dist = 200;
    if (!front_.empty()) {
        const auto col = static_cast<std::size_t>(o.x - front_.front().x);
        if (col < front_.size() && front_[col].contested) {
            dist = std::abs(o.y - front_[col].y);
        } else {
            // Nearest contested column (bounded outward scan).
            for (int r = 1; r <= 20 && dist == 200; ++r) {
                const auto left = col >= static_cast<std::size_t>(r)
                    ? front_[col - static_cast<std::size_t>(r)]
                    : FrontColumn{};
                const std::size_t ri = col + static_cast<std::size_t>(r);
                const auto& right = ri < front_.size()
                    ? front_[ri] : left;
                if (left.contested) { dist = std::abs(o.y - left.y); break; }
                if (right.contested) { dist = std::abs(o.y - right.y); break; }
            }
        }
    }
    int front_score = (200 - dist) / 5;    // × 0.2
    front_score = clamp_i(front_score, -30, 30);
    int bonus = 0;
    if (o.priority > 95) bonus = 50;
    else if (o.priority > 90) bonus = 20;
    return clamp_i(front_score + bonus, 0, 100);
}

int GroundWar::distance_(const GroundUnitState& u,
                         const GroundObjectiveState& o) const {
    return chebyshev(u.x, u.y, o.x, o.y);
}

} // namespace f4::campaign
