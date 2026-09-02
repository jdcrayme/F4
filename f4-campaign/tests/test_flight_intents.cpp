// f4-campaign/tests/test_flight_intents.cpp
//
// B.3 tranche tests:
//   1. MissionCategory — the total many-to-one mapping from mission bytes
//      to behavioral categories (the viewer's symbol/QC granularity).
//   2. emit_flight_intents — MissionIntents derived from LIVE saved
//      flights (the TestCamp.cam shape: tasked flights with mission,
//      TOT, package, squadron, callsign), published on the bus.

#include <f4/campaign/campaign.hpp>
#include <f4/campaign/mission_type.hpp>
#include <f4/world/world_adapters.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace f4::campaign;
using f4::entities::UnitClass;
using f4::entities::WaypointState;

namespace {

// Bus-side recorder — asserts the publish side of the contract.
struct Collector {
    std::vector<MissionIntent> seen;
    explicit Collector(f4::messaging::MessageBus& bus) {
        bus.subscribe<MissionIntent>(
            [this](const MissionIntent& in) { seen.push_back(in); });
    }
};

// A save-shaped world: 2 teams (USA slot 1 vs DPRK slot 6, at war), one
// squadron per team, three flights — two tasked (a 4-ship BARCAP for USA,
// a 2-ship OCASTRIKE for DPRK) and one untasked — plus the DPRK package
// that owns the strike flight.
f4::world::WorldState make_flight_world() {
    using f4::world::TeamState;
    using f4::world::UnitState;

    f4::world::WorldState ws;
    ws.version = 71;
    ws.campaign.current_time = 38574360;  // ~half a day into the war

    ws.teams.resize(8);
    ws.teams[1] = TeamState{1, 1, 1, "USA", "", 0, 0, {}};
    ws.teams[6] = TeamState{6, 6, 6, "DPRK", "", 0, 0, {}};
    // War stance: USA hostile to DPRK and back.
    ws.teams[1].stance = {0, 0, 0, 0, 0, 0, -1, 0};
    ws.teams[6].stance = {0, -1, 0, 0, 0, 0, 0, 0};

    UnitState sq_usa;
    sq_usa.unit_class = UnitClass::Squadron;
    sq_usa.domain = 2;
    sq_usa.x = 390; sq_usa.y = 455;
    sq_usa.owner = 1; sq_usa.id_num = 4281;
    sq_usa.class_name = "52 TFS PAK";

    UnitState sq_dprk;
    sq_dprk.unit_class = UnitClass::Squadron;
    sq_dprk.domain = 2;
    sq_dprk.x = 400; sq_dprk.y = 480;
    sq_dprk.owner = 6; sq_dprk.id_num = 4300;
    sq_dprk.class_name = "1st Guards Fighter Regiment";

    UnitState cap_flight;  // 4-ship BARCAP, USA, in package 7029
    cap_flight.unit_class = UnitClass::Flight;
    cap_flight.domain = 2;
    cap_flight.x = 392; cap_flight.y = 451;
    cap_flight.owner = 1; cap_flight.id_num = 10678;
    cap_flight.roster = 0xA0;              // 2 + 2 = 4 aircraft
    cap_flight.mission = 1;                // AMIS_BARCAP
    cap_flight.time_on_target = 43739352;
    cap_flight.package_id = 7029;
    cap_flight.squadron_id = 4281;
    cap_flight.callsign_id = 125; cap_flight.callsign_num = 1;

    UnitState strike_flight;  // 2-ship OCASTRIKE, DPRK, un-packaged
    strike_flight.unit_class = UnitClass::Flight;
    strike_flight.domain = 2;
    strike_flight.x = 402; strike_flight.y = 482;
    strike_flight.owner = 6; strike_flight.id_num = 10700;
    strike_flight.roster = 0x80;              // 2 aircraft
    strike_flight.mission = 12;               // AMIS_OCASTRIKE
    strike_flight.time_on_target = 44000000;
    strike_flight.package_id = 0;
    strike_flight.squadron_id = 4300;
    WaypointState w;
    w.x = 402; w.y = 482; w.z = 0; w.action = 1;  // WP_TAKEOFF
    strike_flight.waypoints.push_back(w);

    UnitState idle_flight;  // untasked — must produce NO intent
    idle_flight.unit_class = UnitClass::Flight;
    idle_flight.domain = 2;
    idle_flight.x = 395; idle_flight.y = 457;
    idle_flight.owner = 1; idle_flight.id_num = 10701;
    idle_flight.mission = 0;  // AMIS_NONE

    UnitState pkg;
    pkg.unit_class = UnitClass::Package;
    pkg.domain = 2;
    pkg.x = 392; pkg.y = 451;
    pkg.owner = 1; pkg.id_num = 7029;
    pkg.class_name = "Strike Package";
    pkg.element_ids = {10678};

    ws.units = {sq_usa, sq_dprk, cap_flight, strike_flight, idle_flight, pkg};
    return ws;
}

} // namespace

