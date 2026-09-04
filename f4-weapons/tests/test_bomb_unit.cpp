// test_bomb_unit.cpp — G2, the interdiction link's weapon-layer tests.
//
// The battalion sibling of the objective feature-damage endpoint
// (test_bomb.cpp owns the feature side; this file owns the unit side):
//
//   1. roster_vehicle_count: the wire's 2-bit x 16-group packing sums.
//   2. apply_battalion_damage: the point-blast kill model — falloff,
//      the vehicle constant, the strength cap, the destroyed flag,
//      non-battalion and absent targets.
//   3. The ECS chain (release_bomb → BombSimComponent → terminal):
//      a bomb aimed at a BATTALION entity publishes ONE
//      GroundUnitLossMessage carrying the vehicle count (when the
//      blast kills) and the usual zeroed-summary BombImpactMessage —
//      and never touches the entity's state (the engine owns it).
//   4. The objective path is unchanged: feature targets publish NO
//      unit-loss events (the regression guard for the shared terminal).

#include <f4/weapons/bomb_battery.hpp>
#include <f4/weapons/messages.hpp>
#include <f4/weapons/weapon_class_table.hpp>
#include <f4/weapons/weapon_store.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using namespace f4::weapons;

namespace entities = f4::entities;
namespace messaging = f4::messaging;

namespace {

constexpr double kDt = 1.0 / 120.0;

/// A battalion entity: the G1 world-mirror shape — Transform at the
/// grid position, UnitCore (Battalion + packed roster), PropertyBag
/// vu_id_num, TEAM tag. NO DamageState, NO FeatureSet: the aggregate
/// point target the blast endpoint keys on.
entities::EntityHandle spawn_battalion(entities::EntityWorld& world,
                                       double x, double y,
                                       std::uint32_t roster,
                                       std::uint32_t vu,
                                       std::int64_t team = 6) {
    auto h = world.create();
    auto& tc = h.add<entities::TransformComponent>();
    tc.position = f4::geo::WorldPosition{x, y, 0.0};
    auto& uc = h.add<entities::UnitCoreComponent>();
    uc.unit_class = entities::UnitClass::Battalion;
    uc.domain = 3;
    uc.roster = roster;
    uc.class_name = "Armor Battalion";
    auto& pb = h.add<entities::PropertyBag>();
    pb.ints["vu_id_num"] = static_cast<std::int64_t>(vu);
    h.set_tag(entities::tags::TEAM, entities::TagValue::from(team));
    return h;
}

/// A shooter: released directly OVERHEAD the target (5,000 ft, no
/// forward velocity — the battery test's exact solution: the impact
/// lands at the aim point, miss ~ 0).
entities::EntityHandle spawn_bomber_over(
        entities::EntityWorld& world, double x, double y,
        std::uint32_t bomb_handle, int rounds) {
    auto h = world.create();
    auto& tc = h.add<entities::TransformComponent>();
    tc.position = f4::geo::WorldPosition{x, y, 5000.0};
    tc.vx = 0.0;
    auto& store = h.add<WeaponStoreComponent>();
    store.add_station(bomb_handle, rounds, "station 3");
    h.set_tag(entities::tags::TEAM,
              entities::TagValue::from(std::string("blue")));
    return h;
}

/// Aggregate of every bomb event observed on the bus.
struct UnitEventLog {
    std::vector<BombReleasedMessage> released;
    std::vector<BombImpactMessage> impacts;
    std::vector<GroundUnitLossMessage> unit_losses;

    void attach(messaging::MessageBus& bus) {
        bus.subscribe<BombReleasedMessage>(
            [this](const BombReleasedMessage& m) { released.push_back(m); });
        bus.subscribe<BombImpactMessage>(
            [this](const BombImpactMessage& m) { impacts.push_back(m); });
        bus.subscribe<GroundUnitLossMessage>(
            [this](const GroundUnitLossMessage& m) {
                unit_losses.push_back(m);
            });
    }
};

/// Drive the world until no bomb is live (the same pump the battery
/// tests use: the sim time flows, update_all ticks BombSimComponent,
/// the sweep reaps).
void pump_to_impact(entities::EntityWorld& world,
                    messaging::MessageBus& bus, double& t) {
    for (int i = 0; i < 4000; ++i) {
        BombSimComponent::set_sim_time(t);
        world.update_all(kDt, bus);
        t += kDt;
        sweep_spent_bombs(world);
        if (count_live_bombs(world) == 0) return;
    }
}

double mk82_power() {
    const auto table = WeaponClassTable::with_builtins();
    return table.get(table.find_by_name("MK-82"))->warhead_power_lb;
}

double mk82_lethal() {
    const auto table = WeaponClassTable::with_builtins();
    return table.get(table.find_by_name("MK-82"))->lethal_radius_ft;
}

} // namespace

