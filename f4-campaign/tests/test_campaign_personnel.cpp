// f4-campaign/tests/test_campaign_personnel.cpp
//
// CAMP-DOM-3 tranche tests — the personnel layer (the reference's
// FlightClass::BuildMission tail):
//   * AssignPilots: the crew pick (lead = the roster's front third,
//     wingmen backward from the tail), the pick-time crew gate
//     (a squadron that cannot crew is skipped — the request fills
//     from the runner-up), the out-set (two same-squadron flights in
//     one cycle never double-pick a slot), the dead-slot skip
//   * The rating-decay arm: new = (int)(0.75 × rating) + 1 — the
//     integer arithmetic (floor at 4), the live-view ride on the
//     flight, the no-table no-op (the static fallback stands)
//   * The personnel books: the assignment/loss/recovery logs, the
//     per-slot deltas (dead stays dead, sorties credited this run),
//     the flight→crew map the loss and recovery paths consume
//   * The write-back's personnel face: dead slots → status 1,
//     per-pilot missions_flown += the run's credited sorties, the
//     decayed rating table — and the activity gate (a run the
//     personnel never moved writes nothing)
//
// Every knob defaults OFF — the golden identity tests live with the
// session suite; here the arms are pinned ON, mechanically.

#include <f4/campaign/atm.hpp>
#include <f4/campaign/campaign.hpp>
#include <f4/campaign/mission_profile.hpp>
#include <f4/campaign/result_ledger.hpp>
#include <f4/campaign/world_writeback.hpp>
#include <f4/world/world_adapters.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <vector>

using namespace f4::campaign;
using f4::entities::UnitClass;
using f4::world::UnitState;
using f4::world::WorldState;
using f4::world::WorldStateAdapters;

namespace {

MissionProfileTable load_profiles() {
    return MissionProfileTable::load(F4_MISSION_PROFILES_JSON);
}

// A roster helper: `count` available pilots (status 0), the named
// slots forced dead (status 1 — the wire's DEAD byte).
std::vector<f4::entities::PilotState> make_roster(
        int count, const std::vector<int>& dead = {}) {
    std::vector<f4::entities::PilotState> pilots;
    pilots.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        f4::entities::PilotState p;
        p.pilot_id = static_cast<std::int16_t>(1000 + i);
        p.skill = static_cast<std::uint8_t>(5 + (i % 4));
        p.status = 0;
        pilots.push_back(p);
    }
    for (const auto d : dead) {
        if (d >= 0 && d < count) {
            pilots[static_cast<std::size_t>(d)].status = 1;
        }
    }
    return pilots;
}

// The hand ATM world (test_atm.cpp's shape) plus PILOT ROSTERS and
// optional per-squadron UCD Scores (the decay arm's seed). USA(1) vs
// DPRK(6); the USA AA wing 6001, the unspecialized wing 6002, the DPRK
// wing 6003; the team pool funds the draws (24 ships per side).
struct PersonnelOpts {
    std::vector<int> wing1_dead;      // 6001's dead slots
    int wing1_free = 12;              // roster size BEFORE the dead cut
    int wing2_free = 6;               // 6002's roster (all available)
    std::uint8_t wing1_ca_score = 0;  // the UCD ARO_CA column (decay seed)
    std::uint8_t wing2_ca_score = 0;  // 6002's ARO_CA column
    std::uint8_t wing1_specialty = 1; // the AA byte (the spread test zeroes)
};

WorldState make_personnel_world(const PersonnelOpts& opts = {}) {
    using f4::world::ObjectiveState;
    using f4::world::TeamState;

    WorldState ws;
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

    auto sq = [&](uint32_t vu, uint8_t owner, uint8_t specialty, int16_t x,
                  int16_t y, uint32_t airbase, const char* name,
                  std::vector<f4::entities::PilotState> pilots) {
        UnitState u;
        u.unit_class = UnitClass::Squadron;
        u.domain = 2;
        u.x = x;
        u.y = y;
        u.owner = owner;
        u.id_num = vu;
        u.specialty = specialty;
        u.airbase_id = airbase;
        u.class_name = name;
        u.pilots = std::move(pilots);
        return u;
    };
    ws.units.push_back(sq(6001, 1, opts.wing1_specialty, 100, 100, 4281,
                          "AA Wing", make_roster(opts.wing1_free,
                                                 opts.wing1_dead)));
    ws.units.push_back(sq(6002, 1, 0, 250, 250, 4281, "Plain Wing",
                          make_roster(opts.wing2_free)));
    ws.units.push_back(sq(6003, 6, 2, 400, 400, 9001, "DPRK Wing",
                          make_roster(9)));
    if (opts.wing1_ca_score != 0) {
        ws.units[0].unit_class_scores[1] = opts.wing1_ca_score;  // ARO_CA
    }
    if (opts.wing2_ca_score != 0) {
        ws.units[1].unit_class_scores[1] = opts.wing2_ca_score;  // ARO_CA
    }
    return ws;
}

