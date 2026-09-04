// f4-campaign/src/route_builder.cpp
//
// RouteBuilder implementation — see route_builder.hpp for the
// campwp/mission.cpp correspondence. Deviations from the reference are
// marked (DEV) with their reason.

#include "f4/campaign/route_builder.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace f4::campaign {

namespace {

// 8-direction offsets + the turn-point candidate deltas
// (campaign.cpp dx/dy, mission.cpp HDelta[5]).
constexpr int kDx[8] = {0, 1, 1, 1, 0, -1, -1, -1};
constexpr int kDy[8] = {1, 1, 0, -1, -1, -1, 0, 1};
constexpr int kHDelta[5] = {0, 1, -1, 2, -2};

// Collinearity thresholds (mission.cpp's COS_10 / COS_120).
constexpr double kCos10 = 0.984807753012208;   // cos(10 deg)
constexpr double kCos120 = -0.5;               // cos(120 deg)

// DirectionTo (find.cpp): octant heading from (ox,oy) to (tx,ty),
// 22.5-degree-shifted so boundaries fall mid-sector. Heading 0 = +Y
// (north), atan2(dx, dy).
int direction_to(int ox, int oy, int tx, int ty) {
    const int ddx = tx - ox;
    const int ddy = ty - oy;
    if (ddx == 0 && ddy == 0) return 8;  // Here
    double deg = std::atan2(static_cast<double>(ddx),
                            static_cast<double>(ddy));
    if (deg < 0.0) deg += 6.283185307179586;
    deg += 0.3839723910;  // +22 degrees (the reference's .3839F)
    int h = static_cast<int>(deg * 1.273239544) % 8;
    return h;
}

double grid_distance(int ax, int ay, int bx, int by) {
    return std::sqrt(static_cast<double>((ax - bx) * (ax - bx) +
                                         (ay - by) * (ay - by)));
}

// The target waypoint action for a profile: the profile's targetwp
// string is the wire vocabulary ("WP_STRIKE", ...); map the ones the
// generated table carries, everything else falls back to WP_NOTHING
// (a nav leg — honest for patrol-style missions whose pattern is a
// later tranche).
//
// G2: WP_CAS (the mission table's own string for the CAS family — the
// reference's MissionData row, NOT a campwp enum byte) maps to the
// WP_GNDSTRIKE delivery action: CAS delivers iron on ground units, and
// the wire action vocabulary has no distinct CAS byte — the target
// point of a CAS mission IS a ground-strike delivery waypoint, the
// action the brain's strike rung arms on.
std::uint8_t target_action_for(const MissionProfile& profile) {
    const std::string& s = profile.targetwp;
    if (s == "WP_STRIKE")   return 17;
    if (s == "WP_BOMB")     return 18;
    if (s == "WP_GNDSTRIKE") return 14;
    if (s == "WP_NAVSTRIKE") return 15;
    if (s == "WP_SEAD")     return 19;
    if (s == "WP_SAD")      return 16;
    if (s == "WP_CAP")      return 12;
    if (s == "WP_INTERCEPT") return 13;
    if (s == "WP_RECON")    return 21;
    if (s == "WP_ELINT")    return 20;
    if (s == "WP_CAS")      return 14;   // G2: the CAS delivery action
    return kWpNothing;
}

} // namespace

RouteBuilder::RouteBuilder(const f4::world::IObjectiveSource& objectives,
                           const f4::world::IUnitCoreSource& units,
                           const f4::world::ITeamSource& teams,
                           std::uint8_t viewer,
                           RouteBuilderConfig cfg)
    : objectives_(&objectives),
      units_(&units),
      cfg_(cfg),
      map_(objectives, units, teams, viewer),
      finder_(map_, viewer) {}

std::optional<std::pair<int, int>>
RouteBuilder::objective_xy_(std::uint32_t vu_num) const {
    for (int i = 0; i < objectives_->objective_count(); ++i) {
        if (objectives_->id_num(i) == vu_num) {
            return std::make_pair(static_cast<int>(objectives_->x(i)),
                                  static_cast<int>(objectives_->y(i)));
        }
    }
    return std::nullopt;
}

