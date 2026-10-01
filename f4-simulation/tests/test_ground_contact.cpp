// f4-simulation/tests/test_ground_contact.cpp
//
// REPAIR-T1 (Docs/CAMPAIGN_REPAIR_PLAN.md) — ground-contact truth.
//
// The FM's airborne->ground transition is a one-shot latch; the host's
// per-tick sweep classifies each touchdown (a landing/takeoff-owned
// brain phase is aviation, anything else is a crash) and runs the
// zombie detector (Enroute brain + on-ground FM past the grace — the
// NAV-D1 ground-spawned flight never produces a transition to catch).
// A crash parks the corpse (dormant brain + FM — nothing else stops a
// killed aircraft's FM) and publishes EntityKilledMessage(cause=
// "terrain"), so the C1 sink books the loss and the wreck reaper reaps.
//
// Before this sweep the transition was SILENT: the armed war's airborne
// population collapsed 96 -> 4 with zero losses booked, and the BARCAP
// filtered run ended with a 36-minute on-ground ToWaypoint corpse.

#include <f4/simulation/campaign_session.hpp>
#include <f4/flight/flight_model_component.hpp>
#include <f4/entities/entity.hpp>

#include <f4/weapons/f4_weapons.hpp>  // EntityKilledMessage

#include <gtest/gtest.h>


#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <algorithm>
#include <fstream>
#include <memory>
#include <string>

using namespace f4::simulation;
using f4::entities::EntityHandle;

namespace {

std::filesystem::path class_table_path() {
    return std::filesystem::path(F4_SOURCE_FIXTURES_DIR) / "falcon4.ct.json";
}

std::filesystem::path f16_config_path() {
    return std::filesystem::path(F4_GENERATED_FIXTURES_DIR) / "f16.json";
}

// The crafted world (the tier rig's shape, minimal): one airbase
// objective, one squadron, one flight with a 4-waypoint route (SPEED
// mode — no arrive/depart times). The flight is the crash victim; its
// squadron books the loss.
std::string contact_world_json() {
    return R"({
  "version": 71,
  "theater": "korea",
  "campaign": {
    "current_time": 38574360,
    "te_team": 2,
    "teams": [
      {"slot": 2, "name": "ROK", "member": [0,0,1,0,0,0,0,0],
       "stance": [0,0,0,0,0,0,5,0]},
      {"slot": 6, "name": "DPRK", "member": [0,0,0,0,0,0,1,0],
       "stance": [0,0,5,0,0,0,0,0]}
    ]
  },
  "objectives": {
    "count": 1,
    "decoded": 1,
    "items": [
      {"type": 100, "id_num": 4101, "id_creator": 0,
       "objective_type": 1,
       "x": 390, "y": 455, "z": 0,
       "owner": 2, "nameid": 1627, "priority": 10,
       "fstatus": [0, 0], "links": []}
    ]
  },
  "units": {
    "count": 2,
    "decoded": 2,
    "items": [
      {"type": 200, "id_num": 4281, "unit_class": "squadron",
       "entity_type": 273, "domain": 2,
       "x": 390, "y": 455, "z": 0, "owner": 2,
       "airbase_id": 4101, "class_name": "52 TFS PAK"},
      {"type": 200, "id_num": 5001, "unit_class": "flight",
       "entity_type": 273, "domain": 2,
       "x": 390, "y": 455, "z": 0, "owner": 2,
       "mission": 13, "squadron_id": 4281, "package_id": 7029,
       "time_on_target": 38576160,
       "waypoints": [
         {"x": 390, "y": 455, "z": 0,    "action": 1},
         {"x": 420, "y": 460, "z": 2500, "action": 15},
         {"x": 460, "y": 500, "z": 2500, "action": 17},
         {"x": 390, "y": 455, "z": 0,    "action": 7}
       ]}
    ]
  }
})";
}

