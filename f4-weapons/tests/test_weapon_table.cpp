// test_weapon_table.cpp — WeaponClassTable registry: stable handles,
// lookups, the built-in placeholder set's shape (invariants the flyout and
// fire-control logic rely on, NOT specific magic numbers).

#include <f4/weapons/weapon_class_table.hpp>

#include <gtest/gtest.h>

using namespace f4::weapons;

TEST(WeaponTable, EmptyTableLookupsAreSafe) {
    WeaponClassTable table;
    EXPECT_EQ(table.size(), 0u);
    EXPECT_EQ(table.get(0), nullptr);
    EXPECT_EQ(table.get(kInvalidWeapon), nullptr);
    EXPECT_EQ(table.find_by_name("AIM-120C"), kInvalidWeapon);
    EXPECT_EQ(table.find_by_category(WeaponCategory::Gun), kInvalidWeapon);
}

TEST(WeaponTable, HandlesAreStableIndices) {
    WeaponClassTable table;
    WeaponClassRecord a;
    a.name = "TestAlpha";
    a.category = WeaponCategory::AirToAirMissile;
    WeaponClassRecord b;
    b.name = "TestBeta";
    b.category = WeaponCategory::Gun;

    const auto ha = table.add(a);
    const auto hb = table.add(b);
    EXPECT_EQ(ha, 0u);
    EXPECT_EQ(hb, 1u);
    EXPECT_NE(ha, kInvalidWeapon);

    ASSERT_NE(table.get(ha), nullptr);
    EXPECT_EQ(table.get(ha)->name, "TestAlpha");
    ASSERT_NE(table.get(hb), nullptr);
    EXPECT_EQ(table.get(hb)->name, "TestBeta");
    EXPECT_EQ(table.get(2), nullptr);
    EXPECT_EQ(table.get(kInvalidWeapon), nullptr);

    EXPECT_EQ(table.find_by_name("TestBeta"), hb);
    EXPECT_EQ(table.find_by_name("TestGamma"), kInvalidWeapon);
    EXPECT_EQ(table.find_by_category(WeaponCategory::Gun), hb);
    EXPECT_EQ(table.find_by_category(WeaponCategory::Bomb), kInvalidWeapon);
}

TEST(WeaponTable, BuiltinsRegisterTheDocumentedSet) {
    auto table = WeaponClassTable::with_builtins();
    ASSERT_EQ(table.size(), 5u);

    // Order is a documented contract: 0=M61A1, 1=AIM-9M, 2=AIM-7M,
    // 3=AIM-120C, 4=Mk-82 (see the header comment).
    ASSERT_NE(table.get(0), nullptr);
    EXPECT_STREQ(table.get(0)->name.c_str(), "M61A1");
    EXPECT_STREQ(table.get(1)->name.c_str(), "AIM-9M");
    EXPECT_STREQ(table.get(2)->name.c_str(), "AIM-7M");
    EXPECT_STREQ(table.get(3)->name.c_str(), "AIM-120C");
    EXPECT_STREQ(table.get(4)->name.c_str(), "MK-82");

    const auto gun    = table.find_by_name("M61A1");
    const auto nine   = table.find_by_name("AIM-9M");
    const auto sparrow= table.find_by_name("AIM-7M");
    const auto amraam = table.find_by_name("AIM-120C");
    const auto mk82   = table.find_by_name("MK-82");

    EXPECT_EQ(table.get(gun)->category, WeaponCategory::Gun);
    EXPECT_EQ(table.get(gun)->guidance, GuidanceKind::None);

    EXPECT_EQ(table.get(nine)->category, WeaponCategory::AirToAirMissile);
    EXPECT_EQ(table.get(nine)->guidance, GuidanceKind::Ir);
    EXPECT_EQ(table.get(sparrow)->guidance, GuidanceKind::SemiActiveRadar);
    EXPECT_EQ(table.get(amraam)->guidance, GuidanceKind::ActiveRadar);

    EXPECT_EQ(table.get(mk82)->category, WeaponCategory::Bomb);
}

TEST(WeaponTable, BuiltinsHaveFlyoutInvariants) {
    // The missile flyout divides by these; the built-ins must respect the
    // invariants regardless of exact numbers.
    auto table = WeaponClassTable::with_builtins();
    for (const auto& rec : table.records()) {
        if (rec.category != WeaponCategory::AirToAirMissile) {
            continue;
        }
        EXPECT_GT(rec.launch_mass_lb, rec.burnout_mass_lb) << rec.name;
        EXPECT_GT(rec.burnout_mass_lb, 0.0) << rec.name;
        EXPECT_GT(rec.thrust_lbf, 0.0) << rec.name;
        EXPECT_GT(rec.burn_time_s, 0.0) << rec.name;
        EXPECT_GT(rec.ref_area_ft2, 0.0) << rec.name;
        EXPECT_GT(rec.cd, 0.0) << rec.name;
        EXPECT_GT(rec.max_speed_fts, 0.0) << rec.name;
        EXPECT_GT(rec.max_g, 0.0) << rec.name;
        EXPECT_GT(rec.seeker_half_angle_deg, 0.0) << rec.name;
        // NOTE: seeker_max_range may be SHORTER than max_range for datalink
        // weapons (AIM-120C: midcourse datalink + ~15 NM terminal seeker).
        // M1 has no datalink model, so employment beyond seeker range coasts
        // ballistic — the M2 sensor work adds the datalink source.
        EXPECT_GT(rec.seeker_max_range_ft, rec.fuze_radius_ft) << rec.name;
        EXPECT_GT(rec.fuze_radius_ft, 0.0) << rec.name;
        EXPECT_GE(rec.lethal_radius_ft, rec.fuze_radius_ft) << rec.name;
        EXPECT_GT(rec.tof_limit_s, 0.0) << rec.name;
        EXPECT_GT(rec.warhead_power_lb, 0.0) << rec.name;
        EXPECT_GT(rec.max_range_ft, rec.min_range_ft) << rec.name;
    }
}
