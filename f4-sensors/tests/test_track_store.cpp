// test_track_store.cpp — track files: quality build-up, exponential decay,
// state ladder (Tentative/Established/Coasting/Dropped), IFF classification,
// NCTR carry, deterministic iteration, purge.

#include <f4/sensors/track_store.hpp>

#include <gtest/gtest.h>

#include <cmath>

using namespace f4::sensors;

namespace {

constexpr double kPi = 3.14159265358979323846;

TrackStore make_store() {
    return TrackStore{"blue", TrackStoreConfig{}};
}

f4::geo::WorldPosition pos(double x, double y, double z) {
    return f4::geo::WorldPosition{x, y, z};
}

f4::math::Vec3<double> vel(double x, double y, double z) {
    return f4::math::Vec3<double>{x, y, z};
}

} // namespace

TEST(TrackStoreBuild, FirstDetectionIsTentative) {
    TrackStore s = make_store();
    s.on_detection(7, pos(0, 1000, 0), vel(0, 400, 0), 1.0, "red", "");
    const TrackFile* t = s.find(7);
    ASSERT_NE(t, nullptr);
    EXPECT_DOUBLE_EQ(t->quality, 0.34);  // one gain
    EXPECT_EQ(t->state, TrackState::Tentative);
    EXPECT_TRUE(t->hostile_by_iff);      // red vs blue
    EXPECT_EQ(t->team, "red");
    EXPECT_EQ(t->first_detected_s, 1.0);
    EXPECT_EQ(t->last_detected_s, 1.0);
}

TEST(TrackStoreBuild, TwoDetectionsEstablish) {
    TrackStore s = make_store();
    s.on_detection(7, pos(0, 1000, 0), vel(0, 400, 0), 1.0, "red", "");
    s.on_detection(7, pos(0, 1400, 0), vel(0, 400, 0), 2.0, "red", "");
    const TrackFile* t = s.find(7);
    ASSERT_NE(t, nullptr);
    EXPECT_DOUBLE_EQ(t->quality, 0.68);
    EXPECT_EQ(t->state, TrackState::Established);
    EXPECT_EQ(t->position.y, 1400.0);    // refreshed
    EXPECT_EQ(t->last_detected_s, 2.0);
}

TEST(TrackStoreBuild, EstablishedTrackMissesScanCoasts) {
    TrackStore s = make_store();
    s.on_detection(7, pos(0, 1000, 0), vel(0, 400, 0), 1.0, "red", "");
    s.on_detection(7, pos(0, 1400, 0), vel(0, 400, 0), 2.0, "red", "");
    ASSERT_EQ(s.find(7)->state, TrackState::Established);

    // Scan at t=3 misses track 7: quality decays, state coasts.
    const auto dropped = s.decay_untracked(3.0);
    EXPECT_TRUE(dropped.empty());
    const TrackFile* t = s.find(7);
    EXPECT_EQ(t->state, TrackState::Coasting);
    EXPECT_DOUBLE_EQ(t->quality, 0.68 * std::exp(-1.0 / 8.0));
    EXPECT_EQ(t->last_detected_s, 2.0);  // not refreshed
}

TEST(TrackStoreBuild, DetectedTracksAreNotDecayed) {
    TrackStore s = make_store();
    s.on_detection(7, pos(0, 1000, 0), vel(0, 400, 0), 1.0, "red", "");
    EXPECT_TRUE(s.decay_untracked(1.0).empty());  // same timestamp: skip
    EXPECT_DOUBLE_EQ(s.find(7)->quality, 0.34);
    EXPECT_EQ(s.find(7)->state, TrackState::Tentative);
}

TEST(TrackStoreDecay, QualityCollapsesToDropped) {
    TrackStoreConfig cfg;
    cfg.quality_gain = 0.34;
    cfg.drop_quality = 0.05;
    cfg.decay_tau_s = 2.0;   // fast decay for the test
    TrackStore s{"blue", cfg};
    s.on_detection(9, pos(0, 0, 0), vel(0, 0, 0), 0.0, "red", "");
    // Long gap: 3 seconds on tau=2 -> quality 0.34 * e^-1.5 ~ 0.076
    // Another 3 -> ~0.017 < drop threshold.
    auto dropped = s.decay_untracked(3.0);
    EXPECT_TRUE(dropped.empty());
    dropped = s.decay_untracked(6.0);
    ASSERT_EQ(dropped.size(), 1u);
    EXPECT_EQ(dropped[0], 9u);
    EXPECT_EQ(s.find(9)->state, TrackState::Dropped);
    EXPECT_EQ(s.live_count(), 0u);
}

TEST(TrackStoreDecay, StaleTimeoutDrops) {
    TrackStoreConfig cfg;
    cfg.stale_timeout_s = 20.0;
    cfg.decay_tau_s = 100.0;  // slow decay — staleness does the dropping
    TrackStore s{"blue", cfg};
    s.on_detection(4, pos(0, 0, 0), vel(0, 0, 0), 0.0, "red", "");
    const auto dropped = s.decay_untracked(20.1);  // age > timeout
    ASSERT_EQ(dropped.size(), 1u);
    EXPECT_EQ(s.find(4)->state, TrackState::Dropped);
}

