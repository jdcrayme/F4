// f4-campaign/include/f4/campaign/war_verdict.hpp
//
// CAMP-DOM-1 — victory scoring over the books. Upstream tracks victory
// points in the campaign (the .cmp header's TE_VictoryPoints /
// TE_team_pts[8] — decoded, bridged, displayed, and never touched by an
// engine since); this repo "has books but no verdict" (plan §8). The
// verdict is a READ-ONLY projection of the books the engine already
// keeps — no new engine state, no accumulation, no save-format change:
//
//   * the territorial half: the LIVE owner of every objective (the
//     ground war's mirror when one runs, the WorldState otherwise)
//     against the session's OPENING owner (the snapshot taken at
//     session construction — the same run-scope the ledger's books
//     keep). Per team: objectives held now, gained (opening != slot,
//     live == slot — a neutral-origin objective counts for whoever
//     holds it), lost (opening == slot, live != slot — including
//     reverting to neutral), each weighted by the objective's own
//     priority byte (the wire's importance scale, the same number the
//     GTM tasking score reads).
//   * the attrition half: the ledger's own team books (air losses,
//     ground losses, battalions destroyed, objectives captured, the
//     aircraft pool's existence view) — reported, not folded into the
//     band.
//
// THE BAND is territorial only (so a capture is the only mover, and a
// verdict event is as sparse as the front moving):
//
//   stalemate  no team holds a net gain (the leader needs swing > 0)
//   advantage  the leader's net gained priority is 1..99
//   decisive   the leader's net gained priority >= kDecisiveSwing (100
//              — one top-priority objective's worth of territory)
//
// A tie for the lead is NO lead (an ambiguous verdict is not a verdict).
// Slot 0 is the neutral control value, never a row of its own; a named
// team at slot 0 keeps its ledger row for its books.
//
// The TE threshold (te_victory_points) rides along as reported context —
// korea's full-campaign save carries 0 (no threshold set), and no
// terminal ("the war is over") semantics are claimed for it here; a
// later tranche may wire the threshold into the band without touching
// any caller (the struct carries it from day one).
//
// C++20.

#pragma once

#include <f4/campaign/result_ledger.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace f4::campaign {

/// The band's wire words (band_name) — the vocabulary any UX speaks.
enum class VerdictBand : std::uint8_t {
    Stalemate = 0,
    Advantage = 1,
    Decisive = 2,
};

/// The decisive-swing threshold: a net gained priority of 100 — one
/// top-priority objective's worth of territory (the priority byte's own
/// ceiling). Documented constant; the tests pin the boundary.
inline constexpr int kDecisiveSwing = 100;

[[nodiscard]] const char* band_name(VerdictBand band) noexcept;

/// One objective, as the verdict reads it: the LIVE owner and the
/// objective's own priority. Wire order (the caller's walk order is the
/// census's only order — integer sums, so order cannot drift a byte).
struct VerdictObjective {
    std::uint8_t owner = 0;      ///< live owner (0 = neutral)
    std::uint8_t priority = 0;   ///< the wire's importance byte
};

/// One team's verdict row (slot order on the wire).
struct VerdictTeamRow {
    int slot = 0;
    std::string name;                 ///< the ledger's own team name
    // --- the territorial census ---------------------------------------
    int owned = 0;                    ///< objectives held now
    int gained = 0;                   ///< held now, not at opening
    int lost = 0;                     ///< held at opening, not now
    int gained_value = 0;             ///< Σ priority(gained)
    int lost_value = 0;               ///< Σ priority(lost)
    int swing = 0;                    ///< gained_value − lost_value
    // --- the ledger's own books (this run) ------------------------------
    int captures = 0;                 ///< TeamLedger::objectives_captured
    int air_losses = 0;               ///< TeamLedger::losses
    int ground_losses = 0;            ///< TeamLedger::ground_losses
    int battalions_destroyed = 0;     ///< TeamLedger::battalions_destroyed
    int aircraft_remaining = 0;       ///< the pool's existence view
};

/// The theater's verdict snapshot (the `verdict` query's engine truth).
struct TheaterVerdict {
    VerdictBand band = VerdictBand::Stalemate;
    int leader_slot = -1;             ///< -1 = no lead (stalemate or tie)
    int leader_swing = 0;             ///< the leader's swing (0 when none)
    /// The .cmp header's own threshold, reported as context (0 = the
    /// save sets none). The band does NOT read it (see the header note).
    int victory_threshold = 0;
    std::vector<VerdictTeamRow> teams;  ///< slot order
};

/// Compute the verdict (the header's model). `live` is every objective
/// in wire order; `opening_owners` is the session's opening owner per
/// objective in the SAME order (a shorter vector reads as neutral for
/// its missing tail — a defensive default, never hit by the session).
/// `belligerent_slots` forces a row for each war-carrying slot even
/// when its books are empty (Campaign::belligerent_teams' set); every
/// ledger team row joins too, plus any slot the census finds holding or
/// having opened with territory (slot 0 excepted).
[[nodiscard]] TheaterVerdict compute_theater_verdict(
    const std::vector<VerdictObjective>& live,
    const std::vector<std::uint8_t>& opening_owners,
    const CampaignResultLedger& ledger,
    const std::vector<int>& belligerent_slots,
    int victory_points_threshold);

} // namespace f4::campaign
