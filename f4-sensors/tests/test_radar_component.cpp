// test_radar_component.cpp — RadarSimComponent end-to-end in a bare
// EntityWorld: scan timing, acquisition, quality build-up, decay + drop
// transitions, STT lock/unlock, NCTR resolution, IFF, out-of-volume
// rejection, RNG determinism, RWR coupling.

#include <f4/sensors/radar_component.hpp>
#include <f4/sensors/rwr.hpp>

#include <f4/entities/entity.hpp>
#include <f4/sensors/messages.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using namespace f4::sensors;

namespace entities = f4::entities;
namespace messaging = f4::messaging;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kFeetPerNm = 6076.11548;

struct World {
    entities::EntityWorld world;
    messaging::MessageBus bus;

    template <typename Msg>
    std::vector<Msg> collect() {
        std::vector<Msg> out;
        bus.subscribe<Msg>([&](const Msg& m) { out.push_back(m); });
        return out;
    }
};

/// Radar at origin; target due north at `range_ft`, both at 20,000 ft,
/// target flying SOUTH (head-on aspect, closing on the radar).
struct HeadOn {
    World w;
    entities::EntityHandle radar;
    entities::EntityHandle target;

    explicit HeadOn(double range_ft, std::uint32_t seed = 0x46344ull) {
        radar = w.world.create();
        radar.add<entities::TransformComponent>()
             .position = f4::geo::WorldPosition{0.0, 0.0, 20000.0};
        auto& r = radar.add<RadarSimComponent>();
        r.rng_seed = seed;         // lazy-baked on first update
        r.scan_interval_s = 1.0;

        target = w.world.create();
        auto& tf = target.add<entities::TransformComponent>();
        tf.position = f4::geo::WorldPosition{0.0, range_ft, 20000.0};
        tf.vy = -400.0;  // flying south: head-on, closing
        target.set_tag(entities::tags::TEAM, f4::entities::TagValue::from(std::string("red")));
    }

    RadarSimComponent& r() { return *radar.get<RadarSimComponent>(); }

    /// Advance `seconds` in `tick` steps, stamping the sim clock each tick
    /// (the tests play the host).
    void run(double seconds, double tick = 0.2) {
        for (double i = 0; i < seconds; i += tick) {
            RadarSimComponent::set_sim_time(RadarSimComponent::sim_time() + tick);
            w.world.update_all(tick, w.bus);
        }
    }
};

} // namespace

TEST(RadarScan, AcquiresAndEstablishesTrackInVolume) {
    HeadOn s{20.0 * kFeetPerNm};  // 20 NM: well inside 40 NM reference
    const auto acquired = s.w.collect<RadarTrackAcquiredMessage>();

    s.run(2.5);  // scans at t=1.0 and t=2.0

    EXPECT_EQ(s.r().scans_performed(), 2u);
    const auto* t = s.r().tracks().find(s.target.id().value);
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->state, TrackState::Established);  // 2 detections
    EXPECT_TRUE(t->hostile_by_iff);
    EXPECT_EQ(t->position.y, 20.0 * kFeetPerNm);   // last detected position

    ASSERT_EQ(acquired.size(), 1u);  // acquired exactly once
    EXPECT_EQ(acquired[0].target_entity_id, s.target.id().value);
    EXPECT_EQ(acquired[0].radar_entity_id, s.radar.id().value);
}

TEST(RadarScan, NoDetectionOutsideScanVolume) {
    HeadOn s{20.0 * kFeetPerNm};
    // Default bar points north (center 0) — put the target due EAST.
    s.target.get<entities::TransformComponent>()->position =
        f4::geo::WorldPosition{20.0 * kFeetPerNm, 0.0, 20000.0};

    s.run(2.5);
    EXPECT_EQ(s.r().scans_performed(), 2u);
    EXPECT_EQ(s.r().tracks().live_count(), 0u);  // never seen
    EXPECT_EQ(s.r().tracks().find(s.target.id().value), nullptr);
}

TEST(RadarScan, NoDetectionBeyondDetectionRange) {
    HeadOn s{120.0 * kFeetPerNm};  // 120 NM: Pd = 0 (closure caps at +25% -> 50 NM max)
    s.run(2.5);
    EXPECT_EQ(s.r().tracks().live_count(), 0u);
}

TEST(RadarScan, TrackDropsWhenTargetLeavesVolume) {
    HeadOn s{20.0 * kFeetPerNm};
    const auto dropped = s.w.collect<RadarTrackDroppedMessage>();
    const auto acquired = s.w.collect<RadarTrackAcquiredMessage>();

    s.run(2.5);                      // 2 scans: established
    ASSERT_EQ(s.r().tracks().live_count(), 1u);
    ASSERT_EQ(acquired.size(), 1u);

    // Turn the antenna away and keep scanning: quality decays out.
    s.r().scan.azimuth_center_rad = kPi;  // south — target now outside
    s.run(60.0);                          // decay tau 8 s, stale 20 s
    EXPECT_EQ(s.r().tracks().live_count(), 0u);
    ASSERT_EQ(dropped.size(), 1u);
    EXPECT_EQ(dropped[0].target_entity_id, s.target.id().value);
}

