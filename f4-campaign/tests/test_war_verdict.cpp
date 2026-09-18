// f4-campaign/tests/test_war_verdict.cpp
//
// CAMP-DOM-1 — the verdict's model, pinned over the make_ledger_world
// discipline: a hand-built WorldState where every number exposes one
// rule, a ledger snapshot for the books, and LIVE/OPENING arrays the
// test writes by hand (the session's ground-mirror-or-WorldState walk
// is the production caller; here the arrays ARE the truth):
//
//   1. A war at rest reads stalemate (the opening is the baseline).
//   2. A capture swings both sides of one objective (gain for the
//      taker, loss for the holder), weighted by the priority byte.
//   3. Neutral-origin territory counts for whoever holds it; territory
//      that reverts to neutral is its opening owner's loss.
//   4. The band: advantage below kDecisiveSwing, decisive at it — the
//      boundary is pinned at 99/100.
//   5. A tie for the lead is no lead (an ambiguous verdict is not a
//      verdict).
//   6. The books ride the row: captures, air losses, ground losses,
//      the aircraft pool's existence view.
//   7. The census: named teams, belligerents with empty books, and
//      territory-holding unnamed slots get rows; slot 0 never does.
//   8. Rows come out in slot order; the TE threshold is echoed.

#include <f4/campaign/war_verdict.hpp>
#include <f4/world/world_adapters.hpp>

#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <vector>

using namespace f4::campaign;
using f4::world::WorldState;
using f4::world::WorldStateAdapters;

namespace {

f4::world::TeamState team(int slot, const std::string& name,
                          std::vector<int16_t> stance) {
    f4::world::TeamState t;
    t.slot = slot;
    t.name = name;
    t.stance = std::move(stance);
    t.tea_loaded = true;
    return t;
}

f4::world::ObjectiveState objective(std::uint32_t vu,
                                    std::uint8_t owner,
                                    std::uint8_t priority = 30) {
    f4::world::ObjectiveState o;
    o.id_num = vu;
    o.owner = owner;
    o.first_owner = owner;   // the wire's own save-start semantics
    o.priority = priority;
    return o;
}

/// Two belligerents (2 ROK / 6 DPRK, mutual War rows), one named
/// neutral (3), the standard pool vector (slot 2 flies 40 aircraft).
/// The rest world holds one objective per side, at rest.
struct Rig {
    std::unique_ptr<WorldState> ws;
    std::unique_ptr<WorldStateAdapters> adapters;
    std::unique_ptr<CampaignResultLedger> ledger;
    std::vector<std::uint8_t> opening_owners_;  ///< the session-start
                                                ///< snapshot (pre-mutate)

    /// Two phases, the session's own shape: `shape` builds the world
    /// BEFORE the opening snapshot (extra objectives, neutral origins);
    /// `run` is the war's flips AFTER it (the ledger's run scope).
    static std::unique_ptr<Rig> make(
            const std::function<void(WorldState&)>& shape = nullptr,
            const std::function<void(WorldState&)>& run = nullptr) {
        auto r = std::make_unique<Rig>();
        r->ws = std::make_unique<WorldState>();
        r->ws->version = 71;
        r->ws->campaign.current_time = 1'000'000;
        r->ws->campaign.te_number_aircraft = {0, 0, 40, 0, 0, 0, 0, 0};
        std::vector<int16_t> rok{0, 0, 0, 3, 0, 0, 5, 0};
        std::vector<int16_t> dprk{0, 0, 5, 3, 0, 0, 0, 0};
        std::vector<int16_t> neutral{0, 0, 3, 0, 0, 0, 3, 0};
        r->ws->teams = {team(2, "ROK", rok), team(6, "DPRK", dprk),
                        team(3, "Neutralia", neutral)};
        r->ws->objectives = {
            objective(101, 2), objective(201, 6),   // one each, at rest
        };
        if (shape) shape(*r->ws);
        // The opening snapshot lands between the world's shaping and
        // the run's flips — the session's construction-time discipline.
        for (const auto& o : r->ws->objectives) {
            r->opening_owners_.push_back(o.owner);
        }
        if (run) run(*r->ws);
        r->adapters = std::make_unique<WorldStateAdapters>(*r->ws);
        r->ledger = std::make_unique<CampaignResultLedger>(
            r->adapters->campaign, r->adapters->teams, r->adapters->units);
        return r;
    }