// A fully wired ATM over the hand world (test_atm.cpp's Rig shape).
struct Rig {
    std::unique_ptr<WorldState> ws;
    std::unique_ptr<WorldStateAdapters> adapters;
    MissionProfileTable profiles;
    std::unique_ptr<AirTaskingManager> atm;

    static std::unique_ptr<Rig> make(const PersonnelOpts& popts = {},
                                     AtmConfig cfg = {}) {
        auto r = std::make_unique<Rig>();
        r->ws = std::make_unique<WorldState>(make_personnel_world(popts));
        r->adapters = std::make_unique<WorldStateAdapters>(*r->ws);
        r->profiles = load_profiles();
        r->atm = std::make_unique<AirTaskingManager>(
            r->profiles, r->adapters->campaign, r->adapters->teams,
            r->adapters->units, &r->adapters->objectives, cfg);
        return r;
    }
};

// A BARCAP request — ARO_CA, the crew gate's cleanest walk. The
// priority sets FindBestAir's own gate: lowest_score = (255 −
// priority) / 25 (priority 200 → gate 2 — low-rating tables stay
// flyable, which the decay tests need).
MissionRequest barcap(int aircraft, int tot = 3600, int priority = 100) {
    MissionRequest r;
    r.mission = 1;   // BARCAP — ARO_CA
    r.team = 1;
    r.priority = priority;
    r.aircraft = aircraft;
    r.tot = tot;
    r.tot_type = TotType::LE;
    return r;
}

constexpr CampaignTime kNow = 0;   // relative clock

// The ledger rig: a minimal world with a rostered squadron (slot 3),
// the adapters, and the ledger seeded from them.
struct LedgerRig {
    std::unique_ptr<WorldState> ws;
    std::unique_ptr<WorldStateAdapters> adapters;
    std::unique_ptr<CampaignResultLedger> ledger;

    static std::unique_ptr<LedgerRig> make() {
        auto r = std::make_unique<LedgerRig>();
        WorldState w;
        w.version = 71;
        w.campaign.te_number_aircraft = {0, 0, 0, 10, 0, 0, 0, 0};
        f4::world::TeamState team;
        team.slot = 3;
        team.name = "TEST";
        w.teams = {team};
        UnitState sq;
        sq.unit_class = UnitClass::Squadron;
        sq.domain = 2;
        sq.owner = 3;
        sq.id_num = 4281;
        sq.class_name = "Test Squadron";
        sq.pilots = make_roster(12);
        w.units = {sq};
        r->ws = std::make_unique<WorldState>(std::move(w));
        r->adapters = std::make_unique<WorldStateAdapters>(*r->ws);
        r->ledger = std::make_unique<CampaignResultLedger>(
            r->adapters->campaign, r->adapters->teams, r->adapters->units);
        return r;
    }
};

} // namespace

// ============================================================================
// AssignPilots — the crew pick
// ============================================================================

TEST(PilotAssignment, CrewPickOrderFrontThirdLeadBackwardWingmen) {
    PersonnelOpts popts;   // 6001: 12 available pilots
    AtmConfig cfg;
    cfg.pilot_assignment = true;
    auto rig = Rig::make(popts, cfg);

    auto flights = rig->atm->compose_packages({barcap(4)}, 1, kNow);
    ASSERT_EQ(flights.size(), 1u);
    EXPECT_EQ(flights[0].squadron_vu, 6001u);
    // The lead: the FIRST free slot in the front third (12 pilots →
    // slots 0..3) — slot 0. The wingmen: backward from the tail —
    // 11, 10, 9. The reference's own scan, slot for slot.
    ASSERT_EQ(flights[0].crew.size(), 4u);
    EXPECT_EQ(flights[0].crew[0], 0);
    EXPECT_EQ(flights[0].crew[1], 11);
    EXPECT_EQ(flights[0].crew[2], 10);
    EXPECT_EQ(flights[0].crew[3], 9);
    EXPECT_EQ(rig->atm->stats().crews_assigned, 1);
    EXPECT_EQ(rig->atm->stats().crew_denials, 0);
    // The decay arm is OFF: the flight carries no rating view.
    EXPECT_FALSE(flights[0].ratings_valid);
}