std::optional<std::pair<int, int>>
RouteBuilder::unit_xy_(std::uint32_t vu_num) const {
    // G2 — a UNIT target's grid position: the aggregate battalion's
    // own coordinates from the unit source (the same source that feeds
    // the threat map). Wire order; the first id match wins (VU ids are
    // unique across the campaign's unit space).
    for (int i = 0; i < units_->unit_count(); ++i) {
        if (units_->id_num(i) == vu_num) {
            return std::make_pair(static_cast<int>(units_->x(i)),
                                  static_cast<int>(units_->y(i)));
        }
    }
    return std::nullopt;
}

RouteBuildResult RouteBuilder::build(std::uint8_t team,
                                     const MissionProfile& profile,
                                     std::uint32_t airbase_vu,
                                     std::uint32_t target_vu) const {
    RouteBuildResult result;

    // Resolve both endpoints FIRST — an unresolvable base or target
    // builds nothing (the reference's BuildPathToTarget aborts before
    // creating any waypoint). G2: the TARGET resolves objectives
    // first, then UNITS (a CAS mission's target is a battalion VU) —
    // the world loader's own resolution order for mission targets.
    const auto airbase_xy = objective_xy_(airbase_vu);
    if (!airbase_xy) return result;  // unresolvable base: no route
    const auto target_xy = target_vu != 0
        ? (objective_xy_(target_vu) ? objective_xy_(target_vu)
                                    : unit_xy_(target_vu))
        : std::nullopt;
    if (target_vu != 0 && !target_xy) return result;  // bad target

    const int ax = airbase_xy->first;
    const int ay = airbase_xy->second;

    // Mission altitude: the profile's altitudes are HUNDREDS of feet
    // (FreeFalcon convention); clamp the target altitude into the
    // profile's own [min, max] window (SetupAltitudes' clamp, minus the
    // vehicle-class ceiling we don't carry — DEV: vehicle data is the
    // M4.6 squadron-selection tranche's).
    const int mission_alt =
        std::clamp(profile.missionalt, profile.minalt, profile.maxalt) * 100;
    const AltBand mission_band = alt_band_from_feet(mission_alt);

    std::vector<RouteWaypoint>& wps = result.waypoints;

    // 1. Takeoff at the airbase.
    {
        RouteWaypoint w;
        w.x = static_cast<std::int16_t>(ax);
        w.y = static_cast<std::int16_t>(ay);
        w.altitude_ft = 0;
        w.action = kWpTakeoff;
        w.flags = kWpfTakeoff;
        wps.push_back(w);
    }

    // 2. Landing at the airbase (the terminal approach fix — the sim
    //    side's MissionPlan treats the last WP exactly like a saved
    //    route's WP_LAND).
    RouteWaypoint land;
    land.x = static_cast<std::int16_t>(ax);
    land.y = static_cast<std::int16_t>(ay);
    land.altitude_ft = 0;
    land.action = kWpLand;
    land.flags = kWpfLand;

    if (target_vu != 0) {
        const int tx = target_xy->first;
        const int ty = target_xy->second;

        // --- Ingress: CheckSafePath + FindSafePath (SetupIngressPoints)
        // ---
        // (DEV: the reference routes ingress toward the ASSEMBLY point
        // and lets the package share it; this slice routes to the IP
        // line directly — package sharing is C4's ATM composition.)
        std::vector<RouteWaypoint> ingress;
        bool searched = false, fallback = false;
        result.ingress_complete = safe_path_(ax, ay, tx, ty, team,
                                             mission_band, searched,
                                             fallback, ingress);
        result.safe_path_searched = result.safe_path_searched || searched;
        result.direct_fallback = result.direct_fallback || fallback;
        for (auto& w : ingress) w.altitude_ft = mission_alt;
        wps.insert(wps.end(), ingress.begin(), ingress.end());

        // --- Target area (AddAttackProfile, TPROF_ATTACK shape) ---
        // IP: the first point on the approach line within
        // breakpoint_distance of the target. The approach starts at the
        // route's current end (the last ingress corner, or the airbase
        // when the leg was safe/direct).
        int px = ax, py = ay;
        if (!ingress.empty()) {
            px = ingress.back().x;
            py = ingress.back().y;
        }
        if (profile.target_profile == "TPROF_ATTACK") {
            if (auto ip = distance_point_(px, py, tx, ty,
                                          cfg_.breakpoint_distance)) {
                RouteWaypoint w;
                w.x = static_cast<std::int16_t>(ip->first);
                w.y = static_cast<std::int16_t>(ip->second);
                w.altitude_ft = mission_alt;
                w.action = kWpNothing;
                w.flags = kWpfIp;
                wps.push_back(w);
                px = ip->first;
                py = ip->second;
            }

            // Target WP (the profile's action; the strike target's VU
            // rides on it — the sim side's StrikeModule arms on it).
            RouteWaypoint tw;
            tw.x = static_cast<std::int16_t>(tx);
            tw.y = static_cast<std::int16_t>(ty);
            tw.altitude_ft = mission_alt;
            tw.action = target_action_for(profile);
            tw.flags = kWpfTarget;
            tw.target_num = target_vu;
            wps.push_back(tw);

            // Turn point past the target (the 5-candidate scan).
            const auto tp = turn_point_(px, py, tx, ty, team,
                                        mission_band, ax, ay);
            RouteWaypoint tpw;
            tpw.x = static_cast<std::int16_t>(tp.first);
            tpw.y = static_cast<std::int16_t>(tp.second);
            tpw.altitude_ft = mission_alt;
            tpw.action = kWpNothing;
            tpw.flags = kWpfTurnPoint;
            wps.push_back(tpw);
            px = tp.first;
            py = tp.second;
        } else {
            // TPROF_LOITER and the rest: the target WP only (the
            // racetrack patterns are the loiter tranche).
            RouteWaypoint tw;
            tw.x = static_cast<std::int16_t>(tx);
            tw.y = static_cast<std::int16_t>(ty);
            tw.altitude_ft = mission_alt;
            tw.action = target_action_for(profile);
            tw.flags = kWpfTarget;
            tw.target_num = target_vu;
            wps.push_back(tw);
            px = tx;
            py = ty;
        }

        // --- Egress (SetupEgressPoints: the ingress shape in reverse,
        //     from the turn point home) ---
        std::vector<RouteWaypoint> egress;
        bool esearched = false, efallback = false;
        result.egress_complete = safe_path_(px, py, ax, ay, team,
                                            mission_band, esearched,
                                            efallback, egress);
        result.safe_path_searched = result.safe_path_searched || esearched;
        result.direct_fallback = result.direct_fallback || efallback;
        for (auto& w : egress) w.altitude_ft = mission_alt;
        wps.insert(wps.end(), egress.begin(), egress.end());
    }
    // target_vu == 0 (route-only circuit): takeoff → land, nothing
    // between (alert-training shapes get patterns with their tranche).

    wps.push_back(land);

    // --- EliminateExcessWaypoints (two-pass cleanup) ---
    eliminate_excess_(wps, team, mission_band, result.eliminated);

    // Route length (grid units) over the surviving legs.
    for (std::size_t i = 1; i < wps.size(); ++i) {
        result.route_length_grid += static_cast<int>(
            grid_distance(wps[i - 1].x, wps[i - 1].y, wps[i].x, wps[i].y));
    }

    return result;
}

