// f4-entities/air_roster.hpp
//
// AGG-2b — the air-picture roster: f4-entities' SpatialIndex finally
// wired for the radar/detection term (Docs/AGGREGATE_CLOCK_PLAN.md §4
// AGG-2b; Docs/FID_OPT_PLAN.md §5's named 20.8 s residual).
//
// THE PROBLEM IT CLOSES. Two hot walks pay the full transform bucket
// every pass to reject the same 99.8% of candidates with the same two
// arithmetic checks:
//   * the radar scan's Search walk (RadarSimComponent::perform_scan —
//     ~7,400 candidates/scan/radar, 99.8% rejected by the ground-clutter
//     gate; the FID-OPT-3 ref-walk cut the per-candidate cost, the WALK
//     stayed O(theater) per radar per scan), and
//   * the air-picture walk (Simulation::push_air_picture_ — the same
//     bucket, the same clutter rule, ~1.4 ms per walk at 10 Hz under
//     demand).
// The rejection predicate — TransformComponent::is_ground_clutter()
// (stationary AND below 8,000 ft MSL) — is a property that is TRUE for
// essentially the whole parked theater and only flips on rare events.
// Caching the FLIPPED population (the non-clutter membership, ~100-200
// of ~7,400) turns both walks O(contacts) while every VALUE the walks
// read (positions, velocities, liveness, clutter truth) stays a fresh
// transform read — the index is a membership filter, never a value
// cache.
//
// THE MAINTENANCE RULE (what makes the cache honest). Membership flips
// come in exactly two shapes:
//   * STRUCTURAL — spawn, destroy, component add/replace/remove. The
//     world's structural_epoch() bumps on every one of them (CAMP-OPT-1);
//     the roster compares its captured epoch at every refresh and
//     rebuilds when it moved. Latency: the next refresh call.
//   * BEHAVIORAL — a parked entity starts moving (the taxi launch, the
//     ground unit's advance) or a mover stops low (the landing rollout).
//     No structural event marks these — transform writes go straight
//     into the component. The roster rebuilds on a CALLER-DRIVEN
//     cadence (sim-clock based, never wall clock): the radar scans and
//     the picture walk refresh with their own stamped time and the
//     shared 1 s revalidation interval, so a behavioral flip is observed
//     within one cadence window of when the uncached walk would have
//     seen it. That bounded latency is AGG-2b's only observable delta,
//     it is documented in the plan (§5 re-pinning), and every gate the
//     walks apply FRESH (clutter, range, liveness, exclusion) keeps the
//     old outputs exact for the shared population.
//
// CONSUMPTION CONTRACT.
//   * members() is the non-clutter population in ENTITY-INDEX order —
//     the same order the uncached walks produced (the transform ref
//     bucket's order is entity-index order by invariant, and the rebuild
//     walks that bucket). Radar candidate order, air-picture contact
//     order, and the RNG streams that ride them are preserved.
//   * The scan re-applies its gates fresh per member: a member that
//     became clutter since the refresh (landed) is skipped exactly as
//     the uncached walk would skip it; the fresh gate is idempotent.
//   * within_radius() reads the SPATIAL side — the roster maintains a
//     SpatialIndex over member positions captured at refresh time. It
//     answers future air-picture consumers (SAM rings, formation
//     spacing, threat queries) at O(ball) instead of O(theater). It is
//     deliberately NOT a pruning authority for the radar scan: positions
//     are as-of-refresh, and a mover crossing a query boundary between
//     refreshes would be pruned on stale geometry. The scan walks
//     members() with fresh transform reads instead — the membership is
//     the win, the ball would not prune it anyway (the 8× range cutoff
//     covers the theater at fighter radar ranges).
//
// Threading: sim thread only, same rule as update_all().

#pragma once

#include <cstdint>
#include <vector>

#include "spatial_index.hpp"

// Forward declaration — the roster's header needs only the reference;
// air_roster.cpp includes entity.hpp for the walk templates.
namespace f4::entities { class EntityWorld; }

namespace f4::entities {

class AirPictureRoster {
public:
    /// Refresh the membership against `world`.
    ///
    /// Rebuilds when the world's structural epoch moved past the
    /// captured one (or on the first call — the constructor state is
    /// unseen, the AGG-2a verdict-gate doctrine), and otherwise when
    /// `now_s - last_refresh >= revalidate_interval_s` (the behavioral
    /// flip cadence). `revalidate_interval_s <= 0` revalidates on every
    /// call (the tests' exact-control knob; a never-revalidates rig
    /// passes a huge interval). `now_s` is the host-stamped sim clock —
    /// never wall time, so the refresh schedule is deterministic and
    /// replayable.
    void refresh(const EntityWorld& world, double now_s,
                 double revalidate_interval_s);

    /// The non-clutter population, entity-index order (the bucket
    /// order the uncached walks produced). Valid until the next
    /// refresh() — iterate immediately, or copy (the radar scan copies
    /// into its candidate vector by value).
    [[nodiscard]] const std::vector<EntityId>& members() const noexcept {
        return members_;
    }

    /// Member ids within `radius` (slant) of (cx,cy,cz), positions as
    /// of the last refresh. See the header doc: a convenience for
    /// air-picture consumers, not a pruning authority.
    [[nodiscard]] std::vector<EntityId> within_radius(
        double cx, double cy, double cz, double radius) const;

    /// The spatial side's cell size (passthrough for probes).
    [[nodiscard]] double index_cell_size() const noexcept {
        return index_.cell_size();
    }

    // --- Probe counters (the tests' maintenance-rule observability) ---
    [[nodiscard]] std::size_t rebuild_count() const noexcept {
        return rebuilds_;
    }
    [[nodiscard]] bool primed() const noexcept { return primed_; }

private:
    void rebuild_(const EntityWorld& world);

    bool primed_ = false;
    std::uint64_t epoch_ = 0;          // structural epoch at last rebuild
    double last_refresh_s_ = 0.0;      // sim clock at last rebuild
    std::vector<EntityId> members_;    // non-clutter, entity-index order
    SpatialIndex index_;               // member positions at last rebuild
    std::size_t rebuilds_ = 0;
};

} // namespace f4::entities
