// f4-campaign/src/war_verdict.cpp
//
// CAMP-DOM-1 — the verdict's model, over the books (see the header).
// Pure function: no engine state, no RNG, no clock — the caller owns
// when to ask and what "live" means (the ground war's mirror when one
// runs, the WorldState otherwise).

#include <f4/campaign/war_verdict.hpp>

#include <algorithm>
#include <unordered_map>

namespace f4::campaign {

const char* band_name(VerdictBand band) noexcept {
    switch (band) {
        case VerdictBand::Advantage: return "advantage";
        case VerdictBand::Decisive:  return "decisive";
        case VerdictBand::Stalemate: break;
    }
    return "stalemate";
}

namespace {

/// The census: every slot that must appear on the verdict. A ledger row
/// (named teams — the books exist), a belligerent slot (a war-carrying
/// team with empty books still deserves its row), or a slot the
/// objective walk finds holding or having opened with territory. Slot 0
/// is the neutral control value — never a census row of its own.
class RowSet {
public:
    explicit RowSet(const CampaignResultLedger& ledger) {
        for (const auto& t : ledger.teams()) {
            if (t.slot == 0 && t.name.empty()) continue;  // neutral
            VerdictTeamRow row;
            row.slot = t.slot;
            row.name = t.name;
            row.captures = t.objectives_captured;
            row.air_losses = t.losses;
            row.ground_losses = t.ground_losses;
            row.battalions_destroyed = t.battalions_destroyed;
            row.aircraft_remaining = t.aircraft_remaining;
            index_.emplace(t.slot, rows_.size());
            rows_.push_back(std::move(row));
        }
    }

    /// The row for `slot` (creating it when absent; slot 0 — the
    /// neutral control value — has no row, reads on a scratch row).
    [[nodiscard]] VerdictTeamRow& ensure(int slot) {
        const auto it = index_.find(slot);
        if (it != index_.end()) return rows_[it->second];
        if (slot <= 0) return scratch_;  // never written; reads discard
        index_.emplace(slot, rows_.size());
        VerdictTeamRow row;
        row.slot = slot;
        rows_.push_back(row);
        return rows_.back();
    }

    [[nodiscard]] std::vector<VerdictTeamRow> take() {
        std::sort(rows_.begin(), rows_.end(),
                  [](const VerdictTeamRow& a, const VerdictTeamRow& b) {
                      return a.slot < b.slot;
                  });
        return std::move(rows_);
    }

private:
    std::vector<VerdictTeamRow> rows_;
    std::unordered_map<int, std::size_t> index_;
    VerdictTeamRow scratch_;  ///< slot <= 0 writes land here, unread
};

} // namespace

TheaterVerdict compute_theater_verdict(
        const std::vector<VerdictObjective>& live,
        const std::vector<std::uint8_t>& opening_owners,
        const CampaignResultLedger& ledger,
        const std::vector<int>& belligerent_slots,
        int victory_points_threshold) {
    RowSet rows(ledger);
    for (const auto slot : belligerent_slots) (void)rows.ensure(slot);

    // The territorial census — one walk, every rule at once:
    //   live == opening  holds (the static front)
    //   live != opening, live != 0    a GAIN for the live holder
    //   opening != 0, live != opening a LOSS for the opening holder
    // (an objective that reverted to neutral is its opening owner's
    // loss; a neutral-origin objective is its live holder's gain.)
    for (std::size_t i = 0; i < live.size(); ++i) {
        const auto owner = live[i].owner;
        const auto opening = i < opening_owners.size()
                                 ? opening_owners[i]
                                 : static_cast<std::uint8_t>(0);
        if (owner == opening) {
            if (owner != 0) rows.ensure(owner).owned += 1;
            continue;
        }
        if (owner != 0) {
            auto& r = rows.ensure(owner);
            r.owned += 1;
            r.gained += 1;
            r.gained_value += live[i].priority;
        }
        if (opening != 0) {
            auto& r = rows.ensure(opening);
            r.lost += 1;
            r.lost_value += live[i].priority;
        }
    }

    auto verdict_rows = rows.take();
    int max_swing = 0;
    int leader = -1;
    bool tie = false;
    for (auto& r : verdict_rows) {
        r.swing = r.gained_value - r.lost_value;
        if (r.swing > max_swing) {
            max_swing = r.swing;
            leader = r.slot;
            tie = false;
        } else if (r.swing == max_swing && r.swing > 0) {
            tie = true;  // an equal lead is no lead
        }
    }

    TheaterVerdict v;
    v.victory_threshold = victory_points_threshold;
    v.teams = std::move(verdict_rows);
    if (leader >= 0 && !tie) {
        v.leader_slot = leader;
        v.leader_swing = max_swing;
        v.band = max_swing >= kDecisiveSwing ? VerdictBand::Decisive
                                             : VerdictBand::Advantage;
    }
    return v;
}

} // namespace f4::campaign