int RouteBuilder::leg_threat_max_(int x1, int y1, int x2, int y2,
                                  std::uint8_t team,
                                  AltBand alt) const {
    // ScoreThreatsOnWPLeg (TT_MAX): sample every MAP_RATIO along the
    // leg. The altitude band: the mission band for every leg this
    // slice (the reference samples the leg's own waypoint altitudes —
    // per-leg shaping is M4.5).
    const double d = grid_distance(x1, y1, x2, y2);
    if (d <= 0.0) {
        return map_.score(x1, y1, alt, team);
    }
    const double xd = (x2 - x1) / d;
    const double yd = (y2 - y1) / d;
    int worst = 0;
    for (int step = 0; step <= static_cast<int>(d); step += kThreatMapRatio) {
        const int sx = x1 + static_cast<int>(xd * step + 0.5);
        const int sy = y1 + static_cast<int>(yd * step + 0.5);
        worst = std::max(worst, map_.score(sx, sy, alt, team));
    }
    return worst;
}

bool RouteBuilder::safe_path_(int x1, int y1, int x2, int y2,
                              std::uint8_t team, AltBand alt,
                              bool& searched, bool& fallback,
                              std::vector<RouteWaypoint>& out) const {
    // CheckSafePath: below the avoid threshold, the direct leg stands.
    if (leg_threat_max_(x1, y1, x2, y2, team, alt) <=
        cfg_.min_avoid_threat) {
        return true;  // direct, complete by definition
    }

    // FindSafePath: the A* loop. The reference allows three fills
    // (`passes > 1` fails on the third) — partial paths make progress
    // toward the target each pass.
    searched = true;
    const int flags = static_cast<int>(PathFlags::PartialOnFail) |
                      static_cast<int>(PathFlags::PartialOnMax);
    int cx = x1, cy = y1;
    bool complete = false;
    for (int pass = 0; pass < 3 && !(cx == x2 && cy == y2); ++pass) {
        const auto r = finder_.find(cx, cy, x2, y2, team, alt, flags,
                                    true, cfg_.air_path_max);
        if (r.positions.empty()) break;
        // FillAirPath: waypoints at path corners — a corner lands when
        // the run of same-direction steps preceding it is longer than
        // one step (single-step jags are absorbed; the eliminator would
        // remove them anyway). The positions the finder returns are
        // already the per-step trail; corners are the direction changes.
        const auto corners = [&]() {
            std::vector<std::pair<int, int>> c;
            for (std::size_t i = 1; i < r.positions.size(); ++i) {
                const auto& a = r.positions[i - 1];
                const auto& b = r.positions[i];
                if (a.first == b.first && a.second == b.second) continue;
                if (i + 1 < r.positions.size()) {
                    const auto& n2 = r.positions[i + 1];
                    const int d1 = direction_to(a.first, a.second,
                                                b.first, b.second);
                    const int d2 = direction_to(b.first, b.second,
                                                n2.first, n2.second);
                    if (d1 == d2) continue;  // same heading: not a corner
                }
                c.push_back(b);
            }
            return c;
        }();
        for (const auto& c : corners) {
            RouteWaypoint w;
            w.x = static_cast<std::int16_t>(c.first);
            w.y = static_cast<std::int16_t>(c.second);
            w.action = kWpNothing;
            out.push_back(w);
        }
        const auto& end = r.positions.back();
        cx = end.first;
        cy = end.second;
        complete = r.complete;
        if (complete) break;
    }

    if (!complete && !(cx == x2 && cy == y2)) {
        // The reference aborts the mission route here (BuildPathToTarget
        // returns 0). (DEV) our host prefers a straight unsafe leg to
        // no aircraft — counted loudly, visible in QC.
        fallback = true;
        return false;
    }
    return true;
}

