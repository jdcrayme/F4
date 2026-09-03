// f4-simulation/tests/test_campaign_result_sink.cpp
//
// C1 — the result sink: sim outcomes resolve back to campaign identity
// and land in the ledger.
//
//   1. Origin stamping: every campaign-spawned aircraft carries
//      CampaignOriginComponent (flight/squadron VUs, team slot,
//      callsign bytes) — the data the sink attributes through.
//   2. Kill classification (manual events): origin-ful victim → air
//      loss; origin-less victim + origin-ful shooter → ag credit;
//      neither → unclassified (counted, unbooked).
//   3. THE E2E MISSILE KILL: launch → flyout → fuze → EntityKilled on
//      the bus → sink → ledger (pool −1, squadron loss, aa credit) —
//      the exact chain a BVR fight produces, deterministic and
//      byte-stable across two runs.
//   4. Objective damage: bomb impact + final-state sync → the ledger's
//      fstatus record → apply_to → repopulate → DamageBitmapComponent
//      carries the damage (the save-format face round-trips).

#include <f4/simulation/campaign_bridge.hpp>
#include <f4/simulation/campaign_origin.hpp>
#include <f4/simulation/campaign_result_sink.hpp>
#include <f4/campaign/result_ledger.hpp>
#include <f4/campaign/world_writeback.hpp>
#include <f4/entities/entity.hpp>
#include <f4/messaging/bus.hpp>
#include <f4/weapons/damage.hpp>
#include <f4/weapons/messages.hpp>
#include <f4/weapons/missile_battery.hpp>
#include <f4/weapons/weapon_class_table.hpp>
#include <f4/weapons/weapon_store.hpp>
#include <f4/world/world_adapters.hpp>
#include <f4/world/world_loader.hpp>
#include <f4/data/aircraft_config.hpp>
#include <f4/data/config_loader.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace f4::simulation;
using f4::campaign::CampaignResultLedger;
using f4::entities::DamageBitmapComponent;
using f4::entities::DamageStateComponent;
using f4::entities::EntityHandle;
using f4::entities::EntityId;
using f4::entities::EntityWorld;
using f4::entities::FeatureEntryState;
using f4::entities::FeatureSetComponent;
using f4::entities::FlightPlanComponent;
using f4::entities::PropertyBag;
using f4::entities::TransformComponent;
using f4::entities::UnitClass;
using f4::messaging::MessageBus;
using f4::world::ObjectiveState;
using f4::world::TeamState;
using f4::world::UnitState;
using f4::world::WorldState;
using f4::world::WorldStateAdapters;