TEST(PilotAssignment, SecondSameSquadronFlightSkipsTheDrawnSlots) {
    // Two 2-ship BARCAPs in one compose: both land on the AA wing (the
    // higher CA score). The second pick must skip the first flight's
    // crew (the ATM's pick-time out-set — the ledger books at PUBLISH,
    // after compose built the whole cycle).
    PersonnelOpts popts;
    AtmConfig cfg;
    cfg.pilot_assignment = true;
    auto rig = Rig::make(popts, cfg);

    auto flights = rig->atm->compose_packages(
        {barcap(2, 3600), barcap(2, 5400)}, 1, kNow);
    ASSERT_EQ(flights.size(), 2u);
    EXPECT_EQ(flights[0].squadron_vu, 6001u);
    EXPECT_EQ(flights[1].squadron_vu, 6001u);
    ASSERT_EQ(flights[0].crew.size(), 2u);
    ASSERT_EQ(flights[1].crew.size(), 2u);
    // Flight 1: [0, 11]. Flight 2: the lead falls forward to 1, the
    // wingman skips the drawn 11 → 10.
    EXPECT_EQ(flights[0].crew[0], 0);
    EXPECT_EQ(flights[0].crew[1], 11);
    EXPECT_EQ(flights[1].crew[0], 1);
    EXPECT_EQ(flights[1].crew[1], 10);
    EXPECT_EQ(rig->atm->stats().crews_assigned, 2);
}

TEST(PilotAssignment, DeadLeadFallsForwardInsideTheFrontThird) {
    // Slot 0 is dead (the wire's own status byte): the lead scan takes
    // the first FREE slot — 1. The wingmen still scan from the tail.
    PersonnelOpts popts;
    popts.wing1_dead = {0};
    AtmConfig cfg;
    cfg.pilot_assignment = true;
    auto rig = Rig::make(popts, cfg);

    auto flights = rig->atm->compose_packages({barcap(4)}, 1, kNow);
    ASSERT_EQ(flights.size(), 1u);
    ASSERT_EQ(flights[0].crew.size(), 4u);
    EXPECT_EQ(flights[0].crew[0], 1);
    EXPECT_EQ(flights[0].crew[1], 11);
    EXPECT_EQ(flights[0].crew[2], 10);
    EXPECT_EQ(flights[0].crew[3], 9);
}

TEST(PilotAssignment, CannotCrewFailsToTheRunnerUp) {
    // The AA wing cannot field 4 crew (only 2 free pilots); the
    // unspecialized wing (6 available) fields it. The scored walk
    // falls to the runner-up — the reference's flight-fails rule
    // reshaped as a pick-time gate, the denial counted.
    PersonnelOpts popts;
    popts.wing1_free = 2;
    AtmConfig cfg;
    cfg.pilot_assignment = true;
    auto rig = Rig::make(popts, cfg);

    auto flights = rig->atm->compose_packages({barcap(4)}, 1, kNow);
    ASSERT_EQ(flights.size(), 1u);
    EXPECT_EQ(flights[0].squadron_vu, 6002u);
    // 6 pilots → front third = 2 → the lead from slots 0..1 → 0; the
    // wingmen from the tail: 5, 4, 3.
    ASSERT_EQ(flights[0].crew.size(), 4u);
    EXPECT_EQ(flights[0].crew[0], 0);
    EXPECT_EQ(flights[0].crew[1], 5);
    EXPECT_EQ(flights[0].crew[2], 4);
    EXPECT_EQ(flights[0].crew[3], 3);
    EXPECT_EQ(rig->atm->stats().crew_denials, 1);
    EXPECT_EQ(rig->atm->stats().crews_assigned, 1);
}

