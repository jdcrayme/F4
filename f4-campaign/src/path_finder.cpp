// f4-campaign/src/path_finder.cpp
//
// AirPathFinder implementation — see path_finder.hpp for the asearch.cpp
// correspondence. The port keeps the reference's algorithm shape:
// sorted linked-list open queue, linear tried list, fixed node pool,
// strict-< comparisons everywhere (deterministic ties), and the
// best-partial-node recovery on budget exhaustion.

#include "f4/campaign/path_finder.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace f4::campaign {

namespace {

// 8-direction offsets (campaign.cpp dx[]/dy[], d = 0..7: N, NE, E, SE,
// S, SW, W, NW).
constexpr int kDx[kPathMaxNeighbors] = {0, 1, 1, 1, 0, -1, -1, -1};
constexpr int kDy[kPathMaxNeighbors] = {1, 1, 0, -1, -1, -1, 0, 1};

// Diagonal moves cost x sqrt(2) (path.cpp: heading bit 0x01 test).
constexpr double kDiagonalFactor = 1.4142135623730951;

// Base cost of one grid unit of air movement — the air column of
// CostTable is uniformly 1.0 (GetMovementCost for MoveType Air).
constexpr double kAirBaseCost = 1.0;

// Map extent guard: the pool indices by position, so the search needs
// a bound. The threat map's derived extent covers every data point;
// targets beyond it are clamped for the search (they'd score 100
// "off map" anyway — the reference clamps via Map_Max_X/Y checks in
// GetNeighborCoord).
struct Extent {
    int max_x = 0;
    int max_y = 0;
};

double grid_distance(int ax, int ay, int bx, int by) {
    return std::sqrt(static_cast<double>((ax - bx) * (ax - bx) +
                                         (ay - by) * (ay - by)));
}

} // namespace

double AirPathFinder::move_threat(int x, int y, std::uint8_t team,
                                  AltBand alt) const {
    // GetNeighborCoord: five samples, maximum.
    const int s0 = map_.score(x, y, alt, team);
    int best = s0;
    best = std::max(best, map_.score(x - kThreatMapRatio, y, alt, team));
    best = std::max(best, map_.score(x + kThreatMapRatio, y, alt, team));
    best = std::max(best, map_.score(x, y - kThreatMapRatio, alt, team));
    best = std::max(best, map_.score(x, y + kThreatMapRatio, alt, team));
    return static_cast<double>(best);
}