// ---------------------------------------------------------------------------
// roster_vehicle_count
// ---------------------------------------------------------------------------

TEST(BombUnit, RosterVehicleCountPacking) {
    EXPECT_EQ(roster_vehicle_count(0), 0);
    EXPECT_EQ(roster_vehicle_count(0xAAA), 12);   // 6 groups x 2
    EXPECT_EQ(roster_vehicle_count(0x001), 1);
    EXPECT_EQ(roster_vehicle_count(0x003), 3);
    EXPECT_EQ(roster_vehicle_count(0xFFFFFFFFu), 48);  // 16 groups x 3
}

// ---------------------------------------------------------------------------
// apply_battalion_damage — the point-blast model
// ---------------------------------------------------------------------------

TEST(BombUnit, DirectHitKillsWholeVehicles) {
    entities::EntityWorld world;
    const auto h = spawn_battalion(world, 1000.0, 0.0, 0xAAA, 4621);

    // A burst AT the unit center: power x falloff(0)=1 x spread(0.5)=1
    // => 192 lb / 96 lb-per-vehicle = 2 vehicles.
    const auto r = apply_battalion_damage(
        world, h.id().value,
        f4::geo::WorldPosition{1000.0, 0.0, 0.0},
        mk82_power(), mk82_lethal());
    ASSERT_TRUE(r.unit_found);
    EXPECT_EQ(r.battalion_vu, 4621u);
    EXPECT_EQ(r.victim_team, 6);   // the TEAM tag's int (owner slot)
    EXPECT_EQ(r.strength, 12);
    EXPECT_EQ(r.vehicles_killed, 2);
    EXPECT_FALSE(r.destroyed);
    // PURE: the entity's roster is untouched (the engine owns it).
    EXPECT_EQ(h.get<entities::UnitCoreComponent>()->roster, 0xAAAu);
}

TEST(BombUnit, MissBeyondLethalKillsNothing) {
    entities::EntityWorld world;
    const auto h = spawn_battalion(world, 1000.0, 0.0, 0xAAA, 4621);
    const auto r = apply_battalion_damage(
        world, h.id().value,
        f4::geo::WorldPosition{1000.0 + mk82_lethal() + 1.0, 0.0, 0.0},
        mk82_power(), mk82_lethal());
    ASSERT_TRUE(r.unit_found);
    EXPECT_EQ(r.vehicles_killed, 0);
}

TEST(BombUnit, BlastCappedAtStrength) {
    entities::EntityWorld world;
    // 1 vehicle left in the group 0 slot.
    const auto h = spawn_battalion(world, 100.0, 50.0, 0x001, 7);
    const auto r = apply_battalion_damage(
        world, h.id().value,
        f4::geo::WorldPosition{100.0, 50.0, 0.0},
        mk82_power(), mk82_lethal());
    ASSERT_TRUE(r.unit_found);
    EXPECT_EQ(r.strength, 1);
    EXPECT_EQ(r.vehicles_killed, 1);
    EXPECT_TRUE(r.destroyed);   // the last vehicles, taken
}

TEST(BombUnit, SpentBattalionKillsNothing) {
    entities::EntityWorld world;
    const auto h = spawn_battalion(world, 100.0, 50.0, 0, 9);
    const auto r = apply_battalion_damage(
        world, h.id().value,
        f4::geo::WorldPosition{100.0, 50.0, 0.0},
        mk82_power(), mk82_lethal());
    ASSERT_TRUE(r.unit_found);
    EXPECT_EQ(r.strength, 0);
    EXPECT_EQ(r.vehicles_killed, 0);
    EXPECT_FALSE(r.destroyed);
}

