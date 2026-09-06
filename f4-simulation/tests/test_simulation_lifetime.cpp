// f4-simulation/tests/test_simulation_lifetime.cpp
//
// REGRESSION: the "Start Session → access violation in
// ClassTable::vis_type_for()" crash (user report, C4 tranche).
//
// Root cause (fixed in this tranche): Simulation::init_bubble_manager()
// loaded FALCON4.CT into a STACK-LOCAL ClassTable and handed the
// BubbleManager a reference to it. The local died at function return —
// but the BubbleManager holds `const ClassTable& ct_` for the
// Simulation's LIFETIME, so the first tick's deagg
// (update_bubble → deaggregate_ → spawn_vehicles_from_unit →
// vis_type_for) read freed stack memory. The QC and every unit test
// missed it because their fixture worlds deaggregate nothing near the
// bubble center; the user's real install campaign has garrison
// battalions on the airbase the first flight parks at.
//
// These tests pin the fix from three angles:
//
//   1. The class table the Simulation loads is a MEMBER that stays
//      loaded after initialize() returns (the hoist itself).
//   2. The deagg path reads it through the LIVE member: after a deep
//      stack-stomping recursion (which deterministically corrupts the
//      dead frame the old code left behind — the same corruption the
//      user's render loop caused between Start Session and the first
//      tick), force_deaggregate() still resolves every vehicle's vis
//      type and model. Under the old code this is a hard crash (the
//      stomped vector pointer is 0xCD… — an unmapped address) or a
//      zero-vehicle spawn; both fail the test.
//   3. The per-tick bubble update (update_bubble via tick()) deaggregates
//      a co-located battalion on the first tick and reaggregates it
//      when the player moves away — through the same live member.
//
// The companion fix (CampaignSession owning the spawner's
// airfield/airbase-map/template lenders) is pinned in
// test_campaign_session.cpp's finite-parking test.

#include "f4/simulation/simulation.hpp"
#include "f4/simulation/bubble_manager.hpp"

#include <f4/entities/entity.hpp>
#include <f4/simulation/visual_model_component.hpp>
#include <f4/world_types/class_table.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

using namespace f4::simulation;
using f4::entities::EntityHandle;
using f4::entities::EntityId;

namespace {

std::filesystem::path class_table_path() {
    return std::filesystem::path(F4_SOURCE_FIXTURES_DIR) / "falcon4.ct.json";
}

std::filesystem::path f16_config_path() {
    return std::filesystem::path(F4_GENERATED_FIXTURES_DIR) / "f16.json";
}

std::filesystem::path models_hdr_path() {
    return std::filesystem::path(F4_MODELS_DIR) / "KoreaObj.HDR";
}

std::filesystem::path models_lod_path() {
    return std::filesystem::path(F4_MODELS_DIR) / "KoreaObj.LOD";
}

// A campaign world shaped like a save: two teams at war, an AIRBASE
// objective (empty ground_layout — the B.3+ synthetic-field path), a
// squadron, one flight parked at the base, and — the piece the fixture
// worlds never had — a BATTALION WITH VEHICLE GROUPS parked ON the
// airbase grid, exactly like the garrison battalions a real campaign
// save carries. vehicle_type 101 is a DOMAIN_LAND / CLASS_VEHICLE
// entity type whose FALCON4.ct visType[0] = 225 (verified by
// f4-world-convert's class-table tests) and whose vis index resolves a
// KoreaObj model record.
const char* kWorldJson = R"({
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
    "count": 3,
    "decoded": 3,
    "items": [
      {"type": 200, "id_num": 4281, "unit_class": "squadron",
       "entity_type": 273, "domain": 2,
       "x": 390, "y": 455, "z": 0, "owner": 2,
       "airbase_id": 4101, "class_name": "52 TFS PAK"},
      {"type": 200, "id_num": 5001, "unit_class": "flight",
       "entity_type": 273, "domain": 2,
       "x": 390, "y": 455, "z": 0, "owner": 2,
       "mission": 13, "squadron_id": 4281, "package_id": 7029,
       "time_on_target": 43739352,
       "waypoints": [
         {"x": 390, "y": 455, "z": 0,    "action": 1},
         {"x": 420, "y": 460, "z": 2500, "action": 15},
         {"x": 460, "y": 500, "z": 2500, "action": 17},
         {"x": 390, "y": 455, "z": 0,    "action": 7}
       ]},
      {"type": 200, "id_num": 6001, "unit_class": "battalion",
       "entity_type": 180, "domain": 3,
       "x": 390, "y": 455, "z": 0, "owner": 6,
       "vehicle_groups": [
         {"group": 0, "vehicle_type": 101, "count": 3, "live_count": 3}
       ]}
    ]
  }
})";