std::filesystem::path make_temp_dir() {
    static std::atomic<unsigned> counter{0};
    const auto dir = std::filesystem::temp_directory_path() /
                     ("f4_contact_" +
                      std::to_string(counter.fetch_add(1)) + "_" +
                      std::to_string(
                          std::chrono::steady_clock::now()
                              .time_since_epoch()
                              .count()));
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

struct ContactRig {
    std::filesystem::path dir;
    std::filesystem::path world;
    std::unique_ptr<CampaignSession> session;

    static ContactRig make(FidelityPolicy policy) {
        ContactRig rig;
        rig.dir = make_temp_dir();
        rig.world = rig.dir / "contact.world.json";
        {
            std::ofstream f(rig.world);
            f << contact_world_json();
        }
        CampaignSessionOptions opts;
        opts.world_json = rig.world;
        opts.class_table = class_table_path();
        opts.aircraft_config = f16_config_path();
        opts.mission_profiles = F4_MISSION_PROFILES_JSON;
        opts.fidelity_policy = policy;
        opts.max_flights = 8;
        // No synthetic spawns: the assertions count only the save's
        // own flight.
        opts.tasking_cycle_sec = 1000000;
        opts.reinforce_period_sec = 0;
        opts.atm_pipeline = false;
        opts.max_steps_per_advance = 400000;
        std::string err;
        rig.session = CampaignSession::create(opts, &err);
        EXPECT_NE(rig.session, nullptr) << err;
        return rig;
    }

    std::uint32_t flight_vu() const { return 5001; }

    // The one aircraft the crafted world materializes (the tier deagg's
    // in tiered sessions, the initialize spawn in full-fidelity ones).
    f4::entities::EntityId the_aircraft() const {
        const auto& roster = session->sim().aircraft_entities();
        EXPECT_EQ(roster.size(), 1u);
        return roster.empty() ? f4::entities::EntityId{} : roster.front();
    }
};

// A kill counter on the session bus, filtered to one entity.
struct KillWatch {
    int terrain = 0;
    int grounded_enroute = 0;
    int total = 0;
    std::size_t sub = static_cast<std::size_t>(-1);

    void attach(CampaignSession& s, std::uint64_t victim) {
        sub = s.sim().bus().subscribe<f4::weapons::EntityKilledMessage>(
            [this, victim](const f4::weapons::EntityKilledMessage& m) {
                if (m.target_id != victim) return;
                ++total;
                if (m.cause != nullptr) {
                    if (std::string_view{m.cause} == "terrain") ++terrain;
                    if (std::string_view{m.cause} == "grounded-enroute")
                        ++grounded_enroute;
                }
            });
    }
};

bool is_dead(CampaignSession& s, f4::entities::EntityId id) {
    EntityHandle h(id, &s.sim().world());
    const auto* dmg = h.get<f4::entities::DamageStateComponent>();
    return dmg != nullptr && dmg->killed;
}

}  // namespace

// ── the latch classification ────────────────────────────────────────────────