TEST(TrackStoreDecay, ReDetectionRestoresEstablished) {
    TrackStore s = make_store();
    s.on_detection(7, pos(0, 1000, 0), vel(0, 400, 0), 1.0, "red", "");
    s.on_detection(7, pos(0, 1400, 0), vel(0, 400, 0), 2.0, "red", "");
    EXPECT_TRUE(s.decay_untracked(3.0).empty());  // coasts, not dropped
    ASSERT_EQ(s.find(7)->state, TrackState::Coasting);

    s.on_detection(7, pos(0, 1800, 0), vel(0, 400, 0), 4.0, "red", "");
    const TrackFile* t = s.find(7);
    EXPECT_EQ(t->state, TrackState::Established);
    // 0.68 * e^(-1/8) decayed, then +0.34 gain.
    EXPECT_NEAR(t->quality, 0.68 * std::exp(-1.0 / 8.0) + 0.34, 1e-9);
}

TEST(TrackStoreIff, FriendlyNotHostile) {
    TrackStore s{"blue", TrackStoreConfig{}};
    s.on_detection(1, pos(0, 0, 0), vel(0, 0, 0), 0.0, "blue", "");
    s.on_detection(2, pos(1, 0, 0), vel(0, 0, 0), 0.0, "red", "");
    s.on_detection(3, pos(2, 0, 0), vel(0, 0, 0), 0.0, "", "");  // unknown
    EXPECT_FALSE(s.find(1)->hostile_by_iff);
    EXPECT_TRUE(s.find(2)->hostile_by_iff);
    EXPECT_FALSE(s.find(3)->hostile_by_iff);  // unknown team: not auto-hostile
}

TEST(TrackStoreNctr, CarriesResolvedIdentity) {
    TrackStore s = make_store();
    s.on_detection(7, pos(0, 0, 0), vel(0, 0, 0), 1.0, "red", "");
    EXPECT_EQ(s.find(7)->nctr, "");  // unknown until the caller resolves it
    s.on_detection(7, pos(0, 0, 0), vel(0, 0, 0), 2.0, "red", "FALCON 1");
    EXPECT_EQ(s.find(7)->nctr, "FALCON 1");
}

TEST(TrackStoreOrder, LiveIterationIsAscendingEntityId) {
    TrackStore s = make_store();
    s.on_detection(30, pos(0, 0, 0), vel(0, 0, 0), 0.0, "red", "");
    s.on_detection(10, pos(0, 0, 0), vel(0, 0, 0), 0.0, "red", "");
    s.on_detection(20, pos(0, 0, 0), vel(0, 0, 0), 0.0, "red", "");
    const auto live = s.live();
    ASSERT_EQ(live.size(), 3u);
    EXPECT_EQ(live[0]->entity_id, 10u);
    EXPECT_EQ(live[1]->entity_id, 20u);
    EXPECT_EQ(live[2]->entity_id, 30u);
}

TEST(TrackStorePurge, RemovesOnlyDropped) {
    TrackStore s = make_store();
    s.on_detection(1, pos(0, 0, 0), vel(0, 0, 0), 0.0, "red", "");
    s.on_detection(2, pos(0, 0, 0), vel(0, 0, 0), 0.0, "blue", "");
    EXPECT_EQ(s.decay_untracked(100.0).size(), 2u);  // both stale -> dropped
    EXPECT_EQ(s.live_count(), 0u);
    EXPECT_EQ(s.total_count(), 2u);
    EXPECT_EQ(s.purge_dropped(), 2u);
    EXPECT_EQ(s.total_count(), 0u);
    EXPECT_EQ(s.find(1), nullptr);
    // Re-detection after purge starts fresh.
    s.on_detection(1, pos(0, 0, 0), vel(0, 0, 0), 200.0, "red", "");
    EXPECT_EQ(s.find(1)->first_detected_s, 200.0);
}

TEST(TrackStoreEstablishedQuery, ReturnsEstablishedAndCoasting) {
    TrackStore s = make_store();
    s.on_detection(1, pos(0, 0, 0), vel(0, 0, 0), 1.0, "red", "");  // tentative
    s.on_detection(2, pos(0, 0, 0), vel(0, 0, 0), 1.0, "red", "");  // -> established (2 scans below)
    s.on_detection(2, pos(0, 0, 0), vel(0, 0, 0), 2.0, "red", "");
    EXPECT_TRUE(s.decay_untracked(3.0).empty());  // 2 coasts, 1 tentative
    const auto est = s.established();
    ASSERT_EQ(est.size(), 1u);
    EXPECT_EQ(est[0]->entity_id, 2u);
    EXPECT_EQ(est[0]->state, TrackState::Coasting);
}
