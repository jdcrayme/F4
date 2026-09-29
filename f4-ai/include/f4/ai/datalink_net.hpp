// f4-ai/include/f4/ai/datalink_net.hpp
//
// DatalinkNet — the per-team GCI broadcast state (AI_IMPLEMENTATION_PLAN
// §15 Step 13, the DatalinkTier).
//
// The legacy GCI rule ("GCI sees everything within the theater by
// definition", sensor_fusion.hpp) makes every brain on every team hold an
// omniscient theater rumor. Real Falcon GCI is a NETWORK: AWACS/JSTARS
// aircraft and ground radar sites detect geometrically and broadcast to
// their own team; when a team's nodes die, its picture decays. This type
// is that network as HOST-BUILT DATA — the AirPicture discipline (PERF-1)
// extended one column:
//
//   the host walks the world once per picture walk (the same single walk
//   that builds AirPicture), collects the live datalink nodes, and fills
//   one bitmask per contact: which TEAMS' datalinks see it. The fusion
//   consumes the net beside its DetectionPolicy; absent net = the legacy
//   omniscient leg, byte-identical (the standard twin-test).
//
// The type is deliberately dependency-light — f4::geo positions, a team
// mirror of AirPicture::teams, plain bitmasks. No EntityWorld, no
// components, no f4-sensors: the picture is DATA, and so is the net.
//
// v1 geometry is the plan's "range by altitude band, flat-earth simple":
// a node sees a contact when the horizontal range is within the node's
// radius and the contact's MSL altitude clears the node's horizon clamp.
// No beam physics — the node is a SENTINEL, not a fighter; it does not
// need the FCR model. Dead nodes leave the host's walk the same tick;
// with NO live nodes anywhere the host unwires the net and the legacy
// leg stands (the plan's "gate off OR no live nodes" rule — a scenario
// that arms the datalink without any datalink asset keeps omniscience;
// the operator-lag decay window is a named v2 tranche).

#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <f4/geo/position.hpp>

namespace f4::ai {

/// Feet per nautical mile (f4::geo's own constant, mirrored here so the
/// header stays dependency-light — the value is definitional, not
/// tunable).
inline constexpr double kDatalinkFeetPerNm = 6076.115485;

/// One datalink node: a live entity whose detection geometry other
/// same-team brains may borrow. Stamped by the host at spawn for
/// datalink-type aircraft (AWACS/JSTARS — the campaign's support
/// missions) and for radar-bearing ground objectives (the ground-site
/// arm). Plain state; the host owns the lifetime.
struct DatalinkNode {
    std::uint64_t entity_id{0};
    /// The node's team as an index into DatalinkNet::teams (the mirror
    /// of AirPicture::teams). -1 = untagged (broadcasts nothing).
    std::int16_t team{-1};
    f4::geo::WorldPosition position{};   ///< ENU feet, z = MSL
    /// Detection radius (horizontal, nautical miles). The documented
    /// v1 default — the plan's own DatalinkNode default.
    double range_nm{200.0};
    /// Horizon clamp: contacts BELOW this MSL altitude are not seen
    /// (flat-earth simple v1; the radar-horizon geometry is a v2 note).
    double min_alt_ft{0.0};
    /// Ground radar site (a radar-bearing objective) vs airborne node
    /// (an AWACS-type aircraft). Informational — the geometry is the
    /// same in v1; the distinction serves the tests and future arms.
    bool is_ground_site{false};
};

/// v1 node geometry — the plan's "range by altitude band, flat-earth
/// simple": a node sees a contact when the HORIZONTAL range is within
/// the node's radius and the contact's MSL altitude clears the node's
/// horizon clamp. Pure function: the host's picture walk and the tests
/// share this one body (one source of truth for the sentinel's eyes —
/// a future beam-physics leg replaces the body, not the call sites).
[[nodiscard]] inline bool node_sees(
    const DatalinkNode& node,
    const f4::geo::WorldPosition& contact) noexcept {
    const double dx = contact.x - node.position.x;
    const double dy = contact.y - node.position.y;
    const double horiz_ft = std::sqrt(dx * dx + dy * dy);
    if (horiz_ft > node.range_nm * kDatalinkFeetPerNm) return false;
    if (contact.z < node.min_alt_ft) return false;
    return true;
}

/// The per-tick net state the host maintains. For each air-picture
/// contact: which TEAMS' datalinks see it (bitmask over the team table
/// the host mirrored from AirPicture::teams). The fusion reads it with
/// seen_by_entity(); the picture-index form (seen_by) stays for hosts
/// that consume the net directly.
struct DatalinkNet {
    /// Live nodes this tick (the host's walk; dead nodes leave).
    std::vector<DatalinkNode> nodes;
    /// The team table the contact bits index — the host copies
    /// AirPicture::teams here so the fusion resolves its own team by
    /// the same strings the contacts carry.
    std::vector<std::string> teams;
    /// Parallel to AirPicture::contacts: bit t of contact_seen_teams[i]
    /// = some node of team t sees contact i. Bits 0..30 valid (a team
    /// index is std::int16_t; the mask is built only for 0 <= t < 31).
    std::vector<std::uint32_t> contact_seen_teams;
    /// entity_id -> contact index (the host fills alongside the mask —
    /// the fusion's consumption seam: emplace_target knows the id, not
    /// the picture index, and both rebuild paths share the call).
    std::unordered_map<std::uint64_t, std::size_t> contact_index_by_entity;

    /// True when team `team`'s datalink sees contact `contact_idx`.
    /// Out-of-range inputs read false (a contact the host never masked
    /// — e.g. an entity that joined between walks — is simply not
    /// GCI-seen; the next walk picks it up).
    [[nodiscard]] bool seen_by(std::size_t contact_idx,
                               std::int16_t team) const noexcept {
        if (team < 0 || team >= 31) return false;
        if (contact_idx >= contact_seen_teams.size()) return false;
        return (contact_seen_teams[contact_idx] &
                (1u << static_cast<unsigned>(team))) != 0u;
    }

    /// The fusion's seam: same query keyed by the contact's entity id.
    /// Unknown ids (not in the last walk) read false.
    [[nodiscard]] bool seen_by_entity(std::uint64_t entity_id,
                                      std::int16_t team) const noexcept {
        const auto it = contact_index_by_entity.find(entity_id);
        if (it == contact_index_by_entity.end()) return false;
        return seen_by(it->second, team);
    }

    /// The ownship's team index (linear scan — the table mirrors
    /// AirPicture::teams, a handful of entries). -1 when the team is
    /// not in the table (the fusion's GCI leg goes dark for a brain
    /// whose team the net does not know).
    [[nodiscard]] std::int16_t team_index(
        const std::string& name) const noexcept {
        for (std::size_t i = 0; i < teams.size(); ++i) {
            if (teams[i] == name) {
                return static_cast<std::int16_t>(i);
            }
        }
        return -1;
    }
};

} // namespace f4::ai
