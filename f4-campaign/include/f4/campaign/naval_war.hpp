// f4-campaign/include/f4/campaign/naval_war.hpp
//
// CAMP-DOM-6 — NavalWar, the task-force movement engine (the naval
// GroundWar sibling; CAMP-DOM-5's as-built "how deep" record named it
// first: "the wire's dest_x/dest_y is decoded but never consumed — a
// naval GroundWar sibling is its own tranche").
//
// WHAT THIS IS. DOM-5 made the tasking pipeline see the enemy's task
// forces (the ranked pool, the filings, the query rows); the forces
// themselves sat frozen at their save-time grid cells. This engine is
// the naval movement half of the ground war's shape — the SAME
// machinery, the SEA subset:
//
//   * SNAPSHOT — the wire's domain-4 TaskForce rows (wire order, each
//     carrying its WorldState::units index so the session can sync the
//     moved rows back — the query face reads the WorldState, not the
//     engine).
//   * WAR PAIR — the same named-slot belligerent_pair() rule
//     GroundWar uses (the shared front math's derivation, declared in
//     ground_war.hpp). A task force whose owner is not in the first
//     at-war pair stands down (a legal, pinned state — the ground
//     engine's own rule; the two-war-pair limitation is G1's, naval
//     rides it).
//   * DESTINATION — the wire's own dest_x/dest_y IS the order. No
//     naval orders cycle, no re-tasking: the GTM sibling that would
//     re-task a arrived force onto a new objective is the deeper
//     naval tranche (the "how deep" record keeps it out). A force at
//     its destination (or with dest == position) holds.
//   * MOVEMENT — at the update cadence, every war-pair task force with
//     an unarrived destination walks toward it at its movement speed
//     (the wire's UCD movement_speed when the enrichment carried it,
//     else the sea family default — the same documented-limitation
//     pattern as the ground table: the reference's naval speeds ride
//     the UCD data our exports cannot see). Speeds live in grid
//     units: 1 grid ≈ 1.024 km, kph → grid/sec. Sub-grid travel
//     accumulates in 1/256 fixed point (the ground engine's own
//     arithmetic, verbatim: the integer-truncated sqrt normalization,
//     the arrival snap, the heading byte via atan2 ÷ 1.40625°).
//   * NO ENGAGE, NO CAPTURE, NO SUPPLY GATE — naval losses are a later
//     tranche's book (DOM-5's own record: "a naval-loss book all stay
//     OUT"); the TaskForce tail's supply byte is CARRIED (the query
//     serves it) but gates nothing — its naval semantics are unseen
//     wire, and the ground's <25 half-speed doctrine is a ground
//     document. Fatigue does not accrue (ships do not tire the way
//     battalions do, and no wire field would carry it back).
//   * NO LEDGER — the engine books nothing. The books are the war's
//     air+ground truth (the one-ledger-writer discipline); a task
//     force's POSITION is not a war fact the ledger owns — it is
//     WorldState face, served live by the `taskforces` query and
//     carried by the save (the §6.1 emitter + json2cam reencode).
//     GroundWar syncs the ledger because it ATTRITES (the books must
//     know); NavalWar only moves (the map must know).
//
// DISCIPLINE (the ground engine's, unchanged):
//   * NO EntityWorld, NO f4-world-convert here. The engine sees
//     campaign identity (VU_IDs, team slots, grid cells) and numbers.
//   * NO RNG, NO clocks of its own. The war advances by tick(delta)
//     on the caller's clock; ordering is wire order everywhere; the
//     same sources + the same tick sequence produce the same state
//     (pinned by test).
//   * ONE WRITER — of WorldState rows, via apply_naval_to()
//     (naval_writeback.hpp): the session calls it per update after
//     tick(), only dirty rows are written, and the save path needs
//     nothing new (the rows are already current; the save's own call
//     is the idempotent second touch).
//
// Dependencies: f4-world (IDataSource). C++20.

#pragma once

#include <f4/campaign/mission_type.hpp>
#include <f4/world/data_source.hpp>

#include <cstdint>
#include <vector>