// Overwrite a deep stack region with a fill pattern. This is the
// deterministic stand-in for the frames the user's app ran between
// initialize() (where the old code's ClassTable local died) and the
// first tick (where the BubbleManager dereferenced it): the dead frame
// lived a few KB below the caller's stack pointer, and 256 KB of
// recursion covers it many times over. The old code's stomped
// std::vector (pointer AND size 0xCD…) then faults on the first
// entries_[idx] read.
//
// (Legitimate stack usage: 512 B of buffer per frame, 512 frames =
// 256 KB — far below the 1 MB default main-thread stack on Windows.)
#if defined(_MSC_VER)
__declspec(noinline)
#else
[[gnu::noinline]]
#endif
void stomp_stack(int depth) {
    volatile unsigned char pad[512];
    for (auto& b : pad) b = 0xCD;
    if (depth > 0) stomp_stack(depth - 1);
}

// The full campaign-flights lifecycle over the crafted world: returns
// the initialized Simulation (bubble manager live) plus the battalion's
// entity id.
struct LiveSim {
    std::unique_ptr<Simulation> sim;
    EntityId battalion;
};

LiveSim make_initialized_sim(const std::filesystem::path& dir) {
    // 1. The world JSON.
    const auto world = dir / "lifetime.world.json";
    {
        std::ofstream f(world);
        f << kWorldJson;
    }

    // 2. The scenario (campaign_flights; every path absolute — the
    //    loader resolves relative paths against the scenario file's
    //    directory, and the temp dir is not the CWD).
    const auto scenario = dir / "lifetime.scenario.json";
    {
        std::ofstream f(scenario);
        f << "{\n"
          << "  \"name\": \"lifetime_regression\",\n"
          << "  \"theater\": \"korea\",\n"
          << "  \"spawn_mode\": \"campaign_flights\",\n"
          // generic_string on every embedded path: Windows backslashes
          // would JSON-escape ("\f" = form feed); forward slashes work
          // on Windows filesystems too.
          << "  \"world_json_path\": \"" << world.generic_string() << "\",\n"
          << "  \"class_table_path\": \"" << class_table_path().generic_string()
          << "\",\n"
          << "  \"models_hdr_path\": \"" << models_hdr_path().generic_string()
          << "\",\n"
          << "  \"models_lod_path\": \"" << models_lod_path().generic_string()
          << "\",\n"
          << "  \"aircraft\": [{\n"
          << "    \"callsign\": \"CAMPAIGN\",\n"
          << "    \"aircraft_config_path\": \""
          << f16_config_path().generic_string() << "\",\n"
          << "    \"aircraft_name\": \"F-16C_50\",\n"
          << "    \"vis_type_index\": 1052,\n"
          << "    \"parking_spot\": {\"x\": 0.0, \"y\": 0.0, \"z\": 0.0},\n"
          << "    \"heading_rad\": 0.0\n"
          << "  }],\n"
          << "  \"campaign_flight_filter\": {\"team\": -1, \"mission\": -1,"
          << " \"max_flights\": 4},\n"
          << "  \"sim_dt\": 0.016666666666666666,\n"
          << "  \"total_ticks\": 1000000000,\n"
          << "  \"record\": false\n"
          << "}\n";
    }

    LiveSim out;
    out.sim = std::make_unique<Simulation>(load_scenario(scenario), dir);
    out.sim->initialize();

    // The battalion entity (VU_ID.num 6001) — the co-located garrison.
    const auto& world_ref = out.sim->world();
    for (const auto eid :
         world_ref.with_component<f4::entities::VehicleCompositionComponent>()) {
        EntityHandle h(eid, const_cast<f4::entities::EntityWorld*>(&world_ref));
        auto* pb = h.get<f4::entities::PropertyBag>();
        if (pb) {
            const auto it = pb->ints.find("vu_id_num");
            if (it != pb->ints.end() && it->second == 6001) {
                out.battalion = eid;
                break;
            }
        }
    }
    return out;
}

std::filesystem::path make_temp_dir() {
    // Unique per call (monotonic counter + steady-clock tag) — portable,
    // no getpid dependency.
    static std::atomic<unsigned> counter{0};
    const auto tag =
        std::to_string(counter.fetch_add(1)) + "_" +
        std::to_string(static_cast<std::uintptr_t>(
            std::chrono::steady_clock::now().time_since_epoch().count() %
            1000000));
    auto dir = std::filesystem::temp_directory_path() /
               ("f4_lifetime_" + tag);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

} // namespace