TEST(PilotAssignment, WholeFrontThirdDeadFailsTheFlight) {
    // No commander: every front-third slot dead → the flight fails in
    // the AA wing and the runner-up fields it (two denials never
    // double-count — 6001 fails once).
    PersonnelOpts popts;
    popts.wing1_dead = {0, 1, 2, 3};
    AtmConfig cfg;
    cfg.pilot_assignment = true;
    auto rig = Rig::make(popts, cfg);

    auto flights = rig->atm->compose_packages({barcap(2)}, 1, kNow);
    ASSERT_EQ(flights.size(), 1u);
    EXPECT_EQ(flights[0].squadron_vu, 6002u);
    EXPECT_EQ(rig->atm->stats().crew_denials, 1);
}

TEST(PilotAssignment, ArmOffIgnoresTheRosters) {
    // The golden identity at the engine face: the arm OFF → no crew
    // rides, nothing counts, the same squadron wins the pick.
    PersonnelOpts popts;
    auto rig = Rig::make(popts, AtmConfig{});

    auto flights = rig->atm->compose_packages({barcap(4)}, 1, kNow);
    ASSERT_EQ(flights.size(), 1u);
    EXPECT_EQ(flights[0].squadron_vu, 6001u);
    EXPECT_TRUE(flights[0].crew.empty());
    EXPECT_EQ(rig->atm->stats().crews_assigned, 0);
    EXPECT_EQ(rig->atm->stats().crew_denials, 0);
}

// ============================================================================
// The rating-decay arm
// ============================================================================

TEST(RatingDecay, DecaysTheRoleColumnAndRidesTheFlight) {
    // The UCD ARO_CA column seeds the live view (80); the first
    // assignment decays it: (3 × 80) / 4 + 1 = 61. The decayed view
    // rides the flight (the Campaign syncs the ledger — its write
    // domain).
    PersonnelOpts popts;
    popts.wing1_ca_score = 80;
    AtmConfig cfg;
    cfg.rating_decay = true;
    auto rig = Rig::make(popts, cfg);

    auto flights = rig->atm->compose_packages({barcap(2)}, 1, kNow);
    ASSERT_EQ(flights.size(), 1u);
    EXPECT_EQ(flights[0].squadron_vu, 6001u);
    EXPECT_TRUE(flights[0].ratings_valid);
    EXPECT_EQ(flights[0].squadron_ratings[1], 61);
    EXPECT_EQ(rig->atm->stats().ratings_decayed, 1);
}

TEST(RatingDecay, ThePlusOneFloorsTheFixedPointAtFour) {
    // 5 → (15 / 4) + 1 = 4; 4 → (12 / 4) + 1 = 4 — a rating never
    // decays to zero (the reference's own tuning row). Priority 200
    // drops FindBestAir's gate to 2, so a rating-5 wing stays flyable;
    // the Plain wing's even lower table (1) loses every walk.
    PersonnelOpts popts;
    popts.wing1_ca_score = 5;
    popts.wing2_ca_score = 1;
    AtmConfig cfg;
    cfg.rating_decay = true;
    auto rig = Rig::make(popts, cfg);

    auto flights = rig->atm->compose_packages(
        {barcap(1, 3600, 200), barcap(1, 7200, 200)}, 1, kNow);
    ASSERT_EQ(flights.size(), 2u);
    EXPECT_EQ(flights[0].squadron_vu, 6001u);
    EXPECT_EQ(flights[1].squadron_vu, 6001u);
    EXPECT_EQ(flights[0].squadron_ratings[1], 4);
    EXPECT_EQ(flights[1].squadron_ratings[1], 4);
    EXPECT_EQ(rig->atm->stats().ratings_decayed, 2);
}

TEST(RatingDecay, NoTableNeverDecays) {
    // Neither the wire nor the UCD carries a table (the kunsan shape):
    // the static specialty fallback stands, nothing decays, no ride.
    PersonnelOpts popts;   // wing1_ca_score = 0
    AtmConfig cfg;
    cfg.rating_decay = true;
    auto rig = Rig::make(popts, cfg);

    auto flights = rig->atm->compose_packages({barcap(2)}, 1, kNow);
    ASSERT_EQ(flights.size(), 1u);
    EXPECT_FALSE(flights[0].ratings_valid);
    EXPECT_EQ(rig->atm->stats().ratings_decayed, 0);
}