std::optional<std::pair<int, int>>
RouteBuilder::distance_point_(int x1, int y1, int x2, int y2,
                              int distance) const {
    // AddDistanceWaypoint: walk the (x1,y1)→(x2,y2) line 1 grid unit at
    // a time; the first point within `distance` of (x2,y2) is the
    // waypoint. Skip when it would sit within 3 units of the start (the
    // reference returns the start WP — nothing inserted).
    const double d = grid_distance(x1, y1, x2, y2);
    if (d <= 0.0) return std::nullopt;
    const double xd = (x2 - x1) / d;
    const double yd = (y2 - y1) / d;
    const int dist = static_cast<int>(d);
    for (int step = 0; step <= dist; ++step) {
        const int cx = x1 + static_cast<int>(xd * step + 0.5);
        const int cy = y1 + static_cast<int>(yd * step + 0.5);
        if (grid_distance(cx, cy, x2, y2) <= distance) {
            if (step < 3) return std::nullopt;
            return std::make_pair(cx, cy);
        }
    }
    return std::nullopt;
}

std::pair<int, int> RouteBuilder::turn_point_(int ipx, int ipy, int tx,
                                              int ty, std::uint8_t team,
                                              AltBand alt, int home_x,
                                              int home_y) const {
    // AddAttackProfile's turn-point scan: five headings around the
    // inbound direction (fh + HDelta), each a candidate spot at
    // d = max(3, breakpoint/4) past the target (0.707x when the
    // heading is diagonal). Score = the 2-sample threat sum minus the
    // candidate index; lowest wins, ties prefer the spot nearer home.
    const int fh = direction_to(ipx, ipy, tx, ty);
    int bs = std::numeric_limits<int>::max();
    int bx = tx, by = ty;
    bool first = true;
    for (int i = 0; i < 5; ++i) {
        const int h = (fh + kHDelta[i] + 8) % 8;
        int d = std::max(3, cfg_.breakpoint_distance / 4);
        if (kDx[h] != 0 && kDy[h] != 0) {
            d = static_cast<int>(0.707 * d);
        }
        const int x = tx + kDx[h] * d;
        const int y = ty + kDy[h] * d;
        const int s = map_.score(x, y, alt, team) - i +
                      map_.score(x + kDx[h] * d, y + kDy[h] * d, alt, team);
        const bool better =
            s < bs || (s == bs && grid_distance(x, y, home_x, home_y) <
                                      grid_distance(bx, by, home_x, home_y));
        if (first || better) {
            bs = s;
            bx = x;
            by = y;
            first = false;
        }
    }
    return {bx, by};
}

