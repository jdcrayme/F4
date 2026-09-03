// f4-campaign/include/f4/campaign/route_builder.hpp
//
// RouteBuilder — the C3 route builder (FreeFalcon campwp/mission.cpp's
// BuildPathToTarget + SetupIngressPoints + AddAttackProfile +
// SetupEgressPoints + EliminateExcessWaypoints, the air slice).
//
// B.3 flights fly SAVED routes (the save's WaypointStruct list, built
// by FreeFalcon's own ATM run). Synthetic-ladder missions had nothing
// to fly — "generation-to-spawn is the C3/C4 route tranche". This is
// that tranche's route half: the campaign now BUILDS the route it
// tasks, from the airbase, around the threat map, to the target, and
// home.
//
// The reference shape, per flight (simplified exactly where our data
// ends, each documented in the .cpp):
//
//   takeoff WP (airbase)
//   ingress:   CheckSafePath — max threat along the DIRECT leg
//              (ScoreThreatsOnWPLeg, TT_MAX, MAP_RATIO samples); above
//              MIN_AVOID_THREAT → FindSafePath (the grid A*), whose
//              corner waypoints become WP_NOTHING legs. Threat shaping
//              is COST-based this slice (the reference's no-fly walls
//              need Neutral/Hostile-but-not-war relations our stance
//              vocabulary cannot express — see threat_map.hpp); the
//              A* still prices every SAM ring it crosses.
//   target area (TPROF_ATTACK): IP at BREAKPOINT_DISTANCE from the
//              target (AddDistanceWaypoint), the target WP (the
//              profile's target action + the objective's VU), and a
//              turn point past the target picked by the reference's
//              5-candidate lowest-threat scan
//   egress:    the ingress shape in reverse, toward the landing WP
//   landing WP (airbase)
//   then EliminateExcessWaypoints — the two-pass cleanup (collinear
//   cut, then threat-reduction cut) over WP_NOTHING fillers only.
//
// What is deliberately NOT here yet (each lands with its consumer):
//   * package-shared assembly/breakpoints (SetupIngressPoints stores
//     them on the Package — one package, one ingress; C4's ATM
//     package composition owns that sharing);
//   * TOT/speed timing (SetWPTimes — the C4 TOT-slot tranche);
//   * per-action altitude shaping beyond mission-altitude legs
//     (CheckBestAltitude's 5-point band sampling; M4.5);
//   * loiter/sweep racetrack patterns (TPROF_LOITER gets the target
//     WP; the repeat pattern is the loiter tranche);
//   * tanker waypoints (fuel planning tranche).
//
// Determinism: a pure function of (threat map, objectives, profile,
// endpoints) — the reference's own RNG-free path (its randomness lives
// in target SELECTION, which is not this class's job).
//
// Dependencies: f4-campaign (ThreatMap, AirPathFinder, MissionProfile),
// f4-world (IObjectiveSource). C++20.

#pragma once

#include <f4/campaign/mission_profile.hpp>
#include <f4/campaign/path_finder.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace f4::campaign {

/// One planned waypoint — the campaign's route contract with the
/// sim-side spawner. Grid coordinates (the wire's own frame), altitude
/// in feet, the WP_ACTION byte (the same vocabulary saved routes use:
/// 1=TAKEOFF, 7=LAND, 17=STRIKE, ...), and the WPF flag bits that mark
/// waypoints the eliminator must not remove.
struct RouteWaypoint {
    int16_t x = 0;            ///< grid column
    int16_t y = 0;            ///< grid row
    int32_t altitude_ft = 0;  ///< MSL feet
    std::uint8_t action = 0;  ///< WP_ACTION (campwp.h vocabulary)
    std::uint16_t flags = 0;  ///< WPF_* bits (IP/TARGET/TURNPOINT/...)
    std::uint32_t target_num = 0;  ///< target objective VU_ID.num
                                   ///< (delivery waypoints)

    /// Element-wise equality (MissionIntent's own comparison needs it).
    bool operator==(const RouteWaypoint&) const = default;
};