namespace f4::campaign {

/// Tunables. The ground engine's shape with one knob: movement needs a
/// cadence and nothing else (no orders cycle, no contact, no capture,
/// no supply doctrine — each excluded by the tranche contract above).
struct NavalWarConfig {
    /// Naval update cadence (campaign seconds). The ground update's
    /// own default: 60 s keeps a 24-hour war at 1440 updates of a
    /// handful-of-ships walk — trivially cheap — while sub-grid
    /// movement still resolves at cruise speeds.
    CampaignTime update_sec = 60;
};

/// One task force's live state inside the engine (the sim-side truth;
/// the WorldState row is the serving face, updated per sync).
struct NavalUnitState {
    std::uint32_t vu = 0;        ///< VU_ID.num (the campaign key)
    std::uint8_t owner = 0;      ///< team slot
    std::uint8_t subtype = 0;    ///< STYPE_SEA_* (3 = carrier, ...)
    /// The task force's WorldState::units index (the sync's address;
    /// the engine walks the source in wire order, and the adapter
    /// serves exactly that order).
    std::size_t ws_index = 0;
    std::int32_t x = 0;          ///< grid column (current, integer)
    std::int32_t y = 0;          ///< grid row (current, integer)
    std::int32_t fx = 0;         ///< sub-grid fraction (0..255, × grid)
    std::int32_t fy = 0;         ///< sub-grid fraction (0..255)
    std::int32_t dest_x = 0;     ///< movement destination (the wire's
    std::int32_t dest_y = 0;     ///< own field — consumed, never written)
    std::uint8_t supply = 0;     ///< the TaskForce tail's byte (carried)
    std::uint32_t roster = 0;    ///< the wire's 2-bit packing (carried)
    std::uint8_t heading = 0;    ///< wire convention: 0-255, ×1.4 deg
    /// Absolute campaign time of the last move (the wire's own
    /// convention — TestCamp's are ~38.5M s).
    std::int64_t last_move = 0;
    /// Movement speed (kph; the wire's UCD movement_speed, else the
    /// sea family default).
    int speed_kph = 0;
    /// Precomputed movement step per update tick (grid × 1/256).
    int step_fp = 0;
    /// Snapped to the destination (holds from here on; the engine has
    /// no re-tasking to release it).
    bool arrived = false;
    /// Moved since the last WorldState sync (the sync's activity
    /// filter — a mirror that never moved must not normalize the row).
    bool dirty = false;
};

/// Telemetry (the QC's naval_move block and the harness's report read
/// these; every counter is cumulative unless marked pulse).
struct NavalWarStats {
    int updates = 0;             ///< naval update ticks fired
    int moved_events = 0;        ///< update ticks in which >= 1 force moved
    int arrivals = 0;            ///< destination arrivals (cumulative)
    /// Distance sailed by the whole fleet, grid units ×256 (integer).
    std::uint64_t fleet_distance_fp = 0;
    int task_forces = 0;         ///< snapshot size
    int moving_now = 0;          ///< unarrived war-pair forces NOW
};

class NavalWar {
public:
    /// Snapshot the sources. All references are borrowed for the
    /// constructor call only (the snapshot is taken immediately —
    /// the ground engine's own discipline; the engine holds no
    /// source pointers after construction).
    NavalWar(const f4::world::ICampaignSource& camp,
             const f4::world::ITeamSource& teams,
             const f4::world::IUnitCoreSource& units,
             const NavalWarConfig& cfg = {});

    /// Advance the war by `delta_sec` campaign seconds. Accumulates
    /// whole update ticks and fires them in order (one big tick ==
    /// N small ones — the C2 pin, same contract). A war-less world
    /// (no belligerent pair) is deliberately inert: the clock
    /// advances, nothing moves (pinned by test).
    void tick(CampaignTime delta_sec);

    /// Campaign-relative clock (seconds; 0 = snapshot time).
    [[nodiscard]] CampaignTime clock() const noexcept { return clock_; }

    // --- Queries (the session, harness, QC, and sync read) -----------

    /// The war pair (slot order; empty when nobody is at war — the
    /// engine is then deliberately inert, the ground engine's rule).
    [[nodiscard]] const std::vector<std::uint8_t>&
    belligerents() const noexcept {
        return war_pair_;
    }

    /// Task force live states, wire order (the WorldState sync's
    /// source).
    [[nodiscard]] const std::vector<NavalUnitState>&
    units() const noexcept {
        return units_;
    }

    /// Clear the moved flags (apply_naval_to's own accounting —
    /// called once after the sync walk has written the dirty rows;
    /// idempotent, a no-op when nothing moved).
    void clear_dirty() noexcept {
        for (auto& u : units_) u.dirty = false;
    }

    [[nodiscard]] const NavalWarStats& stats() const noexcept {
        return stats_;
    }

private:
    void move_phase_();           ///< advance unarrived war-pair forces
    void recount_moving_();       ///< the moving_now pulse counter

    NavalWarConfig cfg_;

    std::vector<std::uint8_t> war_pair_;

    std::vector<NavalUnitState> units_;   ///< wire order (subset)

    CampaignTime clock_ = 0;
    CampaignTime next_update_ = 0;

    /// Absolute-time bridge (the .cmp header's now — the ground
    /// engine's own epoch_; last_move stamps ride it).
    std::int64_t epoch_ = 0;

    NavalWarStats stats_;
};

} // namespace f4::campaign