namespace {

constexpr double kDt = 1.0 / 60.0;

/// fstatus bytes for "feature 0 normal, feature 1 destroyed" (2 bits
/// per feature: 00 11 -> 0x0C). A named constant — brace-initialized
/// vectors can't go inside EXPECT_EQ macros (the comma splits them).
const std::vector<std::uint8_t> kDamagedFstatus{0x0C, 0x00};

bool loadF16Config(f4::data::AircraftConfig& cfg) {
    const char* env = std::getenv("F4_GENERATED_FIXTURES_DIR");
    std::string dir = env ? env : "";
#ifdef F4_GENERATED_FIXTURES_DIR
    if (dir.empty()) dir = F4_GENERATED_FIXTURES_DIR;
#endif
    if (dir.empty()) return false;
    const auto path = std::filesystem::path(dir) / "f16.json";
    if (!std::filesystem::exists(path)) return false;
    auto result = f4::data::loadConfig(path.string());
    if (!result.ok) return false;
    cfg = std::move(result.config);
    return true;
}

// The two-team flight world (test_campaign_spawner's shape): ROK(2) and
// DPRK(6) at war, an airbase objective (VU 4101), squadron VU 4281, and
// three flights (ROK 5001/5002, DPRK 5003).
WorldState make_flight_world() {
    WorldState ws;
    ws.version = 71;
    ws.campaign.current_time = 38574360;
    ws.campaign.te_team = 2;
    ws.campaign.te_number_aircraft = {0, 0, 12, 0, 0, 0, 12, 0};

    ws.teams.resize(8);
    ws.teams[2] = TeamState{2, 2, 2, "ROK", "", 0, 0, {}};
    ws.teams[6] = TeamState{6, 6, 6, "DPRK", "", 0, 0, {}};
    ws.teams[2].stance = {0, 0, 0, 0, 0, 0, -1, 0};
    ws.teams[6].stance = {0, 0, -1, 0, 0, 0, 0, 0};

    ObjectiveState ab;
    ab.type = 100; ab.entity_type = 100; ab.x = 390; ab.y = 455;
    ab.owner = 2; ab.id_num = 4101; ab.camp_id = 50;
    ab.objective_type = 1;  // TYPE_AIRBASE
    ws.objectives = {ab};

    UnitState sq;
    sq.unit_class = UnitClass::Squadron;
    sq.domain = 2;
    sq.x = 390; sq.y = 455; sq.owner = 2; sq.id_num = 4281;
    sq.class_name = "52 TFS PAK";
    sq.airbase_id = 4101;
    ws.units = {sq};

    auto make_flight = [&](std::uint32_t id, std::uint8_t owner,
                           std::uint8_t mission) {
        UnitState fl;
        fl.unit_class = UnitClass::Flight;
        fl.domain = 2;
        fl.x = 392; fl.y = 451; fl.owner = owner; fl.id_num = id;
        fl.mission = mission;
        fl.squadron_id = 4281;
        fl.callsign_id = 125; fl.callsign_num = 1;
        return fl;
    };

    UnitState pkg;
    pkg.unit_class = UnitClass::Package;
    pkg.domain = 2;
    pkg.x = 392; pkg.y = 451; pkg.owner = 2; pkg.id_num = 7029;
    pkg.element_ids = {5001};

    ws.units = {sq, make_flight(5001, 2, 13), make_flight(5002, 2, 1),
                make_flight(5003, 6, 13), pkg};
    return ws;
}

// A world with a squadron ledger source: team 2 pool 12, squadron 4281
// (seed history zero), the airbase objective with two features.
WorldState make_sink_world() {
    auto ws = make_flight_world();
    // One objective with a feature set + pristine damage bitmap (the
    // shape populate_world produces from a decoded save).
    f4::entities::FeatureEntryState f1;
    f1.index = 1;
    f1.value = 10;
    f4::entities::FeatureEntryState f2;
    f2.index = 2;
    f2.value = 20;
    ws.objectives[0].features = {f1, f2};
    ws.objectives[0].fstatus = {0x00, 0x00};
    return ws;
}

} // namespace

// ── 1. Origin stamping ──────────────────────────────────────────────────────

TEST(CampaignOrigin, SpawnStampsCampaignIdentity) {
    f4::data::AircraftConfig cfg;
    if (!loadF16Config(cfg)) GTEST_SKIP() << "F-16 config fixture missing";

    auto ws = make_flight_world();
    EntityWorld ew;
    auto pw = f4::world::populate_world(ew, ws);

    ScenarioAirfield airfield;
    airfield.runway_heading_rad = 0.0;
    airfield.threshold_position = f4::geo::WorldPosition(0.0, 5000.0, 50.0);
    ScenarioAircraft tpl;
    tpl.callsign = "CAMPAIGN";
    tpl.vis_type_index = 1052;

    auto spawned = spawn_aircraft_from_flights(
        ew, f4::world_convert::ClassTable{}, f4::models::ModelDatabase{},
        cfg, airfield, tpl);
    ASSERT_EQ(spawned.size(), 3u);

    // Every spawned aircraft carries the origin, resolved from the
    // entities' own VU residue — flight 5001 belongs to squadron 4281,
    // owner slot 2 (ROK), callsign bytes from the wire.
    for (const auto eid : spawned) {
        EntityHandle h(eid, &ew);
        const auto* origin = h.get<CampaignOriginComponent>();
        ASSERT_NE(origin, nullptr);
        EXPECT_NE(origin->flight_vu, 0u);
        EXPECT_EQ(origin->squadron_vu, 4281u);
        EXPECT_EQ(origin->callsign_id, 125);
        EXPECT_TRUE(origin->team_slot == 2 || origin->team_slot == 6)
            << "team slot must be the CAMPAIGN owner, not the sim string";
    }

    // Per-flight specificity: the DPRK flight's aircraft carries the
    // DPRK slot.
    const auto* origin3 =
        EntityHandle(spawned[2], &ew).get<CampaignOriginComponent>();
    ASSERT_NE(origin3, nullptr);
    EXPECT_EQ(origin3->flight_vu, 5003u);
    EXPECT_EQ(origin3->team_slot, 6);
}

