// test_air_roster.cpp — the AGG-2b air-picture roster's maintenance rule
// (Docs/AGGREGATE_CLOCK_PLAN.md §4, Docs/FID_OPT_PLAN.md §5): priming,
// the epoch's structural flips, the cadence's behavioral flips, the
// entity-index order contract, the radius surface, and the move-op
// self-healing. See air_roster.hpp for the contract these pin.

#include <f4/entities/air_roster.hpp>
#include <f4/entities/entity.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

using namespace f4::entities;

namespace {

bool is_clutter(const EntityHandle& h) {
    const auto* tf = h.get<TransformComponent>();
    return tf != nullptr && tf->is_ground_clutter();
}

/// An airborne (non-clutter) entity: moving, at altitude.
EntityHandle spawn_airborne(EntityWorld& w, double x = 0.0, double y = 0.0,
                            double z = 20000.0) {
    auto e = w.create();
    auto& tf = e.add<TransformComponent>();
    tf.position = f4::geo::WorldPosition{x, y, z};
    tf.vy = 400.0;  // moving — never clutter
    return e;
}

/// A parked (clutter) entity: stationary, below every terrain post.
EntityHandle spawn_parked(EntityWorld& w, double x = 0.0, double y = 0.0,
                          double z = 0.0) {
    auto e = w.create();
    e.add<TransformComponent>().position =
        f4::geo::WorldPosition{x, y, z};
    return e;
}

std::vector<std::uint64_t> ids(const std::vector<EntityId>& v) {
    std::vector<std::uint64_t> out;
    for (const auto id : v) out.push_back(id.value);
    return out;
}

} // namespace

TEST(AirRoster, PrimesOnFirstRefreshWithTheNonClutterMembership) {
    EntityWorld w;
    auto parked1 = spawn_parked(w, 1000.0, 0.0);
    auto air1 = spawn_airborne(w, 0.0, 5000.0);
    auto parked2 = spawn_parked(w, -3000.0, 2000.0);
    auto air2 = spawn_airborne(w, 9000.0, -1000.0);

    w.refresh_air_roster(10.0, 1.0);

    const auto& roster = w.air_roster();
    ASSERT_EQ(roster.size(), 2u);
    EXPECT_EQ(roster[0].value, air1.id().value);
    EXPECT_EQ(roster[1].value, air2.id().value);
    EXPECT_TRUE(w.air_roster_state()->primed());
    EXPECT_EQ(w.air_roster_state()->rebuild_count(), 1u);
    (void)parked1;
    (void)parked2;
}

TEST(AirRoster, StationaryHighStaysAMember) {
    // The clutter rule's documented edge: stationary AT ALTITUDE (a
    // hovering test rig, a future tanker anchor) is air picture, not
    // clutter.
    EntityWorld w;
    spawn_airborne(w);                      // moving control
    auto hovering = spawn_airborne(w, 100.0, 100.0);
    hovering.get<TransformComponent>()->vx = 0.0;
    hovering.get<TransformComponent>()->vy = 0.0;
    hovering.get<TransformComponent>()->vz = 0.0;

    w.refresh_air_roster(0.0, 1.0);
    ASSERT_EQ(w.air_roster().size(), 2u);
}

TEST(AirRoster, EpochChangeRebuildsImmediately) {
    EntityWorld w;
    spawn_airborne(w);
    w.refresh_air_roster(0.0, 1.0);
    ASSERT_EQ(w.air_roster_state()->rebuild_count(), 1u);

    // A structural flip (a new airborne spawn) rebuilds on the very
    // next refresh — no cadence wait, the epoch compare forces it.
    auto late = spawn_airborne(w, 500.0, 0.0);
    w.refresh_air_roster(0.1, 1.0);  // 0.1 s into a 1 s cadence window
    ASSERT_EQ(w.air_roster().size(), 2u);
    EXPECT_EQ(w.air_roster().back().value, late.id().value);
    EXPECT_EQ(w.air_roster_state()->rebuild_count(), 2u);
    (void)late;
}

TEST(AirRoster, DestroyDropsThroughTheEpoch) {
    EntityWorld w;
    auto air = spawn_airborne(w);
    spawn_airborne(w, 500.0, 0.0);
    w.refresh_air_roster(0.0, 1.0);
    ASSERT_EQ(w.air_roster().size(), 2u);

    w.destroy(air.id());
    w.refresh_air_roster(0.2, 1.0);
    EXPECT_EQ(w.air_roster().size(), 1u);
}

TEST(AirRoster, BehavioralFlipWaitsForTheCadence) {
    // THE AGG-2b latency pin: a parked entity that starts moving flips
    // NO structural event — it joins the roster at the caller's
    // revalidation cadence, not before, and not never.
    EntityWorld w;
    spawn_airborne(w);
    auto taxi = spawn_parked(w, 2000.0, 0.0);
    w.refresh_air_roster(0.0, 1.0);
    ASSERT_EQ(w.air_roster().size(), 1u);

    // The taxi launch: velocity appears mid-window. Refreshes inside
    // the window keep the pre-flip membership (the cheap path).
    taxi.get<TransformComponent>()->vy = 150.0;
    w.refresh_air_roster(0.5, 1.0);
    EXPECT_EQ(w.air_roster().size(), 1u);
    EXPECT_EQ(w.air_roster_state()->rebuild_count(), 1u);

    // The window elapses: the revalidation catches the flip.
    w.refresh_air_roster(1.0, 1.0);
    ASSERT_EQ(w.air_roster().size(), 2u);
    EXPECT_EQ(w.air_roster().back().value, taxi.id().value);
}