// ============================================================================
// MissionCategory
// ============================================================================

TEST(MissionCategory, MappingIsTotalAndStable) {
    // Every byte 0..40 lands in a category; the anchors below pin the
    // table's rows (same discipline as the mission-name ordering tests).
    EXPECT_EQ(mission_category(0), MissionCategory::None);
    EXPECT_EQ(mission_category(1), MissionCategory::CAP);       // BARCAP
    EXPECT_EQ(mission_category(6), MissionCategory::CAP);       // AMBUSHCAP
    EXPECT_EQ(mission_category(36), MissionCategory::CAP);      // PATROL
    EXPECT_EQ(mission_category(7), MissionCategory::Sweep);
    EXPECT_EQ(mission_category(8), MissionCategory::Intercept); // ALERT
    EXPECT_EQ(mission_category(9), MissionCategory::Intercept);
    EXPECT_EQ(mission_category(11), MissionCategory::Escort);   // SEADESCORT
    EXPECT_EQ(mission_category(13), MissionCategory::Strike);   // INTSTRIKE
    EXPECT_EQ(mission_category(24), MissionCategory::Strike);   // STRATBOMB
    EXPECT_EQ(mission_category(17), MissionCategory::SEAD);
    EXPECT_EQ(mission_category(20), MissionCategory::CAS);
    EXPECT_EQ(mission_category(31), MissionCategory::CAS);      // FAC
    EXPECT_EQ(mission_category(22), MissionCategory::Recon);    // INT
    EXPECT_EQ(mission_category(30), MissionCategory::Recon);    // BDA
    EXPECT_EQ(mission_category(27), MissionCategory::Support);  // TANKER
    EXPECT_EQ(mission_category(33), MissionCategory::Support);  // AIRLIFT
    EXPECT_EQ(mission_category(40), MissionCategory::Support);  // SEARCH
    EXPECT_EQ(mission_category(37), MissionCategory::Other);    // TRAINING
    // Corrupt/out-of-range bytes degrade to Other.
    EXPECT_EQ(mission_category(41), MissionCategory::Other);
    EXPECT_EQ(mission_category(255), MissionCategory::Other);
}

TEST(MissionCategory, NamesAreShortAndUnique) {
    // The name is the stable rendering key — spot-check distinctness of
    // every category's name via a de-dup set.
    std::vector<std::string> names;
    for (int c = 0; c <= static_cast<int>(MissionCategory::Other); ++c) {
        names.emplace_back(mission_category_name(
            static_cast<MissionCategory>(c)));
    }
    std::sort(names.begin(), names.end());
    EXPECT_EQ(std::unique(names.begin(), names.end()), names.end());
    EXPECT_EQ(mission_category_name(MissionCategory::SEAD), "SEAD");
    EXPECT_EQ(mission_category_name(MissionCategory::None), "None");
}

