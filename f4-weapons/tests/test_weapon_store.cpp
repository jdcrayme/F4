// test_weapon_store.cpp — station/round bookkeeping: loadout construction,
// selection, expenditure, category queries, dry-station refusals.

#include <f4/weapons/weapon_store.hpp>

#include <f4/weapons/weapon_class_table.hpp>

#include <gtest/gtest.h>

using namespace f4::weapons;

TEST(WeaponStore, EmptyStoreHasNothing) {
    WeaponStoreComponent store;
    EXPECT_EQ(store.station_count(), 0u);
    EXPECT_EQ(store.station(0), nullptr);
    EXPECT_FALSE(store.can_fire(0));
    EXPECT_EQ(store.select_next_loaded(), WeaponStoreComponent::npos);
    EXPECT_EQ(store.find_with_category(WeaponClassTable::with_builtins(),
                                       WeaponCategory::AirToAirMissile),
              WeaponStoreComponent::npos);
}

TEST(WeaponStore, AddStationAndCountRounds) {
    WeaponClassTable table = WeaponClassTable::with_builtins();
    const auto amraam = table.find_by_name("AIM-120C");

    WeaponStoreComponent store;
    store.add_station(amraam, 2, "station 1");
    store.add_station(amraam, 2, "station 2");
    EXPECT_EQ(store.station_count(), 2u);
    EXPECT_EQ(store.count_for(amraam), 4);

    ASSERT_NE(store.station(0), nullptr);
    EXPECT_EQ(store.station(0)->weapon_handle, amraam);
    EXPECT_EQ(store.station(0)->rounds, 2);
    EXPECT_EQ(store.station(0)->initial_rounds, 2);
    EXPECT_EQ(store.station(0)->label, "station 1");
}

TEST(WeaponStore, ExpendNeverGoesNegative) {
    WeaponClassTable table = WeaponClassTable::with_builtins();
    const auto nine = table.find_by_name("AIM-9M");

    WeaponStoreComponent store;
    store.add_station(nine, 1, "wingtip");
    EXPECT_EQ(store.expend(0, 1), 1);
    EXPECT_EQ(store.station(0)->rounds, 0);
    EXPECT_EQ(store.expend(0, 1), 0);              // dry
    EXPECT_EQ(store.station(0)->rounds, 0);
    EXPECT_EQ(store.expend(99, 1), 0);             // bad index
    EXPECT_EQ(store.expend(0, -5), 0);             // nonsense count
}

TEST(WeaponStore, CanFireRequiresWeaponAndRounds) {
    WeaponClassTable table = WeaponClassTable::with_builtins();
    const auto nine = table.find_by_name("AIM-9M");

    WeaponStoreComponent store;
    store.add_station(kInvalidWeapon, 2, "broken");  // no weapon bound
    store.add_station(nine, 0, "empty");
    store.add_station(nine, 1, "loaded");

    EXPECT_FALSE(store.can_fire(0));
    EXPECT_FALSE(store.can_fire(1));
    EXPECT_TRUE(store.can_fire(2));
    EXPECT_FALSE(store.can_fire(3));
}

TEST(WeaponStore, SelectionClampsAndCycles) {
    WeaponClassTable table = WeaponClassTable::with_builtins();
    const auto nine = table.find_by_name("AIM-9M");
    const auto amraam = table.find_by_name("AIM-120C");

    WeaponStoreComponent store;
    store.add_station(amraam, 1, "a");
    store.add_station(amraam, 0, "b");   // empty slot in the middle
    store.add_station(nine, 1, "c");

    EXPECT_EQ(store.select(99), 2u);                       // clamped
    EXPECT_EQ(store.select_next_loaded(), 0u);             // wraps 2 -> 0 (b is dry)
    EXPECT_EQ(store.expend(0, 1), 1);
    EXPECT_EQ(store.select_next_loaded(), 2u);             // skips the now-dry 0
    EXPECT_EQ(store.expend(2, 1), 1);
    EXPECT_EQ(store.select_next_loaded(), WeaponStoreComponent::npos);  // all dry
}

TEST(WeaponStore, FindWithCategorySkipsDryStations) {
    WeaponClassTable table = WeaponClassTable::with_builtins();
    const auto nine = table.find_by_name("AIM-9M");

    WeaponStoreComponent store;
    store.add_station(nine, 0, "empty first");
    store.add_station(nine, 1, "loaded second");
    EXPECT_EQ(store.find_with_category(table, WeaponCategory::AirToAirMissile), 1u);
    EXPECT_EQ(store.find_with_category(table, WeaponCategory::Bomb),
              WeaponStoreComponent::npos);
}

TEST(WeaponStore, StandardFighterLoadout) {
    WeaponClassTable table = WeaponClassTable::with_builtins();
    auto store = WeaponStoreComponent::standard_fighter(table);

    // gun + 4 AMRAAM + 2 sidewinder = 7 stations (built-in table is complete).
    EXPECT_EQ(store.station_count(), 7u);
    const auto gun = table.find_by_name("M61A1");
    const auto amraam = table.find_by_name("AIM-120C");
    const auto nine = table.find_by_name("AIM-9M");
    EXPECT_EQ(store.count_for(gun), 511);
    EXPECT_EQ(store.count_for(amraam), 8);
    EXPECT_EQ(store.count_for(nine), 2);
}

TEST(WeaponStore, StandardFighterOnPartialTableSkipsMissingClasses) {
    WeaponClassTable table;               // empty: nothing matches
    auto store = WeaponStoreComponent::standard_fighter(table);
    EXPECT_EQ(store.station_count(), 0u);
}