TEST(AirRoster, LandingFlipDropsAtTheCadenceToo) {
    EntityWorld w;
    auto air = spawn_airborne(w);
    w.refresh_air_roster(0.0, 1.0);
    ASSERT_EQ(w.air_roster().size(), 1u);

    // The landing stop: the member went clutter. The roster keeps it
    // until revalidation — the CONSUMERS' fresh gates (the radar scan's,
    // the picture walk's) are what drop it immediately; the membership
    // is the bounded-latency surface.
    auto& tf = *air.get<TransformComponent>();
    tf.vy = 0.0;
    tf.position.z = 0.0;
    w.refresh_air_roster(0.4, 1.0);
    EXPECT_EQ(w.air_roster().size(), 1u);  // still cached

    w.refresh_air_roster(1.0, 1.0);
    EXPECT_EQ(w.air_roster().size(), 0u);
}

TEST(AirRoster, ZeroIntervalRevalidatesEveryCall) {
    EntityWorld w;
    auto taxi = spawn_parked(w, 100.0, 100.0);
    w.refresh_air_roster(0.0, 0.0);
    ASSERT_EQ(w.air_roster().size(), 0u);

    // The exact-control knob: interval <= 0 rebuilds every call — a
    // behavioral flip shows up on the very next refresh.
    taxi.get<TransformComponent>()->vy = 150.0;
    w.refresh_air_roster(0.1, 0.0);
    ASSERT_EQ(w.air_roster().size(), 1u);
    EXPECT_EQ(w.air_roster().front().value, taxi.id().value);
}

TEST(AirRoster, HugeIntervalNeverRevalidatesBehaviorally) {
    EntityWorld w;
    spawn_airborne(w);
    auto taxi = spawn_parked(w, 100.0, 100.0);
    w.refresh_air_roster(0.0, 1e12);
    ASSERT_EQ(w.air_roster().size(), 1u);

    taxi.get<TransformComponent>()->vy = 200.0;
    w.refresh_air_roster(1e6, 1e12);  // behavioral flips never caught
    EXPECT_EQ(w.air_roster().size(), 1u);

    // ...but a STRUCTURAL flip still lands: the epoch outranks the
    // cadence by contract. And the forced rebuild re-walks the FULL
    // predicate fresh — the taxi's behavioral flip rides along with it
    // (the roster holds exactly the non-clutter population, as of the
    // rebuild's own walk).
    spawn_airborne(w, 7.0, 7.0);
    w.refresh_air_roster(1e6 + 0.1, 1e12);
    ASSERT_EQ(w.air_roster().size(), 3u);  // control + flipped taxi + new
    const auto got = ids(w.air_roster());
    EXPECT_NE(std::find(got.begin(), got.end(), taxi.id().value),
              got.end());
}

TEST(AirRoster, MembersAreInEntityIndexOrder) {
    EntityWorld w;
    std::vector<EntityHandle> airborne;
    for (int i = 0; i < 8; ++i) {
        // Alternate parked/airborne; the roster collects only the
        // airborne, in the bucket's ascending slot order.
        if (i % 2 == 0) {
            spawn_parked(w, 100.0 * i, 0.0);
        } else {
            airborne.push_back(spawn_airborne(w, 100.0 * i, 50.0 * i));
        }
    }
    w.refresh_air_roster(0.0, 1.0);

    const auto got = ids(w.air_roster());
    std::vector<std::uint64_t> want;
    for (const auto& e : airborne) want.push_back(e.id().value);
    EXPECT_EQ(got, want);  // ascending slot order, parked interleaved out
}

TEST(AirRoster, RadiusQueryAnswersOverMemberPositions) {
    EntityWorld w;
    spawn_parked(w, 0.0, 0.0);                       // never queried
    auto near = spawn_airborne(w, 10.0 * 6076.11548, 0.0);  // ~10 NM east
    auto far = spawn_airborne(w, 200.0 * 6076.11548, 0.0);  // 200 NM east
    w.refresh_air_roster(0.0, 1.0);

    const auto ball = w.air_roster_within_radius(0.0, 0.0, 0.0,
                                                 50.0 * 6076.11548);
    const auto got = ids(ball);
    EXPECT_EQ(got, std::vector<std::uint64_t>{near.id().value});
    EXPECT_EQ(std::find(got.begin(), got.end(), far.id().value),
              got.end());
    (void)near;
    (void)far;
}

TEST(AirRoster, MoveOpsSelfHeal) {
    EntityWorld w;
    spawn_airborne(w);
    w.refresh_air_roster(0.0, 1.0);
    ASSERT_EQ(w.air_roster().size(), 1u);

    EntityWorld w2(std::move(w));
    // The destination's roster arrives empty (the move ops construct it
    // without transferring) — the first refresh builds it fresh.
    EXPECT_EQ(w2.air_roster_state(), nullptr);
    w2.refresh_air_roster(0.0, 1.0);
    EXPECT_EQ(w2.air_roster().size(), 1u);
}