TEST(RatingDecay, TheLiveViewDrivesThePickScore) {
    // The rotation pressure, observed. FindBestAir's base score is
    // (rating + 4) / 5 — the DECAYED column re-prices the wing. The
    // columns: 6001 = 100, 6002 = 85, both unspecialized (the +5
    // specialty bonus would dominate), 6001 on the target cell (the
    // +2 range and +2 quickest bonuses). The walk: 6001 wins (24 vs
    // 17) and decays to 76 — still wins (20 vs 17) and decays to 57 —
    // the third BARCAP crosses over: 16 vs 17, the Plain wing fields
    // it (and ITS decay fires — 85 → 64 — the table it carries now
    // lives).
    PersonnelOpts popts;
    popts.wing1_ca_score = 100;
    popts.wing2_ca_score = 85;
    popts.wing1_specialty = 0;   // the specialty bonus would dominate
    AtmConfig cfg;
    cfg.rating_decay = true;
    auto rig = Rig::make(popts, cfg);

    auto first = rig->atm->compose_packages({barcap(1, 3600, 200)}, 1, kNow);
    ASSERT_EQ(first.size(), 1u);
    EXPECT_EQ(first[0].squadron_vu, 6001u);   // 24 vs 17
    EXPECT_EQ(first[0].squadron_ratings[1], 76);

    auto second = rig->atm->compose_packages({barcap(1, 3600, 200)}, 1, kNow);
    ASSERT_EQ(second.size(), 1u);
    EXPECT_EQ(second[0].squadron_vu, 6001u);   // 20 vs 17
    EXPECT_EQ(second[0].squadron_ratings[1], 58);   // (3×76)/4 + 1

    auto third = rig->atm->compose_packages({barcap(1, 3600, 200)}, 1, kNow);
    ASSERT_EQ(third.size(), 1u);
    EXPECT_EQ(third[0].squadron_vu, 6002u)
        << "the decayed column never spread the sorties";
    EXPECT_EQ(third[0].squadron_ratings[1], 64);   // 85 decayed once
    EXPECT_EQ(rig->atm->stats().ratings_decayed, 3);
}

// ============================================================================
// The personnel books (the ledger)
// ============================================================================

TEST(PilotBooks, DrawBooksCrewAssignmentAndOutDeltas) {
    auto rig = LedgerRig::make();
    const std::vector<std::uint8_t> crew{0, 11, 10, 9};
    rig->ledger->apply_mission_draw(60.0, 3, 4281, 4, 5101, crew);

    ASSERT_EQ(rig->ledger->pilot_assignment_log().size(), 1u);
    const auto& a = rig->ledger->pilot_assignment_log()[0];
    EXPECT_DOUBLE_EQ(a.t_s, 60.0);
    EXPECT_EQ(a.team, 3);
    EXPECT_EQ(a.squadron, 4281u);
    EXPECT_EQ(a.flight, 5101u);
    EXPECT_EQ(a.crew, crew);

    // Every drawn slot is OUT until its flight recovers.
    const auto* deltas = rig->ledger->squadron_personnel(4281);
    ASSERT_NE(deltas, nullptr);
    ASSERT_EQ(deltas->size(), 4u);
    for (const auto& d : *deltas) {
        EXPECT_TRUE(d.out);
        EXPECT_FALSE(d.dead);
        EXPECT_EQ(d.missions_added, 0);
    }
    // The flight→crew map arms the loss/recovery paths.
    ASSERT_NE(rig->ledger->flight_crew(5101), nullptr);
    EXPECT_EQ(rig->ledger->flight_crew(5101)->crew, crew);
}

TEST(PilotBooks, EmptyCrewIsThePreDom3Shape) {
    auto rig = LedgerRig::make();
    rig->ledger->apply_mission_draw(60.0, 3, 4281, 4);
    EXPECT_TRUE(rig->ledger->pilot_assignment_log().empty());
    EXPECT_EQ(rig->ledger->squadron_personnel(4281), nullptr);
    EXPECT_EQ(rig->ledger->flight_crew(5101), nullptr);
}

