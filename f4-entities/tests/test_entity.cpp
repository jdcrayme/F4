// test_entity.cpp — EntityId, EntityWorld lifecycle, components, tags.

#include <gtest/gtest.h>
#include <algorithm>
#include <f4/entities/f4_entities.hpp>
#include <f4/geo/f4_geo.hpp>

using namespace f4::entities;
using namespace f4::geo;

// ============================================================================
// EntityId
// ============================================================================
TEST(EntityId, DefaultIsInvalid) {
    EntityId id;
    EXPECT_FALSE(id.valid());
    EXPECT_FALSE(static_cast<bool>(id));
}

TEST(EntityId, MakePacksIndexAndGeneration) {
    EntityId id = EntityId::make(7u, 3u);
    EXPECT_EQ(id.index(), 7u);
    EXPECT_EQ(id.generation(), 3u);
    EXPECT_TRUE(id.valid());
}

TEST(EntityId, ComparisonIsValueBased) {
    EXPECT_EQ(EntityId::make(1, 2), EntityId::make(1, 2));
    EXPECT_NE(EntityId::make(1, 2), EntityId::make(1, 3));
}

// ============================================================================
// EntityWorld lifecycle
// ============================================================================
TEST(EntityWorld, CreateProducesValidHandle) {
    EntityWorld w;
    EntityHandle h = w.create();
    EXPECT_TRUE(h.valid());
    EXPECT_EQ(w.size(), 1u);
}

TEST(EntityWorld, DestroyInvalidatesHandle) {
    EntityWorld w;
    EntityHandle h = w.create();
    EntityId id = h.id();
    w.destroy(id);
    EXPECT_FALSE(h.valid());
    EXPECT_FALSE(w.alive(id));
}

TEST(EntityWorld, DestroyedSlotGenerationBumps) {
    // A stale handle captured before destroy must not resolve to the reused
    // slot after create() recycles it.
    EntityWorld w;
    EntityId first = w.create().id();
    w.destroy(first);
    EntityId second = w.create().id();
    // Same slot index, but generation differs.
    EXPECT_EQ(second.index(), first.index());
    EXPECT_NE(second.generation(), first.generation());
    EXPECT_FALSE(w.alive(first));   // stale handle stays dead
    EXPECT_TRUE(w.alive(second));
}

// ============================================================================
// Components
// ============================================================================
TEST(Components, AddGetHasRemove) {
    EntityWorld w;
    EntityHandle h = w.create();
    EXPECT_FALSE(h.has<TransformComponent>());
    auto& tf = h.add<TransformComponent>();
    tf.position = WorldPosition{100.0, 200.0, 300.0};
    EXPECT_TRUE(h.has<TransformComponent>());
    EXPECT_EQ(h.get<TransformComponent>()->position.z, 300.0);
    h.remove<TransformComponent>();
    EXPECT_FALSE(h.has<TransformComponent>());
}

TEST(Components, WithComponentFindsOnlyEntitiesThatHaveIt) {
    EntityWorld w;
    EntityHandle a = w.create();
    [[maybe_unused]] EntityHandle b = w.create();
    EntityHandle c = w.create();
    a.add<TransformComponent>();
    c.add<TransformComponent>();
    // b has none.
    auto ids = w.with_component<TransformComponent>();
    ASSERT_EQ(ids.size(), 2u);
    EXPECT_NE(std::find(ids.begin(), ids.end(), a.id()), ids.end());
    EXPECT_NE(std::find(ids.begin(), ids.end(), c.id()), ids.end());
}

TEST(Components, RequireThrowsWhenMissing) {
    EntityWorld w;
    EntityHandle h = w.create();
    // (void) cast silences [[nodiscard]] on require() inside EXPECT_THROW.
    EXPECT_THROW((void)h.require<TransformComponent>(), std::runtime_error);
}