void RouteBuilder::eliminate_excess_(
        std::vector<RouteWaypoint>& wps, std::uint8_t team, AltBand alt,
        int& removed) const {
    // EliminateExcessWaypoints, restricted to WP_NOTHING fillers
    // without critical flags (the reference's own eligibility test).
    // Two cuts, evaluated per middle waypoint:
    //   a) collinearity: the angle at the middle waypoint deviates
    //      more than 10 degrees from straight (cwm/cnm > COS_10) or
    //      doubles back past 120 degrees (< COS_120) — the middle
    //      waypoint is redundant geometry, cut it;
    //   b) threat: the direct w→nw leg's threat sum + distance is
    //      <= the two sub-legs' totals — the middle waypoint was not
    //      buying safety, cut it.
    // One forward pass (the reference loops until stable over the
    // waypoint list; a single pass per rebuild is what its own calls
    // converge to in practice — the second-order cuts the loop would
    // find are dominated by the threat re-check).
    if (wps.size() < 3) return;

    const auto threat_total = [&](int x1, int y1, int x2, int y2) {
        // TT_TOTAL: sum of the samples along the leg.
        const double d = grid_distance(x1, y1, x2, y2);
        if (d <= 0.0) return map_.score(x1, y1, alt, team);
        const double xd = (x2 - x1) / d;
        const double yd = (y2 - y1) / d;
        int total = 0;
        for (int step = 0; step <= static_cast<int>(d);
             step += kThreatMapRatio) {
            total += map_.score(x1 + static_cast<int>(xd * step + 0.5),
                                y1 + static_cast<int>(yd * step + 0.5),
                                alt, team);
        }
        return total;
    };

    for (std::size_t i = 1; i + 1 < wps.size();) {
        const RouteWaypoint& w = wps[i - 1];
        const RouteWaypoint& mw = wps[i];
        const RouteWaypoint& nw = wps[i + 1];
        if (mw.action != kWpNothing || (mw.flags & kWpfCriticalMask) != 0) {
            ++i;  // not a filler — never cut
            continue;
        }
        const int x = w.x, y = w.y;
        const int mx = mw.x, my = mw.y;
        const int nx = nw.x, ny = nw.y;

        // (a) collinearity — the dot-product cosines against the two
        // legs' unit vectors.
        const double wmd = grid_distance(x, y, mx, my);
        const double mnd = grid_distance(mx, my, nx, ny);
        const double wnd = grid_distance(x, y, nx, ny);
        bool cut = true;
        if (wmd > 0.0 && wnd > 0.0) {
            const double cwm =
                ((mx - x) * (nx - x) + (my - y) * (ny - y)) / (wmd * wnd);
            const double cnm =
                ((mx - nx) * (x - nx) + (my - ny) * (y - ny)) / (mnd * wnd);
            cut = (cwm > kCos10 || cwm < kCos120 || cnm > kCos10 ||
                   cnm < kCos120);
        }
        if (!cut) {
            // (b) threat reduction: is the middle waypoint buying
            // safety? (direct <= sub-legs → it is not)
            const int direct = threat_total(x, y, nx, ny) +
                               static_cast<int>(wnd);
            const int split = threat_total(x, y, mx, my) +
                              static_cast<int>(wmd) +
                              threat_total(mx, my, nx, ny) +
                              static_cast<int>(mnd);
            cut = direct <= split;
        }
        if (cut) {
            wps.erase(wps.begin() + static_cast<std::ptrdiff_t>(i));
            ++removed;
            // The reference advances w = mw on a cut-survivor; after a
            // CUT the new middle is the next waypoint — stay at i and
            // re-evaluate against the same left anchor.
        } else {
            ++i;
        }
    }
}

} // namespace f4::campaign