/// WPF_* constants consumed by the builder and the eliminator
/// (campwp.h values — the saved-route vocabulary).
inline constexpr std::uint16_t kWpfTarget = 0x0001;
inline constexpr std::uint16_t kWpfAssemble = 0x0002;
inline constexpr std::uint16_t kWpfBreakpoint = 0x0004;
inline constexpr std::uint16_t kWpfIp = 0x0008;
inline constexpr std::uint16_t kWpfTurnPoint = 0x0010;
inline constexpr std::uint16_t kWpfTakeoff = 0x0080;
inline constexpr std::uint16_t kWpfLand = 0x0100;
inline constexpr std::uint16_t kWpfCriticalMask = 0x07FF;

/// WP_ACTION bytes the builder emits (campwp.h vocabulary).
inline constexpr std::uint8_t kWpNothing = 0;
inline constexpr std::uint8_t kWpTakeoff = 1;
inline constexpr std::uint8_t kWpAssemble = 2;
inline constexpr std::uint8_t kWpLand = 7;

/// Does this profile's route DELIVER ordnance on an objective? The C3
/// arming rule: only the A-G delivery family (STRIKE/BOMB/GNDSTRIKE/
/// NAVSTRIKE/SAD/SEAD target actions) gets an enemy-objective route —
/// CAP-style profiles (BARCAP etc.) also target OBJECTIVE, but their
/// objective is FRIENDLY airspace and their route shape is the
/// racetrack pattern (the loiter tranche), not an ingress-egress
/// strike route. Data-driven: the profile's own target action.
[[nodiscard]] inline bool profile_flies_delivery_route(
        const MissionProfile& profile) noexcept {
    if (profile.target != "OBJECTIVE") return false;
    const std::string& s = profile.targetwp;
    return s == "WP_STRIKE" || s == "WP_BOMB" || s == "WP_GNDSTRIKE" ||
           s == "WP_NAVSTRIKE" || s == "WP_SAD" || s == "WP_SEAD";
}

/// Tunables — FreeFalcon reads these from aiinput.dat ([ATM] section;
/// aiinput.cpp's GetPrivateProfileInt keys), which is game data, not
/// source. Defaults are the documented reference values; hosts with a
/// Falcon4.AII override them (campaign_qc does).
struct RouteBuilderConfig {
    /// ScoreThreatsOnWPLeg TT_MAX above this triggers the safe-path
    /// search (aiinput key MinAvoidThreat).
    int min_avoid_threat = 40;
    /// Assembly-point distance from the target (MinAssemblyPtDist).
    int min_ap_distance = 15;
    /// IP distance from the target (BreakpointDist).
    int breakpoint_distance = 10;
    /// A* node budget for one safe-path search (AirPathMax; bounded by
    /// the 2000-node pool regardless).
    int air_path_max = 2000;
};

/// One route build's outcome + QC counters.
struct RouteBuildResult {
    /// The route: takeoff, [ingress legs], [IP], target, [turn point],
    /// [egress legs], landing.
    std::vector<RouteWaypoint> waypoints;

    /// The direct airbase→target leg exceeded the avoid threshold and
    /// the A* ran (ingress or egress).
    bool safe_path_searched = false;
    /// The A* reached the target exactly (ingress).
    bool ingress_complete = false;
    /// The A* reached the airbase exactly (egress).
    bool egress_complete = false;
    /// The safe-path search failed to converge (3 partial passes, the
    /// reference's passes limit) and the leg fell back to the direct
    /// line (a documented deviation: the reference aborts the mission
    /// route; our host prefers a straight unsafe leg to no aircraft —
    /// counted loudly so QC sees it).
    bool direct_fallback = false;
    /// Waypoints removed by the eliminator.
    int eliminated = 0;
    /// Legs actually flown: waypoint-to-waypoint distances (grid units),
    /// the QC artifact's route length.
    int route_length_grid = 0;
};