// ============================================================================
// Tags
// ============================================================================
TEST(Tags, SetGetHas) {
    EntityWorld w;
    EntityHandle h = w.create();
    h.set_tag(tags::ROLE, TagValue::from(std::string("fighter")));
    h.set_tag(tags::TEAM, TagValue::from(std::string("blue")));
    EXPECT_TRUE(h.has_tag(tags::ROLE));
    EXPECT_EQ(h.get_tag(tags::ROLE)->str_val, "fighter");
    EXPECT_FALSE(h.has_tag(tags::STEALTH));
}

TEST(Tags,WithTagFiltersByValue) {
    EntityWorld w;
    auto red1 = w.create(); red1.set_tag(tags::TEAM, TagValue::from(std::string("red")));
    auto blue1 = w.create(); blue1.set_tag(tags::TEAM, TagValue::from(std::string("blue")));
    auto red2 = w.create(); red2.set_tag(tags::TEAM, TagValue::from(std::string("red")));
    auto reds = w.with_tag(tags::TEAM, TagValue::from(std::string("red")));
    ASSERT_EQ(reds.size(), 2u);
}

TEST(Tags, DestroyedEntityDoesNotAppearInTagQueries) {
    EntityWorld w;
    EntityHandle h = w.create();
    h.set_tag(tags::TEAM, TagValue::from(std::string("red")));
    ASSERT_EQ(w.with_tag(tags::TEAM, TagValue::from(std::string("red"))).size(), 1u);
    w.destroy(h.id());
    EXPECT_EQ(w.with_tag(tags::TEAM, TagValue::from(std::string("red"))).size(), 0u);
}

// ============================================================================
// TransformComponent uses the strong-typed WorldPosition
// ============================================================================
TEST(TransformComponent, PositionIsStrongTypedWorldPosition) {
    // This is the design decision made concrete: the entity's source-of-truth
    // position is f4::geo::WorldPosition, NOT a raw (x,y,z). A future DIS
    // adapter converts via to_ecef(pos, datum); a radio call converts via
    // to_bra(own, target). The sim frame is always the stored form.
    EntityWorld w;
    EntityHandle h = w.create();
    auto& tf = h.add<TransformComponent>();
    tf.position = WorldPosition{5000.0, -3000.0, 20000.0};  // 5k east, 3k south, 20k up

    // Demonstrate the cross-frame conversion using the entity's position and
    // a theater datum — the kind of call a reporting/DIS system would make.
    TheaterDatum datum(LatLonAlt{38.0 * DEG_TO_RAD, -77.0 * DEG_TO_RAD, 0.0});
    LatLonAlt lla = to_lla(tf.position, datum);
    // 3000 ft south of a 38N origin -> latitude slightly less than 38 deg,
    // but still in the right neighborhood (not tens of miles away).
    EXPECT_LT(lla.lat, 38.0 * DEG_TO_RAD);
    EXPECT_GT(lla.lat, 37.9 * DEG_TO_RAD);
    // 5000 ft east of -77W origin -> longitude slightly more east (greater).
    EXPECT_GT(lla.lon, -77.0 * DEG_TO_RAD);
    // Altitude preserved across the frame crossing.
    EXPECT_NEAR(lla.alt, 20000.0, 1e-6);

    // BRA from the datum origin to this entity: slant range exceeds the
    // 20000 ft vertical component (there's horizontal offset too).
    BRA bra = to_bra(WorldPosition{0, 0, 0}, tf.position);
    EXPECT_GT(bra.range_ft, 20000.0);
}

// ============================================================================
// within_radius (linear scan over transforms)
// ============================================================================
TEST(WithinRadius, FindsEntitiesInsideAndExcludesOutside) {
    EntityWorld w;
    auto a = w.create(); a.add<TransformComponent>().position = WorldPosition{0, 0, 0};
    auto b = w.create(); b.add<TransformComponent>().position = WorldPosition{100, 0, 0};
    auto c = w.create(); c.add<TransformComponent>().position = WorldPosition{10000, 0, 0};
    auto ids = w.within_radius(0, 0, 0, 500.0);
    ASSERT_EQ(ids.size(), 2u);   // a and b are within 500 ft; c is not
}
