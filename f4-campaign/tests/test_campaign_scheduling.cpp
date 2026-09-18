// f4-campaign/tests/test_campaign_scheduling.cpp
//
// CAMP-DOM-4 tranche tests — the airbase-scheduling depth arm, driven
// through the CAMPAIGN (the ATM's denials queue → the ledger's
// slot-denial books — the slot_denied event family's source):
//   * the saturated pick gate: a base whose block (and, armed, the
//     previous one) is full denies every squadron based there — the
//     request goes unfilled, the refusal lands in the ledger
//   * the summary carries the scheduling counters ONLY when the arm
//     is on; the books' slot_denials total answers always (the honest
//     0 is the arms-off answer)
//   * the run is deterministic (two rigs, one summary)
//
// Every knob defaults OFF — the golden identity tests live with the
// session suite; here the arm is pinned ON, mechanically.

#include <f4/campaign/campaign.hpp>
#include <f4/campaign/mission_profile.hpp>
#include <f4/campaign/result_ledger.hpp>
#include <f4/json/f4_json.hpp>
#include <f4/world/world_adapters.hpp>
#include <f4/world/detail/world_state.hpp>

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

using namespace f4::campaign;

namespace {

MissionProfileTable load_profiles() {
    return MissionProfileTable::load(F4_MISSION_PROFILES_JSON);
}

// The hand ATM world (test_atm.cpp's shape): USA(1) vs DPRK(6), the
// USA AA wing 6001 and the unspecialized wing 6002 BOTH based at the
// USA airbase 4281, the DPRK wing 6003 at 9001. `saturate_base` seeds
// the USA airbase's wire schedule full (every block 0x1F) — the save's
// own planned sorties occupying every slot.
struct SchedOpts {
    bool saturate_base = false;
};

f4::world::WorldState make_scheduling_world(const SchedOpts& opts = {}) {
    using f4::world::ObjectiveState;
    using f4::world::TeamState;

    f4::world::WorldState ws;
    ws.version = 71;
    ws.campaign.current_time = 38574360;
    ws.campaign.te_number_aircraft = {0, 24, 0, 0, 0, 0, 24, 0};

    ws.teams.resize(8);
    ws.teams[1] = TeamState{1, 1, 1, "USA", ""};
    ws.teams[6] = TeamState{6, 6, 6, "DPRK", ""};
    ws.teams[1].stance = {0, 0, 0, 0, 0, 0, 5, 0};
    ws.teams[6].stance = {0, 5, 0, 0, 0, 0, 0, 0};

    auto obj = [](int16_t x, int16_t y, uint8_t owner, uint32_t vu,
                  uint8_t priority) {
        ObjectiveState o;
        o.x = x;
        o.y = y;
        o.owner = owner;
        o.id_num = vu;
        o.priority = priority;
        o.objective_type = 4;
        return o;
    };
    ws.objectives.push_back(obj(100, 100, 1, 4281, 5));   // USA airbase
    ws.objectives.push_back(obj(400, 400, 6, 9001, 7));   // DPRK target

    auto sq = [](uint32_t vu, uint8_t owner, uint8_t specialty, int16_t x,
                 int16_t y, uint32_t airbase, const char* name) {
        f4::world::UnitState u;
        u.unit_class = f4::entities::UnitClass::Squadron;
        u.domain = 2;
        u.x = x;
        u.y = y;
        u.owner = owner;
        u.id_num = vu;
        u.specialty = specialty;
        u.airbase_id = airbase;
        u.class_name = name;
        return u;
    };
    ws.units.push_back(sq(6001, 1, 1, 100, 100, 4281, "AA Wing"));
    ws.units.push_back(sq(6002, 1, 0, 250, 250, 4281, "Plain Wing"));
    ws.units.push_back(sq(6003, 6, 2, 400, 400, 9001, "DPRK Wing"));

    if (opts.saturate_base) {
        f4::world::AtmAirbaseState ab;
        ab.id_num = 4281;
        for (auto& b : ab.schedule) b = 0x1F;
        ws.teams[1].atm_airbases.push_back(ab);
    }
    return ws;
}

// A fully wired Campaign + ledger over the hand world, heap-held
// (MessageBus is non-movable; the rig moves as a unique_ptr and the
// destruction order tears the campaign down before its sources).
struct Rig {
    std::unique_ptr<f4::world::WorldState> ws;
    std::unique_ptr<f4::world::WorldStateAdapters> adapters;
    MissionProfileTable profiles;
    std::unique_ptr<f4::messaging::MessageBus> bus;
    std::unique_ptr<CampaignResultLedger> ledger;
    std::unique_ptr<Campaign> campaign;

    static std::unique_ptr<Rig> make(bool arm, bool saturate) {
        auto r = std::make_unique<Rig>();
        r->ws = std::make_unique<f4::world::WorldState>(
            make_scheduling_world(SchedOpts{saturate}));
        r->adapters =
            std::make_unique<f4::world::WorldStateAdapters>(*r->ws);
        r->profiles = load_profiles();
        r->bus = std::make_unique<f4::messaging::MessageBus>();
        r->ledger = std::make_unique<CampaignResultLedger>(
            r->adapters->campaign, r->adapters->teams, r->adapters->units);
        CampaignConfig cfg;
        cfg.atm_pipeline = true;
        cfg.airbase_scheduling = arm;
        r->campaign = std::make_unique<Campaign>(
            r->adapters->campaign, r->adapters->teams, r->adapters->units,
            r->profiles, *r->bus, cfg);
        r->campaign->set_result_ledger(r->ledger.get());
        return r;
    }
};

} // namespace