// ── 2. Kill classification (manual events) ─────────────────────────────────

TEST(ResultSink, ClassifiesKillsByCampaignOrigin) {
    auto ws = make_sink_world();
    EntityWorld ew;
    auto pw = f4::world::populate_world(ew, ws);
    WorldStateAdapters adapters(ws);
    CampaignResultLedger ledger(adapters.campaign, adapters.teams,
                                adapters.units);
    CampaignResultSink sink(ledger, ew);

    // Two origin-ful aircraft (blue flight 5001, red flight 5003) and
    // one origin-less scenario entity.
    auto blue = ew.create();
    blue.add<TransformComponent>();
    auto& blue_origin = blue.add<CampaignOriginComponent>();
    blue_origin.flight_vu = 5001;
    blue_origin.squadron_vu = 4281;
    blue_origin.team_slot = 2;
    auto red = ew.create();
    red.add<TransformComponent>();
    auto& red_origin = red.add<CampaignOriginComponent>();
    red_origin.flight_vu = 5003;
    red_origin.squadron_vu = 4281;
    red_origin.team_slot = 6;
    auto civilian = ew.create();
    civilian.add<TransformComponent>();

    // Air kill: origin-ful victim + origin-ful shooter.
    f4::weapons::EntityKilledMessage kill;
    kill.sim_time_s = 10.0;
    kill.target_id = red.id().value;
    kill.shooter_id = blue.id().value;
    sink.handle_kill(kill);

    // AG kill: origin-less victim, origin-ful shooter.
    kill.target_id = civilian.id().value;
    kill.sim_time_s = 20.0;
    sink.handle_kill(kill);

    // Unclassified: neither side resolves.
    kill.target_id = civilian.id().value;
    kill.shooter_id = 0;
    kill.sim_time_s = 30.0;
    sink.handle_kill(kill);

    // Ledger bookings: one air loss (DPRK pool 12 -> 11, squadron loss),
    // one aa credit for the shooter's squadron, one ag credit, one
    // unclassified (counted only).
    EXPECT_EQ(ledger.air_losses(), 1);
    EXPECT_EQ(ledger.team_aircraft_remaining(6), 11);
    EXPECT_EQ(ledger.team_aircraft_remaining(2), 12);   // untouched
    EXPECT_EQ(ledger.squadron_run_losses(4281), 1);
    EXPECT_EQ(ledger.squadron(4281)->run_aa_kills, 1);
    EXPECT_EQ(ledger.squadron(4281)->run_ag_kills, 1);

    const auto& stats = sink.stats();
    EXPECT_EQ(stats.kills_seen, 3);
    EXPECT_EQ(stats.air_losses_recorded, 1);
    EXPECT_EQ(stats.kills_attributed, 1);
    EXPECT_EQ(stats.ag_kills_recorded, 1);
    EXPECT_EQ(stats.kills_unclassified, 1);
}

// ── 3. The E2E missile kill (the chain a BVR fight produces) ────────────────