    /// The LIVE view the session would build (owner + priority, wire
    /// order) — over the WorldState here; the ground mirror walk has
    /// the same shape in the session.
    [[nodiscard]] std::vector<VerdictObjective> live() const {
        std::vector<VerdictObjective> v;
        for (const auto& o : ws->objectives) {
            v.push_back(VerdictObjective{o.owner, o.priority});
        }
        return v;
    }

    /// The OPENING owners (the construction-time snapshot — the
    /// baseline the run's swings are measured against).
    [[nodiscard]] const std::vector<std::uint8_t>& opening() const {
        return opening_owners_;
    }

    [[nodiscard]] TheaterVerdict verdict(
            const std::vector<VerdictObjective>& live_view,
            const std::vector<std::uint8_t>& opening_view,
            int threshold = 0) const {
        return compute_theater_verdict(live_view, opening_view, *ledger,
                                       {2, 6}, threshold);
    }

    [[nodiscard]] const VerdictTeamRow* row(const TheaterVerdict& v,
                                            int slot) const {
        for (const auto& r : v.teams) {
            if (r.slot == slot) return &r;
        }
        return nullptr;
    }
};

} // namespace

// ── 1. the war at rest ─────────────────────────────────────────────────────

TEST(WarVerdict, WarAtRestReadsStalemate) {
    const auto rig = Rig::make();
    const auto v = rig->verdict(rig->live(), rig->opening());
    EXPECT_EQ(v.band, VerdictBand::Stalemate);
    EXPECT_EQ(v.leader_slot, -1);
    EXPECT_EQ(v.leader_swing, 0);
    ASSERT_EQ(v.teams.size(), 3u);  // ROK, DPRK, Neutralia (slot order)
    EXPECT_EQ(v.teams[0].slot, 2);
    EXPECT_EQ(v.teams[0].name, "ROK");
    EXPECT_EQ(v.teams[0].owned, 1);  // the census still counts holdings
    EXPECT_EQ(v.teams[0].gained, 0);
    EXPECT_EQ(v.teams[0].lost, 0);
    EXPECT_EQ(v.teams[0].swing, 0);
    EXPECT_EQ(v.teams[1].slot, 3);   // Neutralia sorts between
    EXPECT_EQ(v.teams[2].slot, 6);
    EXPECT_EQ(v.teams[2].owned, 1);
}

TEST(WarVerdict, ThresholdIsEchoedNotRead) {
    const auto rig = Rig::make();
    const auto v = rig->verdict(rig->live(), rig->opening(), 250);
    EXPECT_EQ(v.victory_threshold, 250);   // context, not a band input
    EXPECT_EQ(v.band, VerdictBand::Stalemate);
}

// ── 2. one capture, both sides ─────────────────────────────────────────────

TEST(WarVerdict, CaptureSwingsBothSidesByPriority) {
    // Objective 201 (DPRK-held, priority 30) flips to ROK after the
    // opening: ROK +30, DPRK −30 — the front moved one objective's
    // worth.
    const auto rig = Rig::make({}, [](WorldState& ws) {
        for (auto& o : ws.objectives) {
            if (o.id_num == 201) o.owner = 2;
        }
    });
    auto live = rig->live();
    rig->ledger->apply_objective_capture(3600.0, 201, 6, 2, 7001);
    const auto v = rig->verdict(live, rig->opening());

    const auto* rok = rig->row(v, 2);
    ASSERT_NE(rok, nullptr);
    EXPECT_EQ(rok->owned, 2);
    EXPECT_EQ(rok->gained, 1);
    EXPECT_EQ(rok->gained_value, 30);
    EXPECT_EQ(rok->swing, 30);
    EXPECT_EQ(rok->captures, 1);   // the ledger's own book rides too
    const auto* dprk = rig->row(v, 6);
    ASSERT_NE(dprk, nullptr);
    EXPECT_EQ(dprk->owned, 0);
    EXPECT_EQ(dprk->lost, 1);
    EXPECT_EQ(dprk->lost_value, 30);
    EXPECT_EQ(dprk->swing, -30);

    EXPECT_EQ(v.leader_slot, 2);
    EXPECT_EQ(v.leader_swing, 30);
    EXPECT_EQ(v.band, VerdictBand::Advantage);
    EXPECT_STREQ(band_name(v.band), "advantage");
}

