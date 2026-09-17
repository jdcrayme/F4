// test_airbase_synthesis.cpp — the stock-save bridge: squadron airbase
// synthesis over an in-memory WorldState (load_from_string, the same
// synthetic-snippet pattern test_world_state uses).
//
// The scenario mirrors save0.cam's real shape: a US wing with NO
// US-owned airbase (Korea's ownership model — the fields are RK-owned,
// the stance row says ALLIED), an enemy block (War — never acceptable),
// a garbage-stance slot (the real saves' -5141 toward unused slots), and
// squadrons sitting exactly on their base's grid and off-grid. One
// already-based squadron pins "the wire wins, synthesis never reassigns".

#include <gtest/gtest.h>
#include <f4/world/airbase_synthesis.hpp>
#include <f4/world/detail/world_state.hpp>

using namespace f4::world;

namespace {

// Three teams: slot 1 (U.S.) allied with slot 2 (ROK), at war with slot
// 6 (DPRK), and the slot-7 garbage the stock saves carry toward unused
// slots. The stance rows mirror the stock saves' .tea enrichment.
const char* kWorld = R"({
    "version": 63,
    "campaign": {
        "current_time": 0,
        "teams": [
            {"slot": 1, "name": "U.S.",
             "stance": [0, 1, 1, 3, 3, 3, 5, -5141]},
            {"slot": 2, "name": "ROK",
             "stance": [0, 1, 1, 3, 3, 3, 5, -5141]},
            {"slot": 6, "name": "DPRK",
             "stance": [0, 5, 5, 3, 3, 3, 1, -5141]},
            {"slot": 7, "name": "Gorn",
             "stance": [0, 3, 3, 3, 3, 3, 3, 1]}
        ]
    },
    "objectives": {
        "count": 6,
        "items": [
            {"id_num": 100, "objective_type": 1, "owner": 2,
             "x": 100, "y": 100},
            {"id_num": 101, "objective_type": 2, "owner": 2,
             "x": 110, "y": 110},
            {"id_num": 102, "objective_type": 3, "owner": 1,
             "x": 130, "y": 130},
            {"id_num": 103, "objective_type": 1, "owner": 6,
             "x": 101, "y": 101},
            {"id_num": 104, "objective_type": 1, "owner": 7,
             "x": 100, "y": 100},
            {"id_num": 0,   "objective_type": 1, "owner": 2,
             "x": 100, "y": 100}
        ]
    },
    "units": {
        "count": 6,
        "items": [
            {"unit_class": "squadron", "id_num": 10, "owner": 1,
             "x": 100, "y": 100, "airbase_id": 0},
            {"unit_class": "squadron", "id_num": 11, "owner": 1,
             "x": 109, "y": 109, "airbase_id": 0},
            {"unit_class": "squadron", "id_num": 12, "owner": 6,
             "x": 101, "y": 101, "airbase_id": 0},
            {"unit_class": "squadron", "id_num": 13, "owner": 1,
             "x": 102, "y": 102, "airbase_id": 0},
            {"unit_class": "squadron", "id_num": 14, "owner": 2,
             "x": 100, "y": 100, "airbase_id": 100},
            {"unit_class": "flight", "id_num": 15, "owner": 1,
             "x": 100, "y": 100, "airbase_id": 0}
        ]
    }
})";

WorldState load_world() {
    WorldState ws;
    ws.load_from_string(kWorld);
    return ws;
}

std::uint32_t base_of(const WorldState& ws, std::uint32_t id) {
    for (const auto& u : ws.units) {
        if (u.id_num == id) return u.airbase_id;
    }
    return 0;
}

} // namespace

TEST(AirbaseSynthesis, AssignsNearestFriendlyBase) {
    WorldState ws = load_world();
    const auto report = synthesize_squadron_airbases(ws);
    EXPECT_EQ(report.squadrons, 5);   // the flight is not a squadron
    EXPECT_EQ(report.assigned, 4);    // 10, 11, 12, 13
    EXPECT_EQ(report.unresolved, 0);
}

