// f4-campaign/src/naval_war.cpp
//
// CAMP-DOM-6 — NavalWar implementation. See naval_war.hpp for the
// tranche rationale and the excluded-deeper-naval record.
//
// Determinism discipline (the whole file, the ground engine's own
// words): NO RNG, NO wall clocks, NO iteration over unordered
// containers. Every walk is over wire-order vectors; every arithmetic
// path is integer fixed-point (positions ×256) except the two libm
// calls in the movement phase (sqrt for the step normalization, atan2
// for the heading byte) — both pure functions of integer inputs, both
// exercised identically in every run, and neither value ever reaches
// a book (heading is quantized to the wire's own byte).

#include <f4/campaign/naval_war.hpp>

#include <f4/campaign/ground_war.hpp>

#include <cmath>

namespace f4::campaign {

namespace {

// ---------------------------------------------------------------------------
// Constants — the wire's own vocabulary (the ground engine's pattern:
// referenced by value; f4-campaign deliberately does not depend on
// f4-world-convert's enum headers).
// ---------------------------------------------------------------------------

/// DOMAIN_SEA.
constexpr std::uint8_t kDomainSea = 4;

/// Sea-domain unit subtypes (classtbl.h STYPE_SEA_*).
constexpr std::uint8_t kStSeaAmphibious = 1;
constexpr std::uint8_t kStSeaBattleship = 2;
constexpr std::uint8_t kStSeaCarrier = 3;
constexpr std::uint8_t kStSeaCruiser = 4;
constexpr std::uint8_t kStSeaDestroyer = 5;
constexpr std::uint8_t kStSeaFrigate = 6;
constexpr std::uint8_t kStSeaPatrol = 7;
constexpr std::uint8_t kStSeaSupply = 8;
constexpr std::uint8_t kStSeaTanker = 9;
constexpr std::uint8_t kStSeaTransport = 10;

/// Default movement speeds by sea subtype family (kph), used when the
/// wire carries no UCD movement_speed (the fixture UCD is an 8-entry
/// sample; most saves carry no enrichment). Cruise rates in the
/// reference's own scale — a carrier group's make-piece speed, not a
/// flat-out run (≈ knots × 1.852: 25 kph ≈ 13.5 kt for the big
/// decks, 30 ≈ 16 kt for the escorts, the amphibs and replenishment
/// ships trail at the group's slowest hull). The same
/// documented-limitation pattern as the ground table: the reference's
/// naval speeds ride the UCD unit data our exports cannot see; these
/// are the family defaults the fixtures actually exercise.
int default_naval_speed_kph(std::uint8_t st) noexcept {
    switch (st) {
        case kStSeaCarrier:    return 25;
        case kStSeaBattleship: return 25;
        case kStSeaCruiser:    return 30;
        case kStSeaDestroyer:  return 30;
        case kStSeaFrigate:    return 30;
        case kStSeaPatrol:     return 35;
        case kStSeaAmphibious: return 12;
        case kStSeaSupply:     return 15;
        case kStSeaTanker:     return 15;
        case kStSeaTransport:  return 15;
        default:               return 0;   // unknown sea hull: holds
    }
}

/// Fixed-point helpers: positions live in 1/256 grid units (the wire's
/// own position-byte semantics, the ground engine's constant).
constexpr int kFpOne = 256;

bool in_war_pair(const std::vector<std::uint8_t>& pair,
                 std::uint8_t owner) noexcept {
    for (const auto slot : pair) {
        if (slot == owner) return true;
    }
    return false;
}

} // namespace

// ===========================================================================
// Snapshot
// ===========================================================================

NavalWar::NavalWar(const f4::world::ICampaignSource& camp,
                   const f4::world::ITeamSource& teams,
                   const f4::world::IUnitCoreSource& units,
                   const NavalWarConfig& cfg)
    : cfg_(cfg) {
    // The war pair (the shared derivation — the ground engine's own
    // rule, one code path so the two can never drift).
    war_pair_ = belligerent_pair(teams);

    epoch_ = static_cast<std::int64_t>(camp.current_time());

    // The per-update movement step's seconds term (the ground
    // engine's precompute_fp).
    const int precompute_fp = static_cast<int>(cfg_.update_sec) > 0
        ? static_cast<int>(cfg_.update_sec) : 1;

    for (int i = 0; i < units.unit_count(); ++i) {
        if (units.unit_class(i) != f4::entities::UnitClass::TaskForce) {
            continue;
        }
        if (units.domain(i) != kDomainSea) continue;

        NavalUnitState u;
        u.vu = units.id_num(i);
        u.owner = units.owner(i);
        u.subtype = units.unit_subtype(i);
        u.ws_index = static_cast<std::size_t>(i);
        u.x = units.x(i);
        u.y = units.y(i);
        u.dest_x = units.dest_x(i);
        u.dest_y = units.dest_y(i);
        u.roster = units.roster(i);
        if (const auto* gu = units.as_ground_unit(i); gu != nullptr) {
            // The TaskForce class is ground-accessible (the adapter's
            // own rule — Battalion | Brigade | TaskForce); the tail's
            // supply byte and the shared tactical state ride it.
            u.supply = gu->supply(i);
            u.last_move = gu->last_move(i);
            u.heading = gu->heading(i);
        }

        u.speed_kph = units.movement_speed(i) > 0
            ? units.movement_speed(i)
            : default_naval_speed_kph(u.subtype);
        // Per-update movement step, grid units × 1/256 (the ground
        // engine's arithmetic verbatim).
        u.step_fp = static_cast<int>(
            (static_cast<std::int64_t>(u.speed_kph) * kFpOne *
             precompute_fp) / 3600);

        // A force already at its destination holds (the wire's own
        // "no orders pending" state; pinned by test — it never dirties
        // a row and never counts as movement).
        u.arrived = (u.x == u.dest_x && u.y == u.dest_y) ||
                    u.speed_kph <= 0 ||
                    !in_war_pair(war_pair_, u.owner);

        units_.push_back(u);
    }
    stats_.task_forces = static_cast<int>(units_.size());
    recount_moving_();
}

void NavalWar::recount_moving_() {
    stats_.moving_now = 0;
    for (const auto& u : units_) {
        if (!u.arrived) ++stats_.moving_now;
    }
}

// ===========================================================================
// The tick — one due-time wheel, movement only (no orders cycle, no
// engage, no capture, no resupply: each excluded by the tranche
// contract; the ground engine's shape with the phases removed).
// ===========================================================================

void NavalWar::tick(CampaignTime delta_sec) {
    if (delta_sec <= 0) return;
    if (war_pair_.empty()) {
        // No war pair: the engine is deliberately inert (pinned by
        // test). The clock still advances so attach-time queries agree
        // with the ladder's (the ground engine's own rule).
        clock_ += delta_sec;
        return;
    }
    if (cfg_.update_sec <= 0) {
        clock_ += delta_sec;
        return;
    }

    clock_ += delta_sec;
    while (clock_ >= next_update_) {
        move_phase_();

        ++stats_.updates;
        next_update_ += cfg_.update_sec;
    }
}

// ===========================================================================
// Movement — the ground move_phase_ arithmetic, verbatim, minus the
// pinned/supply/fatigue gates (naval has no contact, no doctrine, no
// fatigue) and with the destination = the wire's own dest (no orders
// cycle ever reassigns it).
// ===========================================================================

void NavalWar::move_phase_() {
    bool any_moved = false;
    const std::int64_t now_abs = epoch_ + next_update_;

    for (auto& u : units_) {
        if (u.arrived) continue;
        if (u.step_fp <= 0) continue;

        // Fixed-point position → destination vector.
        const std::int64_t px = static_cast<std::int64_t>(u.x) * kFpOne + u.fx;
        const std::int64_t py = static_cast<std::int64_t>(u.y) * kFpOne + u.fy;
        const std::int64_t tx = static_cast<std::int64_t>(u.dest_x) * kFpOne;
        const std::int64_t ty = static_cast<std::int64_t>(u.dest_y) * kFpOne;
        const std::int64_t dx = tx - px;
        const std::int64_t dy = ty - py;
        const std::int64_t len = static_cast<std::int64_t>(
            std::sqrt(static_cast<double>(dx * dx + dy * dy)));
        if (len <= 0) {
            // Degenerate sub-grid remainder: arrived.
            u.arrived = true;
            continue;
        }

        if (u.step_fp >= len) {
            // Arrive: snap to the destination (the ground engine's
            // arrival rule).
            u.x = u.dest_x;
            u.y = u.dest_y;
            u.fx = 0;
            u.fy = 0;
            u.arrived = true;
            ++stats_.arrivals;
            stats_.fleet_distance_fp += static_cast<std::uint64_t>(len);
        } else {
            // Advance along the normalized vector (integer truncation
            // — deterministic; the lost fraction stays behind, exactly
            // like the wire's own position byte carries the remainder).
            const std::int64_t nx = px + (dx * u.step_fp) / len;
            const std::int64_t ny = py + (dy * u.step_fp) / len;
            u.x = static_cast<std::int32_t>(nx / kFpOne);
            u.y = static_cast<std::int32_t>(ny / kFpOne);
            u.fx = static_cast<std::int32_t>(nx % kFpOne);
            u.fy = static_cast<std::int32_t>(ny % kFpOne);
            stats_.fleet_distance_fp += static_cast<std::uint64_t>(u.step_fp);
        }

        // Heading (the wire's byte convention: 0-255, ×1.40625 deg,
        // 0 = north — the ground engine's quantization).
        const double hdg_deg = std::atan2(
            static_cast<double>(dx), static_cast<double>(dy)) *
            (180.0 / 3.14159265358979323846);
        int hb = static_cast<int>(std::lround(hdg_deg / 1.40625)) % 256;
        if (hb < 0) hb += 256;
        u.heading = static_cast<std::uint8_t>(hb);

        u.last_move = now_abs;
        u.dirty = true;
        any_moved = true;
    }

    if (any_moved) ++stats_.moved_events;
    recount_moving_();
}

} // namespace f4::campaign