TEST(RadarCommand, TrackRequiresLiveTrackAndLocks) {
    HeadOn s{20.0 * kFeetPerNm};

    // Cannot lock an untracked target.
    EXPECT_FALSE(s.r().command_track(s.target.id().value));
    EXPECT_EQ(s.r().mode(), RadarMode::Search);

    s.run(2.5);  // establish
    ASSERT_TRUE(s.r().command_track(s.target.id().value));
    EXPECT_EQ(s.r().mode(), RadarMode::Track);
    EXPECT_EQ(s.r().locked_target(), s.target.id().value);

    // In Track mode the target is scanned even OUTSIDE the search volume.
    s.r().scan.azimuth_center_rad = kPi;  // bar points away
    s.run(2.0);
    const auto* t = s.r().tracks().find(s.target.id().value);
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->state, TrackState::Established);  // refreshed every scan
}

TEST(RadarCommand, LostTrackBreaksLockAutomatically) {
    HeadOn s{20.0 * kFeetPerNm};
    s.run(2.5);
    ASSERT_TRUE(s.r().command_track(s.target.id().value));

    // Remove the target from the world entirely: next scan reverts to Search.
    s.w.world.destroy(s.target.id());
    s.run(1.5);
    EXPECT_EQ(s.r().mode(), RadarMode::Search);
    EXPECT_EQ(s.r().locked_target(), 0u);
}

TEST(RadarCommand, SearchCommandClearsLock) {
    HeadOn s{20.0 * kFeetPerNm};
    s.run(2.5);
    ASSERT_TRUE(s.r().command_track(s.target.id().value));
    s.r().command_search();
    EXPECT_EQ(s.r().mode(), RadarMode::Search);
    EXPECT_EQ(s.r().locked_target(), 0u);
}

TEST(RadarNctr, ResolvesAfterEnoughScans) {
    HeadOn s{20.0 * kFeetPerNm};
    s.target.add<entities::CampaignIdentityComponent>().callsign = "FALCON 2";

    s.run(1.5);  // one scan
    EXPECT_EQ(s.r().tracks().find(s.target.id().value)->nctr, "");

    s.run(1.0);  // second scan
    EXPECT_EQ(s.r().tracks().find(s.target.id().value)->nctr, "FALCON 2");
}

TEST(RadarRng, SameSeedSameScenarioSameTimeline) {
    auto run_once = [](std::uint32_t seed) {
        HeadOn s{44.0 * kFeetPerNm, seed};  // 44 NM: Pd in the ramp, not 1.0
        std::vector<bool> detected;
        for (int i = 0; i < 12; ++i) {
            s.run(1.0);
            const auto* t = s.r().tracks().find(s.target.id().value);
            const bool saw_now = t != nullptr &&
                                 t->last_detected_s >= RadarSimComponent::sim_time() - 0.5;
            detected.push_back(saw_now);
        }
        return detected;
    };

    const auto a = run_once(0xABCD);
    const auto b = run_once(0xABCD);
    const auto c = run_once(0x1234);
    EXPECT_EQ(a, b);  // same seed: identical detection timeline
    // Different seed: overwhelmingly likely to differ on at least one scan.
    // (Not a statistical flake — both timelines are deterministic; if this
    // ever fires, the scenario reached Pd==1 for all scans and should be
    // pushed farther out.)
    EXPECT_NE(a, c);
}

TEST(RadarRwr, LockingFeedsVictimRwrThroughTheSweep) {
    // The full sensor loop: radar locks -> update_rwr publishes to the
    // victim's RwrComponent. Uses two HeadOn worlds glued manually.
    World w;
    auto radar = w.world.create();
    radar.add<entities::TransformComponent>()
         .position = f4::geo::WorldPosition{0.0, 0.0, 20000.0};
    radar.add<RadarSimComponent>();

    auto victim = w.world.create();
    victim.add<entities::TransformComponent>()
          .position = f4::geo::WorldPosition{0.0, 20.0 * kFeetPerNm, 20000.0};
    victim.add<RwrComponent>();

    auto* r = radar.get<RadarSimComponent>();
    r->scan_interval_s = 1.0;
    w.world.update_all(1.0, w.bus);   // first scan: acquire
    w.world.update_all(1.0, w.bus);   // second scan: establish

    const auto messages = w.collect<RwrWarningMessage>();
    ASSERT_TRUE(r->command_track(victim.id().value));
    update_rwr(w.world, w.bus, 2.5);

    auto* rwr = victim.get<RwrComponent>();
    EXPECT_TRUE(rwr->lock_active);
    ASSERT_EQ(messages.size(), 1u);
    EXPECT_EQ(messages[0].victim_id, victim.id().value);
    EXPECT_EQ(messages[0].emitter_id, radar.id().value);
    EXPECT_NEAR(messages[0].bearing_rad, kPi, 1e-9);  // radar due SOUTH of victim
}