TEST(PilotBooks, LossesConsumeSlotsInPickOrderLeadFirst) {
    auto rig = LedgerRig::make();
    const std::vector<std::uint8_t> crew{0, 11, 10, 9};
    rig->ledger->apply_mission_draw(60.0, 3, 4281, 4, 5101, crew);

    // The flight loses two aircraft: the lead's slot dies FIRST (the
    // deterministic subset — the pick order), then the next listed.
    rig->ledger->apply_air_loss(120.0, 3, 4281, 5101, 999);
    rig->ledger->apply_air_loss(121.0, 3, 4281, 5101, 999);

    ASSERT_EQ(rig->ledger->pilot_loss_log().size(), 2u);
    EXPECT_EQ(rig->ledger->pilot_loss_log()[0].slot, 0);
    EXPECT_EQ(rig->ledger->pilot_loss_log()[1].slot, 11);
    const auto* entry = rig->ledger->squadron(4281);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->run_pilot_losses, 2);

    // A third loss (a 4-ship that lost 3): the walk skips the dead
    // slots — the next live slot in pick order consumes.
    rig->ledger->apply_air_loss(122.0, 3, 4281, 5101, 999);
    ASSERT_EQ(rig->ledger->pilot_loss_log().size(), 3u);
    EXPECT_EQ(rig->ledger->pilot_loss_log()[2].slot, 10);
    EXPECT_EQ(entry->run_pilot_losses, 3);
}

TEST(PilotBooks, RecoveryCreditsSurvivorsAndReleasesTheOut) {
    auto rig = LedgerRig::make();
    const std::vector<std::uint8_t> crew{0, 11, 10, 9};
    rig->ledger->apply_mission_draw(60.0, 3, 4281, 4, 5101, crew);
    rig->ledger->apply_air_loss(120.0, 3, 4281, 5101, 999);   // the lead

    // The flight came home: three survivors flew one sortie each; the
    // dead slot stays dead and un-credited; the out-set releases.
    rig->ledger->apply_mission_recovery(3600.0, 3, 4281, 5101, 3);

    ASSERT_EQ(rig->ledger->pilot_recovery_log().size(), 3u);
    for (const auto& r : rig->ledger->pilot_recovery_log()) {
        EXPECT_EQ(r.missions_run, 1);
        EXPECT_NE(r.slot, 0) << "the dead lead flew no sortie";
    }
    const auto* entry = rig->ledger->squadron(4281);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->run_pilot_sorties, 3);
    EXPECT_EQ(entry->run_pilot_losses, 1);

    const auto* deltas = rig->ledger->squadron_personnel(4281);
    ASSERT_NE(deltas, nullptr);
    for (const auto& d : *deltas) {
        EXPECT_FALSE(d.out) << "a recovered flight released its crew";
        if (d.slot == 0) {
            EXPECT_TRUE(d.dead);
            EXPECT_EQ(d.missions_added, 0);
        } else {
            EXPECT_FALSE(d.dead);
            EXPECT_EQ(d.missions_added, 1);
        }
    }
    // The map entry went with the flight.
    EXPECT_EQ(rig->ledger->flight_crew(5101), nullptr);
}

TEST(PilotBooks, TheDocumentCarriesThePersonnelFace) {
    // The ledger's result document (the `books` query's payload) rides
    // the personnel books: the totals always answer, the logs only
    // when one exists, the squadron rows only when the run moved the
    // roster or the decay fired.
    auto rig = LedgerRig::make();

    // Pristine: the totals carry the honest zeros; no personnel logs,
    // no personnel keys on the squadron rows.
    const std::string quiet = rig->ledger->to_json();
    EXPECT_NE(quiet.find("\"pilot_assignments\":0"), std::string::npos);
    EXPECT_EQ(quiet.find("\"pilot_assignments\": ["), std::string::npos);
    EXPECT_EQ(quiet.find("run_pilot_losses"), std::string::npos);

    // The full chain: draw → loss → recovery. The totals count, the
    // logs appear in arrival order, the squadron row carries the run
    // deltas and the decayed table.
    const std::vector<std::uint8_t> crew{0, 11, 10, 9};
    rig->ledger->apply_mission_draw(60.0, 3, 4281, 4, 5101, crew);
    rig->ledger->apply_air_loss(120.0, 3, 4281, 5101, 999);
    rig->ledger->apply_mission_recovery(3600.0, 3, 4281, 5101, 3);
    std::array<std::uint8_t, 16> decayed{};
    decayed[1] = 61;
    rig->ledger->sync_squadron_ratings(4281, decayed);

    const std::string doc = rig->ledger->to_json();
    EXPECT_NE(doc.find("\"pilot_assignments\":1"), std::string::npos);
    EXPECT_NE(doc.find("\"pilot_losses\":1"), std::string::npos);
    EXPECT_NE(doc.find("\"pilot_sorties\":3"), std::string::npos);
    EXPECT_NE(doc.find("\"pilot_assignments\": ["), std::string::npos);
    EXPECT_NE(doc.find("\"pilot_losses\": ["), std::string::npos);
    EXPECT_NE(doc.find("\"pilot_recoveries\": ["), std::string::npos);
    EXPECT_NE(doc.find("\"crew\": [0, 11, 10, 9]"), std::string::npos);
    EXPECT_NE(doc.find("\"run_pilot_losses\":1"), std::string::npos);
    EXPECT_NE(doc.find("\"run_pilot_sorties\":3"), std::string::npos);
    EXPECT_NE(doc.find("\"role_ratings\""), std::string::npos);

    // Run-to-run byte stability (the artifact's own contract).
    auto rig2 = LedgerRig::make();
    rig2->ledger->apply_mission_draw(60.0, 3, 4281, 4, 5101, crew);
    rig2->ledger->apply_air_loss(120.0, 3, 4281, 5101, 999);
    rig2->ledger->apply_mission_recovery(3600.0, 3, 4281, 5101, 3);
    rig2->ledger->sync_squadron_ratings(4281, decayed);
    EXPECT_EQ(doc, rig2->ledger->to_json());
}