TEST(WarVerdict, NeutralOriginCountsForTheHolder) {
    // An objective that opened neutral and is held by ROK now: the
    // census books it as ROK's gain (the holder's swing moved).
    const auto rig = Rig::make(
        [](WorldState& ws) {   // the world opens with 301 neutral
            ws.objectives.push_back(objective(301, 0, 10));
        },
        [](WorldState& ws) {   // ROK takes it as the war opens
            for (auto& o : ws.objectives) {
                if (o.id_num == 301) o.owner = 2;
            }
        });
    const auto v = rig->verdict(rig->live(), rig->opening());
    const auto* rok = rig->row(v, 2);
    ASSERT_NE(rok, nullptr);
    EXPECT_EQ(rok->gained, 1);
    EXPECT_EQ(rok->gained_value, 10);
    EXPECT_EQ(rok->swing, 10);
    EXPECT_EQ(v.leader_slot, 2);
    EXPECT_EQ(v.band, VerdictBand::Advantage);
}

TEST(WarVerdict, RevertToNeutralIsTheOpeningHoldersLoss) {
    // ROK-held at opening, owner 0 now: ROK lost it (value moves
    // against ROK; nobody GAINED — the front dissolved, no lead).
    const auto rig = Rig::make({}, [](WorldState& ws) {
        for (auto& o : ws.objectives) {
            if (o.id_num == 101) o.owner = 0;
        }
    });
    const auto v = rig->verdict(rig->live(), rig->opening());
    const auto* rok = rig->row(v, 2);
    ASSERT_NE(rok, nullptr);
    EXPECT_EQ(rok->owned, 0);
    EXPECT_EQ(rok->lost, 1);
    EXPECT_EQ(rok->lost_value, 30);
    EXPECT_EQ(rok->swing, -30);
    EXPECT_EQ(v.leader_slot, -1);
    EXPECT_EQ(v.band, VerdictBand::Stalemate);
}

// ── 4. the band's boundary ─────────────────────────────────────────────────

TEST(WarVerdict, DecisiveBoundaryPinnedAtKDecisiveSwing) {
    // swing 99 (one 99-priority flip): advantage. swing 100: decisive.
    {
        const auto rig = Rig::make({}, [](WorldState& ws) {
            for (auto& o : ws.objectives) {
                if (o.id_num == 201) {
                    o.owner = 2;
                    o.priority = 99;
                }
            }
        });
        const auto v = rig->verdict(rig->live(), rig->opening());
        EXPECT_EQ(v.leader_swing, 99);
        EXPECT_EQ(v.band, VerdictBand::Advantage);
    }
    {
        const auto rig = Rig::make({}, [](WorldState& ws) {
            for (auto& o : ws.objectives) {
                if (o.id_num == 201) {
                    o.owner = 2;
                    o.priority = 100;
                }
            }
        });
        const auto v = rig->verdict(rig->live(), rig->opening());
        EXPECT_EQ(v.leader_swing, 100);
        EXPECT_EQ(v.band, VerdictBand::Decisive);
        EXPECT_STREQ(band_name(v.band), "decisive");
    }
}

// ── 5. ties ────────────────────────────────────────────────────────────────