// A touchdown while the brain is in a landing/takeoff-owned phase is
// aviation: no kill, the aircraft survives. The full-fidelity spawn
// sits in its Ground phase (the takeoff FSM) for the whole window.
TEST(GroundContact, TouchdownInGroundPhaseIsAviation) {
    if (!std::filesystem::exists(f16_config_path())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    auto rig = ContactRig::make(FidelityPolicy::FullFidelity);
    ASSERT_NE(rig.session, nullptr);
    rig.session->advance(1.0);  // spawn exists, still in Ground
    const auto id = rig.the_aircraft();
    ASSERT_TRUE(id.valid());

    KillWatch watch;
    watch.attach(*rig.session, id.value);

    // Force the contact: park the state 1 ft over the terrain,
    // airborne — the FM's next update produces the transition.
    {
        EntityHandle h(id, &rig.session->sim().world());
        auto* fm = h.get<f4::flight::FlightModelComponent>();
        ASSERT_NE(fm, nullptr);
        fm->model().state().kin.z = -1.0;  // NED: 1 ft AGL over terrain 0
        fm->model().state().gear.inAir = true;
    }
    rig.session->advance(0.5);

    EXPECT_EQ(watch.total, 0) << "a Ground-phase touchdown was killed";
    EXPECT_FALSE(is_dead(*rig.session, id));
    EXPECT_EQ(rig.session->sim().aircraft_entities().size(), 1u);
}

// A touchdown while the brain flies the mission (Enroute — the tier
// deagg's air-spawn phase) is a CRASH: the corpse parks (dormant brain
// + FM) and the terrain kill publishes. This is the BARCAP trace's
// exact fate, now booked instead of silent.
TEST(GroundContact, EnrouteTouchdownIsACrash) {
    if (!std::filesystem::exists(f16_config_path())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    auto rig = ContactRig::make(FidelityPolicy::Tiered);
    ASSERT_NE(rig.session, nullptr);
    rig.session->force_deaggregate_flight(rig.flight_vu());
    rig.session->advance(1.0);
    const auto id = rig.the_aircraft();
    ASSERT_TRUE(id.valid());

    KillWatch watch;
    watch.attach(*rig.session, id.value);

    // Force the contact (the exact BARCAP descent shape: airborne,
    // then the ground arrives).
    {
        EntityHandle h(id, &rig.session->sim().world());
        auto* fm = h.get<f4::flight::FlightModelComponent>();
        ASSERT_NE(fm, nullptr);
        fm->model().state().kin.z = -1.0;
        fm->model().state().gear.inAir = true;
    }
    rig.session->advance(1.0);

    EXPECT_EQ(watch.terrain, 1)
        << "the Enroute touchdown never published the terrain kill";
    EXPECT_EQ(watch.total, 1) << "the kill must publish exactly once";

    // The tiered session's fold heard the kill: the flight folds as
    // DESTROYED (FID-P0) and the corpse retires — the aircraft is
    // either gone (folded) or dead (not yet folded); NEVER alive.
    bool row_destroyed = false;
    for (const auto& t : rig.session->flight_tiers()) {
        if (t.vu == rig.flight_vu()) row_destroyed = t.destroyed;
    }
    const bool still_in_roster =
        std::find(rig.session->sim().aircraft_entities().begin(),
                  rig.session->sim().aircraft_entities().end(),
                  id) != rig.session->sim().aircraft_entities().end();
    EXPECT_TRUE(row_destroyed || !still_in_roster)
        << "the crashed flight neither folded nor retired";
    if (still_in_roster) {
        EXPECT_TRUE(is_dead(*rig.session, id));
    }
}

// ── the zombie detector ─────────────────────────────────────────────────────

// A brain in Enroute whose FM reads on-ground past the grace is a
// crash even without a fresh transition — the NAV-D1 ground-spawned
// flight never leaves the ground, so no transition ever fires for it.
// The grace is 10 s; 12 s of campaign time must be enough.
TEST(GroundContact, EnrouteZombieCrashesAfterTheGrace) {
    if (!std::filesystem::exists(f16_config_path())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    auto rig = ContactRig::make(FidelityPolicy::Tiered);
    ASSERT_NE(rig.session, nullptr);
    rig.session->force_deaggregate_flight(rig.flight_vu());
    rig.session->advance(1.0);
    const auto id = rig.the_aircraft();
    ASSERT_TRUE(id.valid());

    KillWatch watch;
    watch.attach(*rig.session, id.value);

    // Force the zombie shape MID-ROUTE: grounded, no transition latch,
    // and an FM that can never rescue itself. With the CAMP-FAF-GUARD
    // fix the nav module FLIES, and a live brain grounded at its base
    // self-rescues (the splice lands the cursor on the home leg, the
    // nav completes, the phase leaves Enroute — healthy behavior the
    // detector must not fire on). A WEDGED FM mid-route (this freeze —
    // the state a hung physics step presents, far from every waypoint
    // so the nav can neither complete nor capture) stays an Enroute
    // corpse: the state the detector exists to catch.
    {
        EntityHandle h(id, &rig.session->sim().world());
        auto* fm = h.get<f4::flight::FlightModelComponent>();
        ASSERT_NE(fm, nullptr);
        auto& st = fm->model().state();
        st.kin.x = 524.0 * 1024.0;  // NED north = grid y * 1024
        st.kin.y = 545.0 * 1024.0;  // NED east  = grid x * 1024
        st.kin.z = -6.0;            // sitting on its gear
        st.kin.vt = 0.0;
        st.gear.inAir = false;
        fm->set_dormant(true);  // never updates again: inAir stays false
    }
    for (int s = 0; s < 12 && watch.total == 0; ++s) {
        rig.session->advance(1.0);
    }

    EXPECT_EQ(watch.grounded_enroute, 1)
        << "the Enroute-on-ground zombie survived the grace";
    // Exactly one kill — a re-fire would double-book the loss.
    EXPECT_EQ(watch.total, 1);
}

// ── the corpse parks (the full-fidelity world keeps it) ─────────────────────

// Outside the tier machinery (no fold, wreck_hold 0 = no reaper), the
// crashed corpse stays in the roster — parked: dormant brain + FM, dead
// damage state. Nothing may keep flying a killed aircraft.
TEST(GroundContact, CrashParksTheCorpseFullFidelity) {
    if (!std::filesystem::exists(f16_config_path())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    auto rig = ContactRig::make(FidelityPolicy::FullFidelity);
    ASSERT_NE(rig.session, nullptr);

    // The spawned flight flies its route: poll until it is airborne in
    // Enroute (the B.3 shape: liftoff ~1 min, Enroute by ~2-3 min).
    f4::entities::EntityId id{};
    bool airborne_enroute = false;
    for (int s = 0; s < 600 && !airborne_enroute; ++s) {
        rig.session->advance(1.0);
        const auto& roster = rig.session->sim().aircraft_entities();
        if (roster.empty()) continue;
        EntityHandle h(roster.front(), &rig.session->sim().world());
        const auto* brain = h.get<f4::ai::BrainComponent>();
        const auto* fm = h.get<f4::flight::FlightModelComponent>();
        airborne_enroute =
            brain != nullptr && fm != nullptr &&
            brain->phase() == f4::ai::BrainComponent::Phase::Enroute &&
            fm->state().gear.inAir;
        id = roster.front();
    }
    ASSERT_TRUE(airborne_enroute)
        << "the flight never went airborne-Enroute in 10 min";

    KillWatch watch;
    watch.attach(*rig.session, id.value);

    // Force the contact.
    {
        EntityHandle h(id, &rig.session->sim().world());
        auto* fm = h.get<f4::flight::FlightModelComponent>();
        ASSERT_NE(fm, nullptr);
        fm->model().state().kin.z = -1.0;
        fm->model().state().gear.inAir = true;
    }
    rig.session->advance(0.5);

    ASSERT_EQ(watch.terrain, 1);
    EXPECT_TRUE(is_dead(*rig.session, id));
    {
        EntityHandle h(id, &rig.session->sim().world());
        const auto* brain = h.get<f4::ai::BrainComponent>();
        ASSERT_NE(brain, nullptr);
        EXPECT_TRUE(brain->is_dormant()) << "the corpse must park";
        const auto* fm = h.get<f4::flight::FlightModelComponent>();
        ASSERT_NE(fm, nullptr);
        EXPECT_TRUE(fm->is_dormant()) << "the corpse's FM must stop";
    }
    // Exactly one kill: the parked corpse is never re-classified.
    rig.session->advance(2.0);
    EXPECT_EQ(watch.total, 1);
}

// ── the books ───────────────────────────────────────────────────────────────

// The crash victim is a campaign aircraft (the save's flight, its
// origin stamped at spawn): the C1 sink books the air loss — the
// ledger is non-empty, exactly once.
TEST(GroundContact, CrashBooksTheLedgerLoss) {
    if (!std::filesystem::exists(f16_config_path())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    auto rig = ContactRig::make(FidelityPolicy::Tiered);
    ASSERT_NE(rig.session, nullptr);
    rig.session->force_deaggregate_flight(rig.flight_vu());
    rig.session->advance(1.0);
    const auto id = rig.the_aircraft();
    ASSERT_TRUE(id.valid());

    KillWatch watch;
    watch.attach(*rig.session, id.value);

    {
        EntityHandle h(id, &rig.session->sim().world());
        auto* fm = h.get<f4::flight::FlightModelComponent>();
        ASSERT_NE(fm, nullptr);
        fm->model().state().kin.z = -1.0;
        fm->model().state().gear.inAir = true;
    }
    rig.session->advance(1.0);

    ASSERT_EQ(watch.total, 1);
    // The ledger carries the loss (the flight's squadron/team books).
    const std::string ledger = rig.session->ledger_json();
    EXPECT_NE(ledger.find("\"air_losses\":1"), std::string::npos)
        << "the terrain crash never booked: " << ledger;
}