// ============================================================================
// The write-back's personnel face
// ============================================================================

TEST(PilotWriteback, AppliesDeathsSortiesAndRatings) {
    auto r = LedgerRig::make();
    const std::vector<std::uint8_t> crew{0, 11, 10, 9};
    r->ledger->apply_mission_draw(60.0, 3, 4281, 4, 5101, crew);
    r->ledger->apply_air_loss(120.0, 3, 4281, 5101, 999);   // slot 0 dies
    r->ledger->apply_mission_recovery(3600.0, 3, 4281, 5101, 3);

    std::array<std::uint8_t, 16> decayed{};
    decayed[1] = 61;
    r->ledger->sync_squadron_ratings(4281, decayed);

    const auto out = apply_to(*r->ledger, *r->ws);
    EXPECT_EQ(out.personnel_written, 1);

    auto& p = r->ws->units[0].pilots;
    EXPECT_EQ(p[0].status, 1) << "the dead lead wrote the DEAD byte";
    EXPECT_EQ(p[11].missions_flown, 1);
    EXPECT_EQ(p[10].missions_flown, 1);
    EXPECT_EQ(p[9].missions_flown, 1);
    EXPECT_EQ(p[0].missions_flown, 0) << "the dead flew nothing";
    EXPECT_EQ(p[5].missions_flown, 0) << "an un-crewed slot is untouched";
    EXPECT_EQ(r->ws->units[0].role_ratings[1], 61);

    // The save face: the decayed table reaches the saved world JSON
    // (presence = data) and a reload parses it back — the C1
    // round-trip, personnel edition.
    const auto saved = r->ws->to_json_string();
    EXPECT_NE(saved.find("\"role_ratings\""), std::string::npos)
        << "the decayed table never reached the save face";
    WorldState reloaded;
    reloaded.load_from_string(saved);
    ASSERT_FALSE(reloaded.units.empty());
    EXPECT_EQ(reloaded.units[0].role_ratings[1], 61);
    EXPECT_EQ(reloaded.units[0].pilots[0].status, 1);
    EXPECT_EQ(reloaded.units[0].pilots[11].missions_flown, 1);
}

TEST(PilotWriteback, PristineRosterWritesNothing) {
    // A draw with no deaths, no recoveries, no rating fires: the out
    // is TRANSIENT (the save carries no phantom states) — the roster
    // keeps the wire's own bytes, personnel_written stays 0.
    auto r = LedgerRig::make();
    const std::vector<std::uint8_t> crew{0, 11, 10, 9};
    r->ledger->apply_mission_draw(60.0, 3, 4281, 4, 5101, crew);

    const auto out = apply_to(*r->ledger, *r->ws);
    EXPECT_EQ(out.personnel_written, 0);
    for (std::size_t i = 0; i < r->ws->units[0].pilots.size(); ++i) {
        EXPECT_EQ(r->ws->units[0].pilots[i].status, 0);
        EXPECT_EQ(r->ws->units[0].pilots[i].missions_flown, 0);
    }
    EXPECT_EQ(r->ws->units[0].role_ratings[1], 0);
}
