// test_damage.cpp — the warhead-vs-strength damage contract.
//
// Covers: blast falloff shape, deterministic application at nominal roll,
// variance bounds, monotonicity in power and range, kill transitions,
// degenerate inputs (already-dead target, zero lethal radius).

#include <f4/weapons/damage.hpp>

#include <gtest/gtest.h>

using namespace f4::weapons;

namespace {
constexpr double kEps = 1e-9;
}

TEST(BlastFalloff, FullAtZeroRange) {
    EXPECT_DOUBLE_EQ(blast_falloff(0.0, 100.0), 1.0);
    EXPECT_DOUBLE_EQ(blast_falloff(-5.0, 100.0), 1.0);
}

TEST(BlastFalloff, ZeroAtAndBeyondLethalRadius) {
    EXPECT_DOUBLE_EQ(blast_falloff(100.0, 100.0), 0.0);
    EXPECT_DOUBLE_EQ(blast_falloff(150.0, 100.0), 0.0);
}

TEST(BlastFalloff, MonotoneDecreasing) {
    double prev = 1.0;
    for (double r = 0.0; r <= 100.0; r += 5.0) {
        const double f = blast_falloff(r, 100.0);
        EXPECT_LE(f, prev) << "range " << r;
        prev = f;
    }
}

TEST(BlastFalloff, DegenerateWeaponHasNoRadius) {
    EXPECT_DOUBLE_EQ(blast_falloff(0.0, 0.0), 0.0);
}

TEST(DamageTest, NominalHitAtFuzeEqualsPower) {
    // roll 0.5 -> spread = 1.0; range 0 -> falloff 1.
    const auto out = apply_damage(100.0, 100.0, 48.0, 0.0, 55.0, 0.5);
    EXPECT_NEAR(out.damage_applied, 48.0, kEps);
    EXPECT_NEAR(out.hit_points_after, 52.0, kEps);
    EXPECT_FALSE(out.killed);
}

TEST(DamageTest, DeterministicForSameRoll) {
    const auto a = apply_damage(100.0, 100.0, 48.0, 20.0, 55.0, 0.3);
    const auto b = apply_damage(100.0, 100.0, 48.0, 20.0, 55.0, 0.3);
    EXPECT_EQ(a.damage_applied, b.damage_applied);
    EXPECT_EQ(a.killed, b.killed);
}

TEST(DamageTest, VarianceBoundsRespected) {
    // spread in [0.75, 1.25]: damage at fuze within those bounds.
    const auto lo = apply_damage(1e6, 1e6, 48.0, 0.0, 55.0, 0.0);
    const auto hi = apply_damage(1e6, 1e6, 48.0, 0.0, 55.0, 1.0);
    EXPECT_NEAR(lo.damage_applied, 48.0 * 0.75, kEps);
    EXPECT_NEAR(hi.damage_applied, 48.0 * 1.25, kEps);
}

TEST(DamageTest, MonotoneInWarheadPower) {
    const auto small = apply_damage(100.0, 100.0, 10.0, 0.0, 55.0, 0.5);
    const auto big   = apply_damage(100.0, 100.0, 80.0, 0.0, 55.0, 0.5);
    EXPECT_LT(small.damage_applied, big.damage_applied);
}

TEST(DamageTest, MonotoneInRange) {
    const auto close = apply_damage(100.0, 100.0, 48.0, 5.0, 55.0, 0.5);
    const auto far   = apply_damage(100.0, 100.0, 48.0, 50.0, 55.0, 0.5);
    EXPECT_GT(close.damage_applied, far.damage_applied);
}

TEST(DamageTest, ZeroDamageBeyondLethalRadius) {
    const auto out = apply_damage(100.0, 100.0, 48.0, 60.0, 55.0, 0.5);
    EXPECT_DOUBLE_EQ(out.damage_applied, 0.0);
    EXPECT_FALSE(out.killed);
}

TEST(DamageTest, KillWhenDamageExceedsHitPoints) {
    const auto out = apply_damage(10.0, 10.0, 48.0, 0.0, 55.0, 0.5);
    EXPECT_TRUE(out.killed);
    EXPECT_DOUBLE_EQ(out.hit_points_after, 0.0);
    EXPECT_NEAR(out.damage_applied, 48.0, kEps);  // full damage still recorded
}

TEST(DamageTest, ExactlyZeroDamageDoesNotKillLiveTarget) {
    const auto out = apply_damage(10.0, 10.0, 48.0, 55.0, 55.0, 0.5);
    EXPECT_FALSE(out.killed);
    EXPECT_DOUBLE_EQ(out.damage_applied, 0.0);
}

TEST(DamageTest, AlreadyDeadTargetCannotBeKilledAgain) {
    const auto out = apply_damage(0.0, 10.0, 48.0, 0.0, 55.0, 0.5);
    EXPECT_TRUE(out.killed);      // still reports dead (idempotent state)
    EXPECT_DOUBLE_EQ(out.damage_applied, 0.0);  // but takes no further damage
    EXPECT_DOUBLE_EQ(out.hit_points_after, 0.0);
}

TEST(DamageTest, HitPointsClampAtZero) {
    const auto out = apply_damage(5.0, 10.0, 1e6, 0.0, 55.0, 0.5);
    EXPECT_GE(out.hit_points_after, 0.0);
    EXPECT_TRUE(out.killed);
}