// ── 1. The class table is a Simulation MEMBER that survives init ──────────
TEST(SimulationLifetime, ClassTableStaysLoadedAfterInitialize) {
    if (!std::filesystem::exists(f16_config_path())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    const auto dir = make_temp_dir();
    auto live = make_initialized_sim(dir);

    // The member load: initialize() loaded the table once and every
    // long-lived borrower (spawn paths, BubbleManager) references it.
    ASSERT_TRUE(live.sim->class_table().loaded());
    // The lookups the deagg path depends on (vehicle_type 101 → 225).
    EXPECT_EQ(live.sim->class_table().vis_type_for(101, 0), 225);

    // V-3DLIVE: every spawned aircraft carries its vis type (the
    // renderable identity — the session's empty db means no model
    // records, the host resolves meshes from vis_type alone).
    for (const auto eid : live.sim->aircraft_entities()) {
        EntityHandle h(eid, &live.sim->world());
        const auto* vis = h.get<f4::simulation::VisualModelComponent>();
        ASSERT_NE(vis, nullptr);
        EXPECT_GT(vis->vis_type, 0)
            << "flight aircraft " << eid.value
            << " carries no vis type — the live 3D pass has nothing "
               "to draw";
    }
    for (const auto eid : live.sim->squadron_aircraft_entities()) {
        EntityHandle h(eid, &live.sim->world());
        const auto* vis = h.get<f4::simulation::VisualModelComponent>();
        ASSERT_NE(vis, nullptr);
        EXPECT_GT(vis->vis_type, 0)
            << "parked squadron aircraft " << eid.value
            << " carries no vis type";
    }

    // The bubble manager exists (the world has vehicle-composition
    // units) — the object whose dangling ct_ used to crash.
    ASSERT_NE(live.sim->bubble_manager(), nullptr);

    std::filesystem::remove_all(dir);
}

// ── 2. The deagg path reads the LIVE member after the dead frame is
//      stomped — the deterministic Start Session crash repro ───────────────
TEST(SimulationLifetime, DeaggReadsLiveClassTableAfterStackStomp) {
    if (!std::filesystem::exists(f16_config_path())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    const auto dir = make_temp_dir();
    auto live = make_initialized_sim(dir);
    auto* sim = live.sim.get();

    // initialize() has RETURNED: under the old code the BubbleManager's
    // ct_ already dangled here. Stomp the dead frame the way the user's
    // render loop did, then deaggregate — the exact crash sequence.
    stomp_stack(512);

    ASSERT_TRUE(live.battalion.valid());
    sim->bubble_manager()->force_deaggregate(live.battalion);

    // All 3 vehicles spawned, every one with a resolved model record —
    // only possible if vis_type_for read a LIVE table.
    const auto& vehicles = sim->bubble_manager()->vehicle_entities();
    ASSERT_EQ(vehicles.size(), std::size_t{3});
    for (const auto vid : vehicles) {
        EntityHandle h(vid, &sim->world());
        const auto* vis = h.get<f4::simulation::VisualModelComponent>();
        ASSERT_NE(vis, nullptr);
        // V-3DLIVE: the identity rides in vis_type (225 for the
        // fixture's vehicle_type 101) — model_record needs a db.
        EXPECT_EQ(vis->vis_type, 225);
        EXPECT_NE(vis->model_record, nullptr)
            << "vehicle " << vid.value << " has no model — the class "
               "table lookup returned garbage";
    }

    std::filesystem::remove_all(dir);
}

// ── 3. The per-tick bubble path (the user's crash was on tick #1) ─────────
TEST(SimulationLifetime, TickDeaggReggregatesCoLocatedBattalion) {
    if (!std::filesystem::exists(f16_config_path())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    const auto dir = make_temp_dir();
    auto live = make_initialized_sim(dir);
    auto* sim = live.sim.get();

    // Frame-shaped ticking (the campaign session's advance shape):
    // several small ticks — the FIRST one runs update_bubble over the
    // parked first flight, which deaggregates the co-located garrison
    // battalion through the (now live) class table.
    for (int i = 0; i < 10; ++i) {
        sim->tick(1.0 / 60.0);
    }
    EXPECT_EQ(sim->bubble_manager()->deaggregated_unit_count(),
              std::size_t{1});
    EXPECT_EQ(sim->bubble_manager()->vehicle_entities().size(),
              std::size_t{3});

    // Player far away (reaggregate: the vehicles despawn — the same
    // ct/db lenders on the reverse path).
    sim->bubble_manager()->update(f4::geo::WorldPosition(
        3.0e6, 3.0e6, 0.0));   // the far corner of the theater
    EXPECT_EQ(sim->bubble_manager()->deaggregated_unit_count(),
              std::size_t{0});
    EXPECT_EQ(sim->bubble_manager()->vehicle_entities().size(),
              std::size_t{0});

    std::filesystem::remove_all(dir);
}