namespace {

struct MissileRig {
    EntityWorld world;
    MessageBus bus;
    std::unique_ptr<CampaignResultLedger> ledger;
    std::unique_ptr<CampaignResultSink> sink;
    EntityId shooter;
    EntityId bandit;
    f4::weapons::WeaponClassTable table =
        f4::weapons::WeaponClassTable::with_builtins();
    std::vector<f4::weapons::EntityKilledMessage> killed;

    explicit MissileRig(WorldState& ws) {
        auto pw = f4::world::populate_world(world, ws);
        (void)pw;
        WorldStateAdapters adapters(ws);
        // NOTE: adapters borrow ws; the ledger snapshots immediately.
        ledger = std::make_unique<CampaignResultLedger>(
            adapters.campaign, adapters.teams, adapters.units);
        sink = std::make_unique<CampaignResultSink>(*ledger, world);
        sink->attach(bus);
        bus.subscribe<f4::weapons::EntityKilledMessage>(
            [this](const f4::weapons::EntityKilledMessage& m) {
                killed.push_back(m);
            });

        shooter = world.create().id();
        {
            auto h = EntityHandle(shooter, &world);
            auto& tf = h.add<TransformComponent>();
            tf.position = f4::geo::WorldPosition(0.0, 0.0, 20000.0);
            tf.vy = 1200.0;   // the launch velocity the missile inherits
            tf.qw = 1.0;
            h.add<f4::weapons::WeaponStoreComponent>(
                f4::weapons::WeaponStoreComponent::standard_fighter(table));
            auto& dsc = h.add<DamageStateComponent>();
            dsc.hit_points = 25.0;
            dsc.max_hit_points = 25.0;
            auto& origin = h.add<CampaignOriginComponent>();
            origin.flight_vu = 5001;
            origin.squadron_vu = 4281;
            origin.team_slot = 2;
        }
        bandit = world.create().id();
        {
            auto h = EntityHandle(bandit, &world);
            auto& tf = h.add<TransformComponent>();
            // 10 NM dead ahead (+y — the shooter flies +y at 1200 fps),
            // crossing at 500 fps: the engagement test's geometry,
            // rotated to a stern chase (seeker aligned at launch).
            tf.position =
                f4::geo::WorldPosition(0.0, 10.0 * 6076.11548, 20000.0);
            tf.vy = 500.0;
            tf.qw = 1.0;
            auto& dsc = h.add<DamageStateComponent>();
            dsc.hit_points = 25.0;
            dsc.max_hit_points = 25.0;
            auto& origin = h.add<CampaignOriginComponent>();
            origin.flight_vu = 5003;
            origin.squadron_vu = 4281;
            origin.team_slot = 6;
        }
    }
    ~MissileRig() { sink->detach(bus); }

    /// One flyout step: scripted target motion + the sim-time stamp
    /// (Simulation::tick's order: stamp BEFORE update_all, so the
    /// missile components read the current tick's time) + component
    /// updates + the deferred-bus drain.
    void step(int tick) {
        auto* ttc = EntityHandle(bandit, &world).get<TransformComponent>();
        ttc->position.x += ttc->vx * kDt;
        ttc->position.y += ttc->vy * kDt;
        ttc->position.z += ttc->vz * kDt;
        f4::weapons::MissileSimComponent::set_sim_time(
            static_cast<double>(tick + 1) * kDt);
        world.update_all(kDt, bus);
        bus.flush_pending();
        f4::weapons::sweep_spent_missiles(world);
    }
};

} // namespace