AirPathResult AirPathFinder::find(int ox, int oy, int tx, int ty,
                                  std::uint8_t team, AltBand alt,
                                  int flags, bool use_threat,
                                  int max_search) const {
    AirPathResult result;

    if (ox == tx && oy == ty) {
        result.complete = true;
        result.positions.emplace_back(ox, oy);
        return result;
    }

    // Out-of-map endpoints: the reference's GetGridPath boundary check
    // clears the path and returns -1. Points beyond the derived extent
    // clamp INTO it (they score "off map" = 100 everywhere, which is
    // passable-but-costly, and the caller's target-area waypoints still
    // land where they should — only the intermediate path is clamped).
    const Extent ext{map_.cells_x() * kThreatMapRatio,
                     map_.cells_y() * kThreatMapRatio};
    ox = std::clamp(ox, 0, ext.max_x - 1);
    oy = std::clamp(oy, 0, ext.max_y - 1);
    tx = std::clamp(tx, 0, ext.max_x - 1);
    ty = std::clamp(ty, 0, ext.max_y - 1);
    if (ox == tx && oy == ty) {
        result.complete = true;
        result.positions.emplace_back(ox, oy);
        return result;
    }

    // The node pool: fixed 2000 slots, recycled through a free list —
    // the reference's pre-allocated nodes (pool size is INDEPENDENT of
    // the per-search expansion budget, as in asearch.cpp; a small
    // max_search still gets the full pool).
    std::vector<Node> pool(static_cast<std::size_t>(kPathMaxSearch));
    int free_head = 1;   // node 0 is the origin, never on the free list
    for (int i = 1; i < kPathMaxSearch - 1; ++i) {
        pool[static_cast<std::size_t>(i)].next = i + 1;
    }
    pool[static_cast<std::size_t>(kPathMaxSearch) - 1].next = -1;

    int open_head = -1;  // sorted by f = cost + to_go, ascending
    int tried_head = -1; // most-recently-expanded first (push order)

    Node& origin = pool[0];
    origin.x = ox;
    origin.y = oy;
    origin.cost = 0.0;
    origin.to_go = grid_distance(ox, oy, tx, ty) * kPathHeuristicMultiplier;
    origin.parent = -1;
    // Origin goes straight to the open queue (AS_attach_queues leaves
    // `location` pointing at node 0; the first AS_merge takes from it).
    open_head = 0;
    pool[0].next = -1;

    int expanded = 0;
    int solution = -1;
    bool budget_hit = false;
    bool queue_empty = false;

    while (expanded < max_search && free_head != -1) {
        // Pop the best open node.
        const int cur = open_head;
        if (cur < 0) {
            // No possible moves — the reference returns -1 HERE,
            // before any partial-path recovery (the recovery belongs
            // to budget exhaustion, not exhaustion of the graph).
            queue_empty = true;
            break;
        }
        open_head = pool[static_cast<std::size_t>(cur)].next;
        pool[static_cast<std::size_t>(cur)].next = -1;

        // Move to tried (push front — the reference links location->next
        // = tried; tried = location).
        pool[static_cast<std::size_t>(cur)].next = tried_head;
        tried_head = cur;

        const Node& node = pool[static_cast<std::size_t>(cur)];

        // Solution test: the snap-to-target rule makes the step onto
        // (or within one step of) the goal BE the goal — arrival is
        // exact position equality.
        if (node.x == tx && node.y == ty) {
            solution = cur;
            break;
        }

        // Path-length cap (the reference checks BEFORE expanding: a
        // node whose path is already max_length stops the search).
        const int steps = [&]() {
            int n = 0;
            for (int p = cur; p >= 0; p = pool[static_cast<std::size_t>(p)].parent)
                ++n;
            return n - 1;  // edges, not nodes
        }();
        if (steps >= kPathMaxSteps) {
            budget_hit = true;
            if (flags & static_cast<int>(PathFlags::PartialOnMax)) {
                solution = cur;
            }
            break;
        }

        // Extend: evaluate the 8 neighbors (GetNeighborCoord).
        for (int d = 0; d < kPathMaxNeighbors; ++d) {
            int nx = node.x + kPathStep * kDx[d];
            int ny = node.y + kPathStep * kDy[d];

            if (nx < 0 || ny < 0 || nx >= ext.max_x || ny >= ext.max_y) {
                continue;  // off the map — no neighbor
            }

            // Snap to target: within one step of the goal (the
            // reference's `left < QuickSearch` test).
            if (grid_distance(nx, ny, tx, ty) < kPathStep) {
                nx = tx;
                ny = ty;
            }

            // Cost: base x diagonal x step + threat/2 — or impassable
            // when the sampled threat is above the routing threshold.
            double move_cost = kAirBaseCost * kPathStep;
            if (d & 0x01) move_cost *= kDiagonalFactor;
            if (use_threat) {
                const double threat = move_threat(nx, ny, team, alt);
                if (threat > kPathImpassableThreat) {
                    continue;  // illegal move
                }
                move_cost += threat / 2.0;
            }

            // Duplicate suppression: a position already expanded is
            // never re-opened (the reference's tried-list scan in
            // AS_get_new_node).
            bool already_tried = false;
            for (int p = tried_head; p >= 0;
                 p = pool[static_cast<std::size_t>(p)].next) {
                if (pool[static_cast<std::size_t>(p)].x == nx &&
                    pool[static_cast<std::size_t>(p)].y == ny) {
                    already_tried = true;
                    break;
                }
            }
            if (already_tried) continue;

            // Allocate a node from the pool.
            if (free_head < 0) break;  // pool exhausted
            const int ni = free_head;
            free_head = pool[static_cast<std::size_t>(ni)].next;

            Node& nn = pool[static_cast<std::size_t>(ni)];
            nn.x = nx;
            nn.y = ny;
            nn.cost = node.cost + move_cost;
            nn.to_go =
                grid_distance(nx, ny, tx, ty) * kPathHeuristicMultiplier;
            nn.parent = cur;
            nn.next = -1;

            // Sorted insert into the open queue (AS_merge). Two
            // structural rules from the reference:
            //   * strict < everywhere — ties keep the earlier node;
            //   * a same-position entry already in the queue competes
            //     with the new one: the lower f survives, the loser is
            //     recycled (AS_merge moves the duplicate to waste).
            {
                const auto f_of = [&](int idx) {
                    return pool[static_cast<std::size_t>(idx)].cost +
                           pool[static_cast<std::size_t>(idx)].to_go;
                };
                const double f_new = nn.cost + nn.to_go;

                if (open_head < 0) {
                    // Queue was empty.
                    nn.next = -1;
                    open_head = ni;
                } else {
                    // Head duplicate (only reachable when the head's f
                    // is <= ours, i.e. the queue entry wins).
                    if (pool[static_cast<std::size_t>(open_head)].x == nx &&
                        pool[static_cast<std::size_t>(open_head)].y == ny) {
                        pool[static_cast<std::size_t>(ni)].next = free_head;
                        free_head = ni;
                        continue;
                    }
                    if (f_new < f_of(open_head)) {
                        nn.next = open_head;
                        open_head = ni;
                    } else {
                        // Scan from the head: find the same-position
                        // entry or the first node whose successor's f
                        // exceeds ours (insert between), whichever
                        // comes first.
                        int prev = open_head;
                        int dup = -1;
                        int insert_before = -1;  // ni goes before this node
                        while (true) {
                            const int nxt = pool[static_cast<std::size_t>(prev)].next;
                            if (nxt < 0) break;  // prev is the tail
                            if (pool[static_cast<std::size_t>(nxt)].x == nx &&
                                pool[static_cast<std::size_t>(nxt)].y == ny) {
                                dup = nxt;
                                break;
                            }
                            if (f_new < f_of(nxt)) {
                                insert_before = nxt;
                                break;
                            }
                            prev = nxt;
                        }
                        if (dup >= 0) {
                            const double f_dup = f_of(dup);
                            if (f_new >= f_dup) {
                                // Queue entry wins: recycle the new node.
                                pool[static_cast<std::size_t>(ni)].next = free_head;
                                free_head = ni;
                                continue;
                            }
                            // New node wins: unlink + recycle the
                            // duplicate, then insert the new node where
                            // the duplicate was (f_new < f_dup and the
                            // queue is sorted — the position is exact).
                            pool[static_cast<std::size_t>(prev)].next =
                                pool[static_cast<std::size_t>(dup)].next;
                            pool[static_cast<std::size_t>(dup)].next = free_head;
                            free_head = dup;
                            nn.next =
                                pool[static_cast<std::size_t>(prev)].next;
                            pool[static_cast<std::size_t>(prev)].next = ni;
                        } else if (insert_before >= 0) {
                            nn.next = insert_before;
                            pool[static_cast<std::size_t>(prev)].next = ni;
                        } else {
                            // Tail.
                            nn.next = -1;
                            pool[static_cast<std::size_t>(prev)].next = ni;
                        }
                    }
                }
            }
        }

        ++expanded;
    }
    // Budget exhaustion (node cap hit, or the pool ran dry — the
    // reference's `count < maxSearch and waste` loop condition).
    if (expanded >= max_search || free_head == -1) budget_hit = true;

    if (solution < 0 && budget_hit && !queue_empty &&
        (flags & static_cast<int>(PathFlags::PartialOnFail))) {
        // Best partial: lowest to_go among explored nodes with cost > 0
        // (the origin is excluded). The reference scans the open queue
        // first, then the tried list, strict <.
        double best = std::numeric_limits<double>::max();
        for (int t = open_head; t >= 0;
             t = pool[static_cast<std::size_t>(t)].next) {
            const Node& n = pool[static_cast<std::size_t>(t)];
            if (n.cost > 0.0 && n.to_go < best) {
                best = n.to_go;
                solution = t;
            }
        }
        for (int t = tried_head; t >= 0;
             t = pool[static_cast<std::size_t>(t)].next) {
            const Node& n = pool[static_cast<std::size_t>(t)];
            if (n.cost > 0.0 && n.to_go < best) {
                best = n.to_go;
                solution = t;
            }
        }
    }

    result.nodes_expanded = expanded;

    if (solution < 0) {
        return result;  // empty (RETURN_EMPTY_ON_FAIL semantics)
    }

    // Reconstruct the position chain (parent links — the reference
    // copies the bit-packed direction path; positions serve the route
    // builder directly).
    for (int p = solution; p >= 0; p = pool[static_cast<std::size_t>(p)].parent) {
        result.positions.emplace_back(pool[static_cast<std::size_t>(p)].x,
                                      pool[static_cast<std::size_t>(p)].y);
    }
    std::reverse(result.positions.begin(), result.positions.end());
    result.cost = pool[static_cast<std::size_t>(solution)].cost;
    result.complete =
        pool[static_cast<std::size_t>(solution)].x == tx &&
        pool[static_cast<std::size_t>(solution)].y == ty;
    result.partial = !result.complete;
    return result;
}

} // namespace f4::campaign