/// Builds routes over one threat map. The map (and the A* over it) is
/// built ONCE at construction — FreeFalcon rebuilds its maps on a
/// campaign-update cadence when emitters move; this slice's hosts have
/// static saved dispositions, so one map per run is the honest
/// granularity (rebuild-on-event is a later host concern).
class RouteBuilder {
public:
    /// \param objectives objective data (airbase/target resolution)
    /// \param units      unit roster (feeds the threat map)
    /// \param teams      team stance (feeds the threat map)
    /// \param viewer     the team whose perspective the threat map
    ///                   packs (the campaign's player-side team)
    RouteBuilder(const f4::world::IObjectiveSource& objectives,
                 const f4::world::IUnitCoreSource& units,
                 const f4::world::ITeamSource& teams,
                 std::uint8_t viewer,
                 RouteBuilderConfig cfg = {});

    /// Build the route for one mission.
    /// \param team        the flying team (RoE/threat perspective)
    /// \param profile     the mission's profile (altitudes, target
    ///                    action, target profile)
    /// \param airbase_vu  home airbase objective VU_ID.num
    /// \param target_vu   target objective VU_ID.num (0 = route-only
    ///                    mission: takeoff → land circuit)
    [[nodiscard]] RouteBuildResult
    build(std::uint8_t team, const MissionProfile& profile,
          std::uint32_t airbase_vu, std::uint32_t target_vu) const;

    /// Access the threat map (QC/telemetry).
    [[nodiscard]] const ThreatMap& threat_map() const noexcept {
        return map_;
    }

private:
    /// Resolve an objective VU_ID.num to grid coordinates. nullopt when
    /// no objective carries the id.
    [[nodiscard]] std::optional<std::pair<int, int>>
    objective_xy_(std::uint32_t vu_num) const;

    /// ScoreThreatsOnWPLeg port: the worst (TT_MAX) threat along the
    /// direct (x1,y1)→(x2,y2) leg at `alt`, sampled every MAP_RATIO
    /// grid units.
    [[nodiscard]] int leg_threat_max_(int x1, int y1, int x2, int y2,
                                      std::uint8_t team,
                                      AltBand alt) const;

    /// FindSafePath port: the A* corner waypoints between two points,
    /// with the reference's multi-pass partial-progress loop (up to 3
    /// passes — `passes > 1` fails on the third fill). Fills
    /// `out` with WP_NOTHING waypoints; returns whether the target was
    /// reached exactly.
    bool safe_path_(int x1, int y1, int x2, int y2, std::uint8_t team,
                    AltBand alt, bool& searched, bool& fallback,
                    std::vector<RouteWaypoint>& out) const;

    /// AddDistanceWaypoint port: the first point along the
    /// (x1,y1)→(x2,y2) line within `distance` of (x2,y2). Returns
    /// nullopt when the line is degenerate or the point would sit
    /// within 3 grid units of the start (the reference returns the
    /// start waypoint itself — no insertion).
    [[nodiscard]] std::optional<std::pair<int, int>>
    distance_point_(int x1, int y1, int x2, int y2, int distance) const;

    /// The 5-candidate turn-point scan past the target
    /// (AddAttackProfile's WPF_TURNPOINT block): headings around the
    /// inbound direction at max(3, breakpoint/4) offset (0.707x when
    /// diagonal), lowest 2-sample threat score wins, ties resolve
    /// toward the home airbase.
    [[nodiscard]] std::pair<int, int>
    turn_point_(int ipx, int ipy, int tx, int ty, std::uint8_t team,
                AltBand alt, int home_x, int home_y) const;

    /// EliminateExcessWaypoints port (the two-pass cleanup, restricted
    /// to WP_NOTHING fillers without critical flags).
    void eliminate_excess_(std::vector<RouteWaypoint>& wps,
                           std::uint8_t team, AltBand alt,
                           int& removed) const;

    f4::world::IObjectiveSource const* objectives_ = nullptr;
    RouteBuilderConfig cfg_;
    ThreatMap map_;
    AirPathFinder finder_;
};

} // namespace f4::campaign