TEST(BombUnit, NonBattalionAndAbsentTargets) {
    entities::EntityWorld world;
    // An aircraft-like entity: UnitCore but not Battalion.
    auto ac = world.create();
    auto& uc = ac.add<entities::UnitCoreComponent>();
    uc.unit_class = entities::UnitClass::Flight;
    uc.roster = 0xAAA;
    const auto r_flight = apply_battalion_damage(
        world, ac.id().value, f4::geo::WorldPosition{0.0, 0.0, 0.0},
        mk82_power(), mk82_lethal());
    EXPECT_FALSE(r_flight.unit_found);

    // An entity with no UnitCore at all (an objective-shaped one).
    auto obj = world.create();
    obj.add<entities::TransformComponent>();
    const auto r_obj = apply_battalion_damage(
        world, obj.id().value, f4::geo::WorldPosition{0.0, 0.0, 0.0},
        mk82_power(), mk82_lethal());
    EXPECT_FALSE(r_obj.unit_found);

    // A garbage id.
    const auto r_none = apply_battalion_damage(
        world, 99'999ull, f4::geo::WorldPosition{0.0, 0.0, 0.0},
        mk82_power(), mk82_lethal());
    EXPECT_FALSE(r_none.unit_found);
}

// ---------------------------------------------------------------------------
// The ECS chain: a bomb aimed at a battalion
// ---------------------------------------------------------------------------

TEST(BombUnit, BombAtBattalionPublishesUnitLoss) {
    entities::EntityWorld world;
    messaging::MessageBus bus;
    UnitEventLog log;
    log.attach(bus);

    const auto table = WeaponClassTable::with_builtins();
    const auto mk82 = table.find_by_name("MK-82");

    const auto tgt = spawn_battalion(world, 10000.0, 0.0, 0xAAA, 4621);
    const auto shooter = spawn_bomber_over(world, 10000.0, 0.0, mk82, 2);
    BombSimComponent::set_sim_time(0.0);
    const auto bomb = release_bomb(world, bus, shooter, tgt.id(),
                                   table, mk82, 0.0);
    ASSERT_TRUE(bomb.valid());

    double t = 0.0;
    pump_to_impact(world, bus, t);

    // The impact still reports (objective summary zeroed — the log
    // path resolves the victim's own vu).
    ASSERT_EQ(log.impacts.size(), 1u);
    EXPECT_EQ(log.impacts[0].target_id, tgt.id().value);
    EXPECT_EQ(log.impacts[0].features_destroyed, 0);
    EXPECT_EQ(log.impacts[0].destroyed_pct, 0.0);

    // The unit-loss event: exactly one, carrying the blast's count.
    ASSERT_EQ(log.unit_losses.size(), 1u);
    EXPECT_EQ(log.unit_losses[0].target_id, tgt.id().value);
    EXPECT_EQ(log.unit_losses[0].shooter_id, shooter.id().value);
    EXPECT_GE(log.unit_losses[0].vehicles_killed, 1);
    EXPECT_LE(log.unit_losses[0].vehicles_killed, 12);

    // The entity's state is UNTOUCHED — the ledger books, the engine
    // pulls, the mirror syncs (G1's one-writer discipline).
    EXPECT_EQ(tgt.get<entities::UnitCoreComponent>()->roster, 0xAAAu);

    // The store was debited and the bomb swept.
    EXPECT_EQ(shooter.get<WeaponStoreComponent>()->count_for(mk82), 1);
    EXPECT_EQ(count_live_bombs(world), 0u);
}

TEST(BombUnit, BombAtObjectivePublishesNoUnitLoss) {
    // The regression guard: the feature path (the pre-G2 terminal)
    // never emits unit-loss events.
    entities::EntityWorld world;
    messaging::MessageBus bus;
    UnitEventLog log;
    log.attach(bus);

    const auto table = WeaponClassTable::with_builtins();
    const auto mk82 = table.find_by_name("MK-82");

    // An objective with features, at the battery tests' target spot.
    auto obj = world.create();
    auto& tc = obj.add<entities::TransformComponent>();
    tc.position = f4::geo::WorldPosition{10000.0, 0.0, 0.0};
    auto& fs = obj.add<entities::FeatureSetComponent>();
    entities::FeatureEntryState f;
    f.offset_x = 0.0;
    f.offset_y = 0.0;
    f.hit_points = 100;
    fs.features.push_back(f);
    auto& pb = obj.add<entities::PropertyBag>();
    pb.ints["vu_id_num"] = 4101;

    const auto shooter = spawn_bomber_over(world, 10000.0, 0.0, mk82, 1);
    BombSimComponent::set_sim_time(0.0);
    const auto bomb = release_bomb(world, bus, shooter, obj.id(),
                                   table, mk82, 0.0);
    ASSERT_TRUE(bomb.valid());
    double t = 0.0;
    pump_to_impact(world, bus, t);

    ASSERT_EQ(log.impacts.size(), 1u);
    EXPECT_TRUE(log.unit_losses.empty());
}