TEST(AirbaseSynthesis, ExactGridBeatNearest) {
    // Squadron 10 (US) sits exactly on the RK airbase at (100,100): the
    // exact-grid pass takes it even though the same grid also holds a
    // garbage-stance-owned airbase (104 — never acceptable) and a
    // zero-id objective (never a candidate).
    WorldState ws = load_world();
    (void)synthesize_squadron_airbases(ws);
    EXPECT_EQ(base_of(ws, 10), 100u);
    // Squadron 12 (DPRK): its own team's base — the US/allied grid at
    // (100,100) is War-rejected, the exact grid at (101,101) wins.
    EXPECT_EQ(base_of(ws, 12), 103u);
}

TEST(AirbaseSynthesis, OffGridTakesNearestAcceptable) {
    WorldState ws = load_world();
    (void)synthesize_squadron_airbases(ws);
    // Squadron 11 (US, at 109,109): the RK airstrip at (110,110) is
    // d2=2, the RK airbase at (100,100) d2=162 — nearest wins.
    EXPECT_EQ(base_of(ws, 11), 101u);
    // Squadron 13 (US, at 102,102): the RK airbase at (100,100) is
    // d2=8, the airstrip d2=128, the US ARMYBASE at (130,130) d2=784 —
    // nearest wins, and allied basing is accepted for own-team bases
    // too (the ARMYBASE is same-owner but farther).
    EXPECT_EQ(base_of(ws, 13), 100u);
}

TEST(AirbaseSynthesis, NeverReassignsBasedSquadrons) {
    WorldState ws = load_world();
    (void)synthesize_squadron_airbases(ws);
    // Squadron 14's wire airbase (100) stays — synthesis only ever
    // touches wire-zero squadrons.
    EXPECT_EQ(base_of(ws, 14), 100u);
}

TEST(AirbaseSynthesis, WarAndGarbageOwnersRejected) {
    // Covered by every assignment above: the DPRK airbase (103) and the
    // garbage-stance Gorn airbase (104) are never picked for a US
    // squadron, and the zero-id objective is never a candidate.
    WorldState ws = load_world();
    (void)synthesize_squadron_airbases(ws);
    for (const auto& u : ws.units) {
        if (u.unit_class != f4::entities::UnitClass::Squadron) continue;
        if (u.owner == 1) {
            EXPECT_NE(u.airbase_id, 103u);
            EXPECT_NE(u.airbase_id, 104u);
        }
        EXPECT_NE(u.airbase_id, 0u);
    }
}

TEST(AirbaseSynthesis, UnresolvableStaysZero) {
    // A world whose only airbase-type objective is owned by a WAR enemy:
    // nothing acceptable → the squadron stays unbased (the entity-side
    // positional fallback remains the last resort).
    WorldState ws;
    ws.load_from_string(R"({
        "version": 63,
        "campaign": {
            "current_time": 0,
            "teams": [
                {"slot": 1, "name": "U.S.",
                 "stance": [0, 1, 5, 3, 3, 3, 3, 3]},
                {"slot": 6, "name": "DPRK",
                 "stance": [0, 5, 1, 3, 3, 3, 3, 3]}
            ]
        },
        "objectives": {
            "count": 1,
            "items": [
                {"id_num": 200, "objective_type": 1, "owner": 6,
                 "x": 50, "y": 50}
            ]
        },
        "units": {
            "count": 1,
            "items": [
                {"unit_class": "squadron", "id_num": 20, "owner": 1,
                 "x": 50, "y": 50, "airbase_id": 0}
            ]
        }
    })");
    const auto report = synthesize_squadron_airbases(ws);
    EXPECT_EQ(report.squadrons, 1);
    EXPECT_EQ(report.assigned, 0);
    EXPECT_EQ(report.unresolved, 1);
    EXPECT_EQ(base_of(ws, 20), 0u);
}

TEST(AirbaseSynthesis, FlightsAndNonSquadronsUntouched) {
    // The synthesis keys on Squadron only — a flight at the same grid
    // with airbase_id 0 stays 0 (flights aren't based; they reference
    // their squadron).
    WorldState ws = load_world();
    (void)synthesize_squadron_airbases(ws);
    for (const auto& u : ws.units) {
        if (u.id_num == 15) {
            EXPECT_EQ(u.unit_class, f4::entities::UnitClass::Flight);
            EXPECT_EQ(u.airbase_id, 0u);
        }
    }
}