TEST(CampaignScheduling, SaturatedBaseDeniesIntoTheBooks) {
    // The USA base's grid is full: every USA squadron based there is
    // denied at the pick gate (the reference's block-and-previous
    // rule), every USA request goes unfilled, and every refusal books
    // into the ledger's slot-denial log — the slot_denied family's
    // source. The DPRK wing (base 9001, no schedule row) flies on.
    auto rig = Rig::make(/*arm=*/true, /*saturate=*/true);
    rig->campaign->tick(1800);   // one full tasking cycle

    const auto& log = rig->ledger->slot_denial_log();
    ASSERT_FALSE(log.empty()) << "a saturated base denied nothing";
    // Every record is one of the two refusal shapes: the pick gate's
    // block-full skip (reason 0 — and every one of those is the
    // saturated USA base; the DPRK wing's base started empty) or the
    // horizon's refusal (reason 1 — the grids the cycle's own bookings
    // filled, and the far-TOT requests the gate never saw because
    // their block sat past the horizon).
    int pick_denials = 0;
    for (const auto& d : log) {
        ASSERT_TRUE(d.reason == kSlotDeniedPickFull ||
                    d.reason == kSlotDeniedHorizon);
        if (d.reason == kSlotDeniedPickFull) {
            ++pick_denials;
            EXPECT_EQ(d.team, 1);
            EXPECT_EQ(d.airbase, 4281u);
        } else {
            EXPECT_TRUE(d.airbase == 4281u || d.airbase == 9001u);
        }
    }
    EXPECT_GT(pick_denials, 0) << "the pick gate never fired";
    EXPECT_EQ(rig->ledger->slot_denials(),
              static_cast<int>(log.size()));
    // The ATM's counter and the books agree one-for-one, and the USA
    // side filled nothing (both its wings share the denied base) while
    // the war still tasked.
    const auto* atm = rig->campaign->atm_stats();
    ASSERT_NE(atm, nullptr);
    EXPECT_GT(atm->schedule_denials, 0);
    EXPECT_EQ(atm->schedule_denials + atm->slot_overflows,
              static_cast<int>(log.size()));
    EXPECT_GT(atm->requests_unfilled, 0);
    EXPECT_GT(atm->packages_built, 0) << "the DPRK wing stopped flying";

    // The summary carries the scheduling counters (the arm is on) and
    // the books' total answers with the log's size.
    const std::string summary = rig->campaign->to_summary_json();
    EXPECT_NE(summary.find("\"schedule_denials\":"), std::string::npos);
    EXPECT_NE(summary.find("\"slot_overflows\":"), std::string::npos);
    EXPECT_NE(summary.find("\"slot_releases\":"), std::string::npos);
    const std::string books = rig->ledger->to_json();
    EXPECT_NE(books.find("\"slot_denials\":" + std::to_string(log.size())),
              std::string::npos)
        << books.substr(0, 400);
    // And the denial log rides the artifact (the event family's wire
    // source) with its reason bytes.
    EXPECT_NE(books.find("\"slot_denials\": ["), std::string::npos);
    EXPECT_NE(books.find("\"reason\":0"), std::string::npos);
}

TEST(CampaignScheduling, ArmOffDeniesSilentlyLikeTheGoldenShape) {
    // The same saturated world, the arm OFF: the single-block rule
    // still skips the full base (the pre-DOM-4 behavior), but nothing
    // books, nothing queues, and the summary carries no scheduling
    // keys — the artifact's only face is the honest-0 total.
    auto rig = Rig::make(/*arm=*/false, /*saturate=*/true);
    rig->campaign->tick(1800);

    const auto* atm = rig->campaign->atm_stats();
    ASSERT_NE(atm, nullptr);
    EXPECT_EQ(atm->schedule_denials, 0);
    EXPECT_EQ(rig->ledger->slot_denial_log().size(), 0u);
    const std::string summary = rig->campaign->to_summary_json();
    EXPECT_EQ(summary.find("\"schedule_denials\""), std::string::npos);
    const std::string books = rig->ledger->to_json();
    EXPECT_NE(books.find("\"slot_denials\":0"), std::string::npos);
    EXPECT_EQ(books.find("\"slot_denials\": ["), std::string::npos);
}

TEST(CampaignScheduling, ArmedRunIsDeterministic) {
    auto a = Rig::make(/*arm=*/true, /*saturate=*/true);
    auto b = Rig::make(/*arm=*/true, /*saturate=*/true);
    a->campaign->tick(1800);
    b->campaign->tick(1800);
    EXPECT_EQ(a->campaign->to_summary_json(), b->campaign->to_summary_json());
    EXPECT_EQ(a->ledger->to_json(), b->ledger->to_json());
    EXPECT_EQ(a->ledger->slot_denial_log().size(),
              b->ledger->slot_denial_log().size());
}