TEST(ResultSink, MissileKillLandsInTheLedger) {
    auto ws_a = make_sink_world();
    MissileRig rig(ws_a);
    const auto amraam = rig.table.find_by_name("AIM-120C");
    ASSERT_NE(amraam, f4::weapons::kInvalidWeapon);
    f4::weapons::MissileSimComponent::set_sim_time(0.0);
    const auto missile = f4::weapons::launch_missile(
        rig.world, rig.bus, EntityHandle(rig.shooter, &rig.world),
        rig.bandit, rig.table, amraam, 0.0);
    ASSERT_TRUE(missile.valid()) << "launch refused (store/dry?)";

    // Flyout + kill: the missile's MissileSimComponent (priority 40)
    // runs inside update_all; the fuze -> apply_damage ->
    // EntityKilledMessage path flows through the SINK on the same bus.
    int kill_tick = -1;
    for (int i = 0; i < static_cast<int>(90.0 / kDt); ++i) {
        rig.step(i);
        if (!rig.killed.empty()) { kill_tick = i; break; }
    }
    f4::weapons::MissileSimComponent::set_sim_time(0.0);
    ASSERT_NE(kill_tick, -1) << "missile never killed the bandit";
    ASSERT_EQ(rig.killed.size(), 1u);
    EXPECT_EQ(rig.killed[0].target_id, rig.bandit.value);

    // The sink saw it, classified it, and the ledger booked it: DPRK
    // pool 12 -> 11, the victim's squadron took the loss, the shooter's
    // squadron got the aa credit.
    EXPECT_EQ(rig.ledger->air_losses(), 1);
    EXPECT_EQ(rig.ledger->team_aircraft_remaining(6), 11);
    EXPECT_EQ(rig.ledger->team_aircraft_remaining(2), 12);
    EXPECT_EQ(rig.ledger->squadron_run_losses(4281), 1);
    EXPECT_EQ(rig.ledger->squadron(4281)->run_aa_kills, 1);
    EXPECT_EQ(rig.sink->stats().kills_seen, 1);
    EXPECT_EQ(rig.sink->stats().air_losses_recorded, 1);
    EXPECT_EQ(rig.sink->stats().kills_attributed, 1);
}

TEST(ResultSink, MissileKillIsDeterministic) {
    // Two identical runs produce byte-identical result documents —
    // the C1 artifact is QC-able the same way the trace is.
    auto run = []() {
        auto ws = make_sink_world();
        MissileRig rig(ws);
        const auto amraam = rig.table.find_by_name("AIM-120C");
        f4::weapons::MissileSimComponent::set_sim_time(0.0);
        (void)f4::weapons::launch_missile(
            rig.world, rig.bus, EntityHandle(rig.shooter, &rig.world),
            rig.bandit, rig.table, amraam, 0.0);
        for (int i = 0; i < static_cast<int>(90.0 / kDt); ++i) {
            rig.step(i);
            if (!rig.killed.empty()) break;
        }
        f4::weapons::MissileSimComponent::set_sim_time(0.0);
        return rig.ledger->to_json();
    };
    EXPECT_EQ(run(), run());
}

// ── 4. Objective damage: impact + final-state sync + round-trip ─────────────

