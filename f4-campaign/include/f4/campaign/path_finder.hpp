// f4-campaign/include/f4/campaign/path_finder.hpp
//
// AirPathFinder — the C3 grid A* (FreeFalcon asearch.cpp + path.cpp's
// GetNeighborCoord port).
//
// The reference engine is a hand-rolled A* ("ASearch") with a
// pre-allocated pool of 2000 nodes (zero allocation during search), a
// sorted linked-list open queue keyed on f = g + h, a linear tried
// list, and paths stored as bit-packed 8-direction steps. The air
// flavor of its extend function (GetNeighborCoord with a MOVE_AIR
// movement type) does, per neighbor:
//
//   * step size QuickSearch = MAP_RATIO*2 = 12 grid units per move
//     (8 directions, diagonal cost x 1.41);
//   * threat sampling: ScoreThreatFast at the cell center AND the 4
//     cardinal offsets (+-MAP_RATIO); the MAXIMUM of the five is the
//     move's threat cost — max > 120 makes the neighbor impassable
//     (the reference's no-fly wall; in the reference this fires for
//     RoE-denied territory — our stance vocabulary cannot express the
//     denying classes, so with the 2-bit density cap at ~100 the check
//     stands armed for the future RoE refinement and threat scores
//     shape routes through the COST term instead);
//   * otherwise cost = base x step + threat / 2 (base = 1.0: the air
//     column of the terrain CostTable is uniformly 1.0);
//   * heuristic to_go = straight distance to target x 4.0 (the air
//     leftmod — strong goal pull, threat detours still win);
//   * snap-to-target: a neighbor within one step of the goal BECOMES
//     the goal (the search terminates on arrival, not on proximity).
//
// Failure behavior (the part that makes Falcon's routes usable):
//   * MAX_SEARCH 2000 nodes expanded per search (and the pool is 2000
//     nodes — expansion stops when either bound hits);
//   * path length capped at 96 steps (1152 grid units at step 12 —
//     beyond any Korea-theater hop);
//   * RETURN_PARTIAL_ON_FAIL: when the budget runs out, the search
//     returns the best node it reached — lowest to_go among explored
//     nodes (queue first, then tried; strict <, so the earliest
//     minimum wins, the reference's scan order).
//
// This port keeps the algorithm and every constant; the plumbing is
// modern C++ (parent-chain nodes instead of bit-packed directions —
// the route builder consumes positions, and a bit-packing layer would
// be ceremony without a wire consumer). No thread locking: the
// campaign layer is single-threaded by the same discipline the
// reference's critical sections protect against.
//
// Determinism: pure function of (threat map, endpoints, team, band).
// No RNG; queue order is insertion order (ties keep the earlier node —
// the reference's sorted-insert with strict < comparisons).
//
// Dependencies: f4-campaign (ThreatMap). C++20.

#pragma once

#include <f4/campaign/threat_map.hpp>

#include <cstdint>
#include <vector>

namespace f4::campaign {

/// How many locations the search is willing to expand (asearch.h
/// MAX_SEARCH) — also the node-pool size.
inline constexpr int kPathMaxSearch = 2000;

/// Longest path, in steps (asearch.h MAX_DISTANCE).
inline constexpr int kPathMaxSteps = 96;

/// Neighbor count (asearch.h MAX_NEIGHBORS — 8-directional grid).
inline constexpr int kPathMaxNeighbors = 8;

/// Default A* step in grid units (FindSafePath's QuickSearch =
/// MAP_RATIO * 2).
inline constexpr int kPathStep = kThreatMapRatio * 2;

/// A threat sample above this makes a cell impassable to routing
/// (path.cpp GetNeighborCoord's hcost > 120.0F test).
inline constexpr double kPathImpassableThreat = 120.0;

/// Air heuristic multiplier (path.cpp leftmod for MOVE_AIR).
inline constexpr double kPathHeuristicMultiplier = 4.0;

/// Search mode flags (asearch.h RETURN_*_ON_*).
enum class PathFlags : std::uint8_t {
    /// Empty path on failure (RETURN_EMPTY_ON_FAIL).
    EmptyOnFail = 0,
    /// Best-effort partial path when the search budget runs out
    /// (RETURN_PARTIAL_ON_FAIL).
    PartialOnFail = 1,
    /// Partial path when the path-length cap is hit
    /// (RETURN_PARTIAL_ON_MAX).
    PartialOnMax = 2,
};

/// One A* search's outcome.
struct AirPathResult {
    /// Grid positions along the path, origin first. The LAST entry is
    /// the search's end node — the target when complete, the
    /// best-effort node when partial.
    std::vector<std::pair<int, int>> positions;

    /// Path cost (sum of move costs, threat included).
    double cost = 0.0;

    /// The search reached the target.
    bool complete = false;

    /// The search ended early (budget or path-length cap) — positions
    /// is the best partial path found.
    bool partial = false;

    /// Nodes expanded (QC instrumentation).
    int nodes_expanded = 0;
};

/// The grid A* over one threat map. Reusable across searches (the node
/// pool is per-instance and reset per search — the ASD singleton's
/// data, minus the singleton).
class AirPathFinder {
public:
    /// \param map    the threat map scores are read from (must outlive
    ///               the finder)
    /// \param viewer the map's viewer team — bit-half rule; queries
    ///               score as this team
    explicit AirPathFinder(const ThreatMap& map,
                           std::uint8_t viewer) noexcept
        : map_(map), viewer_(viewer) {}

    /// Find a path from (ox, oy) to (tx, ty) for `team` at altitude
    /// band `alt`. Behavior per FreeFalcon's GetGridPath for air:
    ///
    ///   * flags PartialOnFail|PartialOnMax (the GetGridPath default):
    ///     budget exhaustion returns the best partial path instead of
    ///     an empty one;
    ///   * use_threat = true (the PATH_ENEMYCOST flavor FindSafePath
    ///     uses): threat cost sampled at the 5 points, > 120
    ///     impassable. use_threat = false is the PATH_BASIC flavor —
    ///     straight terrain cost only (still RoE-blind: it will cross
    ///     hostile territory, since the lethal score is threat data).
    /// \param max_search node budget (default kPathMaxSearch)
    [[nodiscard]] AirPathResult find(int ox, int oy, int tx, int ty,
                                     std::uint8_t team, AltBand alt,
                                     int flags = static_cast<int>(
                                         PathFlags::PartialOnFail) |
                                     static_cast<int>(
                                         PathFlags::PartialOnMax),
                                     bool use_threat = true,
                                     int max_search = kPathMaxSearch) const;

    /// One move's threat cost — max of ScoreThreatFast over the cell
    /// center + 4 cardinal offsets at MAP_RATIO. Exposed for tests.
    [[nodiscard]] double move_threat(int x, int y, std::uint8_t team,
                                     AltBand alt) const;

private:
    struct Node {
        int x = 0;
        int y = 0;
        double cost = 0.0;   // g — cost to reach this node
        double to_go = 0.0;  // h — lower bound to target
        int parent = -1;     // index into the pool
        int next = -1;       // open-queue link (-1 = none)
    };

    const ThreatMap& map_;
    std::uint8_t viewer_ = 0;
};

} // namespace f4::campaign