TEST(WarVerdict, TieForTheLeadIsNoLead) {
    // Both belligerents gain one neutral-origin priority-30 objective
    // and lose nothing: swings 30 and 30 — an equal lead is no lead.
    const auto rig = Rig::make(
        [](WorldState& ws) {   // both sides' gains open neutral
            ws.objectives.push_back(objective(301, 0, 30));
            ws.objectives.push_back(objective(401, 0, 30));
        },
        [](WorldState& ws) {   // each side takes one
            for (auto& o : ws.objectives) {
                if (o.id_num == 301) o.owner = 2;
                if (o.id_num == 401) o.owner = 6;
            }
        });
    const auto v = rig->verdict(rig->live(), rig->opening());
    ASSERT_NE(rig->row(v, 2), nullptr);
    EXPECT_EQ(rig->row(v, 2)->swing, 30);
    EXPECT_EQ(rig->row(v, 6)->swing, 30);
    EXPECT_EQ(v.leader_slot, -1);
    EXPECT_EQ(v.leader_swing, 0);
    EXPECT_EQ(v.band, VerdictBand::Stalemate);
}

// ── 6. the books ride the row ──────────────────────────────────────────────

TEST(WarVerdict, LedgerBooksRideTheRow) {
    const auto rig = Rig::make();
    // Air loss: a ROK aircraft dies (pool 40 → 39, the existence view).
    rig->ledger->apply_air_loss(120.0, 2, 4281, 5001, 0);
    // Ground loss: a DPRK battalion loses 4 vehicles to ROK's line.
    rig->ledger->apply_ground_loss(240.0, 6001, 6, 7001, 2, 4);
    const auto v = rig->verdict(rig->live(), rig->opening());
    const auto* rok = rig->row(v, 2);
    const auto* dprk = rig->row(v, 6);
    ASSERT_NE(rok, nullptr);
    ASSERT_NE(dprk, nullptr);
    EXPECT_EQ(rok->air_losses, 1);
    EXPECT_EQ(rok->aircraft_remaining, 39);
    EXPECT_EQ(dprk->ground_losses, 4);
    // The books alone never move the band (territorial only):
    EXPECT_EQ(v.band, VerdictBand::Stalemate);
    EXPECT_EQ(v.leader_slot, -1);
}

// ── 7. the census ──────────────────────────────────────────────────────────

TEST(WarVerdict, CensusRowsWithoutNamesJoinSlotZeroDoesNot) {
    // Slot 7 (unnamed, not a belligerent) holds a priority-20 objective
    // that opened neutral; slot 0 holds nothing and gets NO row. The
    // unnamed row still sorts by slot and reads empty-named.
    const auto rig = Rig::make(
        [](WorldState& ws) {   // 401 opens neutral
            ws.objectives.push_back(objective(401, 0, 20));
        },
        [](WorldState& ws) {   // unnamed slot 7 holds it now
            for (auto& o : ws.objectives) {
                if (o.id_num == 401) o.owner = 7;
            }
        });
    const auto v = rig->verdict(rig->live(), rig->opening());
    ASSERT_EQ(v.teams.size(), 4u);   // ROK, DPRK, Neutralia, slot 7
    const auto* unnamed = rig->row(v, 7);
    ASSERT_NE(unnamed, nullptr);
    EXPECT_TRUE(unnamed->name.empty());
    EXPECT_EQ(unnamed->owned, 1);
    EXPECT_EQ(unnamed->gained, 1);
    EXPECT_EQ(unnamed->gained_value, 20);
    EXPECT_EQ(rig->row(v, 0), nullptr);
    EXPECT_EQ(v.leader_slot, 7);
    EXPECT_EQ(v.band, VerdictBand::Advantage);
}

TEST(WarVerdict, ShorterOpeningVectorReadsNeutral) {
    // Defensive default: a missing opening tail reads as neutral (the
    // session never hits it — both walks cover the same wire order).
    // Both belligerents end up +30 (a tie): no lead, stalemate.
    const auto rig = Rig::make();
    const auto v = rig->verdict(rig->live(), {});  // nothing "opened"
    ASSERT_NE(rig->row(v, 2), nullptr);
    EXPECT_EQ(rig->row(v, 2)->gained, 1);   // 101 counts as gained
    EXPECT_EQ(rig->row(v, 2)->swing, 30);
    ASSERT_NE(rig->row(v, 6), nullptr);
    EXPECT_EQ(rig->row(v, 6)->gained, 1);   // 201 too
    EXPECT_EQ(rig->row(v, 6)->swing, 30);
    EXPECT_EQ(v.leader_slot, -1);
    EXPECT_EQ(v.band, VerdictBand::Stalemate);
}