TEST(ResultSink, BombImpactAndObjectiveDamageSync) {
    auto ws = make_sink_world();
    EntityWorld ew;
    auto pw = f4::world::populate_world(ew, ws);
    WorldStateAdapters adapters(ws);
    CampaignResultLedger ledger(adapters.campaign, adapters.teams,
                                adapters.units);
    CampaignResultSink sink(ledger, ew);

    // The objective entity the bombs are aimed at (VU 4101).
    const auto obj_entity = pw.objective_id_map.at(4101);
    EntityHandle obj_h(obj_entity, &ew);
    ASSERT_NE(obj_h.get<FeatureSetComponent>(), nullptr);
    ASSERT_NE(obj_h.get<DamageBitmapComponent>(), nullptr);
    ASSERT_EQ(obj_h.get<FeatureSetComponent>()->features.size(), 2u);

    // One bomb impacted (the event the log wants)...
    f4::weapons::BombImpactMessage impact;
    impact.sim_time_s = 45.5;
    impact.target_id = obj_entity.value;
    impact.miss_distance_ft = 142.4;
    impact.features_destroyed = 1;
    impact.destroyed_pct = 33.3;
    sink.handle_bomb_impact(impact);
    ASSERT_EQ(ledger.bomb_impacts(), 1);
    EXPECT_EQ(ledger.bomb_impact_log()[0].objective, 4101u);
    EXPECT_EQ(ledger.bomb_impact_log()[0].miss_distance_ft, 142);

    // ...and the post-blast state f4-weapons leaves behind (its own
    // tests cover producing it): feature 1 destroyed, the bitmap
    // updated. 0x0C = feature 0 normal (00), feature 1 destroyed (11).
    {
        auto* fs = obj_h.get<FeatureSetComponent>();
        fs->features[1].damage_state = 3;   // kVisDestroyed
        auto* db = obj_h.get<DamageBitmapComponent>();
        db->fstatus = {0x0C, 0x00};
    }

    // Pristine objectives are not synced: the sync walks EVERY feature
    // objective, but only CHANGED ones become records.
    sink.sync_objective_damage();
    ASSERT_EQ(ledger.objective_damage().size(), 1u);
    const auto& rec = ledger.objective_damage()[0];
    EXPECT_EQ(rec.objective, 4101u);
    EXPECT_EQ(rec.features_total, 2);
    EXPECT_EQ(rec.features_destroyed, 1);
    // Weighted: 20 destroyed of 30 total value = 67%.
    EXPECT_EQ(rec.destroyed_pct, 67);
    EXPECT_EQ(rec.fstatus, kDamagedFstatus);
    EXPECT_EQ(sink.stats().objectives_synced, 1);

    // Repeated sync is a no-op (the snapshot never moves; the ledger
    // is last-write-wins).
    sink.sync_objective_damage();
    EXPECT_EQ(ledger.objective_damage().size(), 1u);
    EXPECT_EQ(ledger.features_destroyed(), 1);

    // The write-back + repopulate round-trip: apply_to the WorldState,
    // populate a FRESH EntityWorld, and the damage is there — the
    // save-format face round-trips, which is the C1 acceptance.
    const auto out = f4::campaign::apply_to(ledger, ws);
    ASSERT_EQ(out.objectives_written, 1);
    ASSERT_EQ(out.unmatched_objectives.size(), 0u);
    EXPECT_EQ(ws.objectives[0].fstatus, kDamagedFstatus);

    EntityWorld reloaded;
    f4::world::populate_world(reloaded, ws);
    const auto it = [&]() {
        for (const auto id : reloaded.with_component<DamageBitmapComponent>()) {
            EntityHandle h(id, &reloaded);
            const auto* pb = h.get<PropertyBag>();
            if (pb) {
                const auto vu = pb->ints.find("vu_id_num");
                if (vu != pb->ints.end() && vu->second == 4101) return id;
            }
        }
        return EntityId{};
    }();
    ASSERT_TRUE(it.valid()) << "objective 4101 not repopulated";
    const auto* reloaded_db =
        EntityHandle(it, &reloaded).get<DamageBitmapComponent>();
    ASSERT_NE(reloaded_db, nullptr);
    EXPECT_EQ(reloaded_db->fstatus, kDamagedFstatus);
}

TEST(ResultSink, PristineObjectivesSyncNothing) {
    // A mid-campaign save's PRE-EXISTING damage is initial state, not
    // this run's results: build a world whose objective ALREADY carries
    // damage, snapshot it, change nothing, sync — no records.
    auto ws = make_sink_world();
    ws.objectives[0].fstatus = {0xCC, 0x00};   // feature 1 damaged already
    EntityWorld ew;
    auto pw = f4::world::populate_world(ew, ws);
    (void)pw;
    WorldStateAdapters adapters(ws);
    CampaignResultLedger ledger(adapters.campaign, adapters.teams,
                                adapters.units);
    CampaignResultSink sink(ledger, ew);

    sink.sync_objective_damage();
    EXPECT_TRUE(ledger.objective_damage().empty());
    EXPECT_TRUE(ledger.empty());
    EXPECT_EQ(sink.stats().objectives_synced, 0);
}