// ============================================================================
// emit_flight_intents
// ============================================================================

TEST(EmitFlightIntents, TaskedFlightsOnly) {
    auto ws = make_flight_world();
    f4::world::WorldStateAdapters adapters(ws);
    f4::messaging::MessageBus bus;
    Collector collect(bus);

    const auto intents = emit_flight_intents(
        static_cast<const f4::world::IUnitCoreSource&>(adapters.units),
        static_cast<const f4::world::IFlightSource&>(adapters.units),
        bus, ws.campaign.current_time,
        &static_cast<const f4::world::ITeamSource&>(adapters.teams));

    // Two tasked flights, one untasked → two intents, and the bus saw
    // exactly the same two.
    ASSERT_EQ(intents.size(), 2u);
    ASSERT_EQ(collect.seen.size(), 2u);
    EXPECT_EQ(intents, collect.seen);

    // Untasked flight 10701 must not appear anywhere.
    for (const auto& in : intents) {
        EXPECT_NE(in.flight_id, 10701u);
    }
}

TEST(EmitFlightIntents, CarriesTheSaveShapedFacts) {
    auto ws = make_flight_world();
    f4::world::WorldStateAdapters adapters(ws);
    f4::messaging::MessageBus bus;

    const auto intents = emit_flight_intents(
        static_cast<const f4::world::IUnitCoreSource&>(adapters.units),
        static_cast<const f4::world::IFlightSource&>(adapters.units),
        bus, ws.campaign.current_time);

    // Wire order: flights appear in unit order (10678 before 10700).
    ASSERT_EQ(intents.size(), 2u);

    const auto& cap = intents[0];
    EXPECT_EQ(cap.flight_id, 10678u);
    EXPECT_EQ(cap.package_id, 7029u);
    EXPECT_EQ(cap.squadron_id, 4281u);
    EXPECT_EQ(cap.squadron_name, "52 TFS PAK");
    EXPECT_EQ(cap.team, 1);
    EXPECT_EQ(cap.mission_byte, 1);
    EXPECT_EQ(cap.mission_name, "AMIS_BARCAP");
    EXPECT_EQ(cap.aircraft_count, 4);                 // 0xA0 roster
    EXPECT_EQ(cap.issued_time, 38574360);
    EXPECT_EQ(cap.time_on_target, 43739352);          // absolute, unbated

    const auto& strike = intents[1];
    EXPECT_EQ(strike.flight_id, 10700u);
    EXPECT_EQ(strike.team, 6);
    EXPECT_EQ(strike.mission_byte, 12);
    EXPECT_EQ(strike.aircraft_count, 2);              // 0x80 roster
    EXPECT_EQ(strike.package_id, 0u);                 // un-packaged
}

TEST(EmitFlightIntents, TeamNamesResolvedWhenSourceProvided) {
    auto ws = make_flight_world();
    f4::world::WorldStateAdapters adapters(ws);
    f4::messaging::MessageBus bus;

    // With the team source: names filled.
    {
        const auto intents = emit_flight_intents(
            static_cast<const f4::world::IUnitCoreSource&>(adapters.units),
            static_cast<const f4::world::IFlightSource&>(adapters.units),
            bus, 0, &static_cast<const f4::world::ITeamSource&>(adapters.teams));
        ASSERT_EQ(intents.size(), 2u);
        EXPECT_EQ(intents[0].team_name, "USA");
        EXPECT_EQ(intents[1].team_name, "DPRK");
    }
    // Without: names empty (the contract allows it).
    {
        const auto intents = emit_flight_intents(
            static_cast<const f4::world::IUnitCoreSource&>(adapters.units),
            static_cast<const f4::world::IFlightSource&>(adapters.units),
            bus, 0);
        ASSERT_EQ(intents.size(), 2u);
        EXPECT_TRUE(intents[0].team_name.empty());
    }
}
