// test_ecm.cpp — the ECM tranche (SENSORS_COUNTERMEASURES_PLAN §8):
//
//   1. the radar burn-through model — a live enemy jammer in the beam
//      toward a candidate stretches the range the detection ramp reads;
//      closing the range wins through (the same jammer that blinds at
//      5 NM cannot blind at 1 NM); friendly/corpse/disabled pods and
//      jammers outside the beam degrade nothing.
//   2. the RWR hears jammers — the Jamming warning: classification,
//      rank order (Launch < Lock < Jamming < Search), transition
//      publishing, and the emitter rules (corpses stop, own pod never
//      warns itself).
//
// Identity discipline: every no-jammer (or ignored-jammer) case pins the
// exact pre-ECM track state — with no EcmComponent in the world the scan
// is arithmetic-free and the RNG stream is untouched.

#include <f4/sensors/rwr.hpp>

#include <f4/sensors/ecm.hpp>
#include <f4/sensors/messages.hpp>
#include <f4/sensors/radar_component.hpp>

#include <gtest/gtest.h>

#include <cmath>

using namespace f4::sensors;

namespace entities = f4::entities;
namespace messaging = f4::messaging;

namespace {

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

/// Radar at origin (own_team "blue"), target due north at `range_ft`
/// flying south (head-on, closing). `jam_on_target`: the target carries
/// a live pod; `jam_params` shapes it.
///
/// Jam sits at namespace scope: a default argument (`Jam jam = {}`)
/// inside the enclosing class would need the nested aggregate's
/// defaulted constructor while `HeadOn` is still incomplete — MSVC and
/// Clang allow it, GCC 14 rejects it ("default member initializer ...
/// required before the end of its enclosing class"). Hoisted, every
/// compiler reads the same file.
struct Jam {
    bool on = false;
    double strength = 1.0;
    double burn_through_nm = 20.0;
    const char* team = "red";
    bool dead = false;
    bool enabled = true;
};

struct HeadOn {
    World w;
    entities::EntityHandle radar;
    entities::EntityHandle target;

    explicit HeadOn(double range_ft, Jam jam = {}) {
        radar = w.world.create();
        radar.add<entities::TransformComponent>()
             .position = f4::geo::WorldPosition{0.0, 0.0, 20000.0};
        auto& r = radar.add<RadarSimComponent>();
        r.rng_seed = 0x46344ull;
        r.scan_interval_s = 1.0;
        r.own_team = "blue";

        target = w.world.create();
        auto& tf = target.add<entities::TransformComponent>();
        tf.position = f4::geo::WorldPosition{0.0, range_ft, 20000.0};
        tf.vy = -400.0;
        target.set_tag(entities::tags::TEAM,
                       f4::entities::TagValue::from(std::string("red")));
        if (jam.on) {
            auto& pod = target.add<EcmComponent>();
            pod.jamming_strength = jam.strength;
            pod.burn_through_range_nm = jam.burn_through_nm;
            pod.own_team = jam.team;
            pod.enabled = jam.enabled;
            if (jam.dead) {
                auto& d = target.add<entities::DamageStateComponent>();
                d.killed = true;
            }
        }
    }

    RadarSimComponent& r() { return *radar.get<RadarSimComponent>(); }

    void run(double seconds, double tick = 0.2) {
        for (double i = 0; i < seconds; i += tick) {
            RadarSimComponent::set_sim_time(RadarSimComponent::sim_time() +
                                            tick);
            w.world.update_all(tick, w.bus);
        }
    }
};

const TrackFile* target_track(HeadOn& s) {
    return s.r().tracks().find(s.target.id().value);
}

} // namespace

// ============================================================================
// The burn-through model (radar scan degradation)
// ============================================================================

TEST(EcmBurnThrough, ReferenceCaseStillAcquiresWithoutAnyJammer) {
    HeadOn s{5.0 * kFeetPerNm};
    s.run(2.5);
    const auto* t = target_track(s);
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->state, TrackState::Established);
}

TEST(EcmBurnThrough, StrongJammerOnTheTargetBlindsTheRadar) {
    // Self-screening: the pod rides the target at 5 NM, burn-through
    // 20 NM — the weight saturates, the ramp reads 5/(1-0.95) = 100 NM,
    // far beyond the 40 NM reference: P(detect) = 0, no roll, no track.
    HeadOn s{5.0 * kFeetPerNm,
             {.on = true, .strength = 1.0, .burn_through_nm = 20.0}};
    s.run(2.5);
    EXPECT_EQ(s.r().scans_performed(), 2u);  // the scan still ran
    EXPECT_EQ(target_track(s), nullptr);
}

TEST(EcmBurnThrough, ClosingTheRangeWinsThrough) {
    // The burn-through signature: the SAME jammer that blinds at 5 NM
    // cannot blind at 1 NM — the stretched 1/0.05 = 20 NM sits inside
    // the 0.75-knee (30 NM), P(detect) = 1.
    HeadOn s{1.0 * kFeetPerNm,
             {.on = true, .strength = 1.0, .burn_through_nm = 20.0}};
    s.run(2.5);
    const auto* t = target_track(s);
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->state, TrackState::Established);
}

TEST(EcmBurnThrough, FriendlyPodNeverDegradesOwnRadar) {
    // IFF: a pod advertising the radar's own team is ignored — the
    // track builds exactly as the no-jammer reference.
    HeadOn s{5.0 * kFeetPerNm,
             {.on = true, .team = "blue"}};
    s.run(2.5);
    const auto* t = target_track(s);
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->state, TrackState::Established);
}

TEST(EcmBurnThrough, CorpseAndDisabledPodsDoNotJam) {
    {
        HeadOn s{5.0 * kFeetPerNm, {.on = true, .dead = true}};
        s.run(2.5);
        const auto* t = target_track(s);
        ASSERT_NE(t, nullptr);
        EXPECT_EQ(t->state, TrackState::Established);
    }
    {
        HeadOn s{5.0 * kFeetPerNm, {.on = true, .enabled = false}};
        s.run(2.5);
        const auto* t = target_track(s);
        ASSERT_NE(t, nullptr);
        EXPECT_EQ(t->state, TrackState::Established);
    }
}

TEST(EcmBurnThrough, JammerOutsideTheBeamDegradesNothing) {
    // The pod is due SOUTH of the radar while the candidate is due
    // NORTH: the noise corridor is directional, and the antenna looking
    // north hears nothing from a stern jammer. The track builds exactly
    // as the no-jammer reference.
    World w;
    auto radar = w.world.create();
    radar.add<entities::TransformComponent>()
         .position = f4::geo::WorldPosition{0.0, 0.0, 20000.0};
    auto& r = radar.add<RadarSimComponent>();
    r.rng_seed = 0x46344ull;
    r.scan_interval_s = 1.0;
    r.own_team = "blue";

    auto target = w.world.create();
    auto& tf = target.add<entities::TransformComponent>();
    tf.position = f4::geo::WorldPosition{0.0, 5.0 * kFeetPerNm, 20000.0};
    tf.vy = -400.0;
    target.set_tag(entities::tags::TEAM,
                   f4::entities::TagValue::from(std::string("red")));

    auto jammer = w.world.create();
    jammer.add<entities::TransformComponent>()
          .position = f4::geo::WorldPosition{0.0, -5.0 * kFeetPerNm, 20000.0};
    jammer.add<EcmComponent>();  // red pod, saturated weight if consulted
    jammer.set_tag(entities::tags::TEAM,
                   f4::entities::TagValue::from(std::string("red")));

    for (double i = 0; i < 2.5; i += 0.2) {
        RadarSimComponent::set_sim_time(RadarSimComponent::sim_time() + 0.2);
        w.world.update_all(0.2, w.bus);
    }
    const auto* t = r.tracks().find(target.id().value);
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->state, TrackState::Established);
}

// ============================================================================
// The RWR hears jammers
// ============================================================================

TEST(RwrJamming, JammingSortsBetweenLockAndSearch) {
    const RwrModel model;
    const auto own = f4::geo::WorldPosition{0.0, 0.0, 0.0};

    std::vector<EmitterReading> readings;
    auto search = EmitterReading{};
    search.emitter_id = 5;
    search.position = f4::geo::WorldPosition{0.0, 10000.0, 0.0};
    search.is_illuminating_self = true;
    auto lock = EmitterReading{};
    lock.emitter_id = 9;
    lock.position = f4::geo::WorldPosition{10000.0, 0.0, 0.0};
    lock.is_locked_on_self = true;
    auto jam = EmitterReading{};
    jam.emitter_id = 7;
    jam.position = f4::geo::WorldPosition{-10000.0, 0.0, 0.0};
    jam.is_jamming_self = true;
    auto launch = EmitterReading{};
    launch.emitter_id = 3;
    launch.position = f4::geo::WorldPosition{0.0, 5000.0, 0.0};
    launch.is_missile = true;
    readings = {search, lock, jam, launch};

    const auto w = model.evaluate(readings, own, 1.0);
    ASSERT_EQ(w.size(), 4u);
    EXPECT_EQ(w[0].type, RwrWarningType::Launch);
    EXPECT_EQ(w[1].type, RwrWarningType::Lock);
    EXPECT_EQ(w[2].type, RwrWarningType::Jamming);
    EXPECT_EQ(w[3].type, RwrWarningType::Search);
}

TEST(RwrJamming, VictimHearsTheJammerOnce) {
    World w;
    auto victim = w.world.create();
    victim.add<entities::TransformComponent>()
          .position = f4::geo::WorldPosition{0.0, 0.0, 20000.0};
    victim.add<RwrComponent>();

    auto jammer = w.world.create();
    jammer.add<entities::TransformComponent>()
          .position = f4::geo::WorldPosition{0.0, 10.0 * kFeetPerNm, 20000.0};
    jammer.add<EcmComponent>();

    const auto messages = w.collect<RwrWarningMessage>();

    update_rwr(w.world, w.bus, 1.0);
    update_rwr(w.world, w.bus, 2.0);

    auto* rwr = victim.get<RwrComponent>();
    ASSERT_NE(rwr, nullptr);
    ASSERT_EQ(rwr->warnings.size(), 1u);
    EXPECT_EQ(rwr->warnings[0].type, RwrWarningType::Jamming);
    EXPECT_EQ(rwr->warnings[0].emitter_id, jammer.id().value);
    // No brain-facing activity flags flip for noise (the strobe is
    // informational — lock/launch stay the actionable threats).
    EXPECT_FALSE(rwr->lock_active);
    EXPECT_FALSE(rwr->launch_active);
    // Transition-published exactly once; the repeat is silent.
    ASSERT_EQ(messages.size(), 1u);
    EXPECT_EQ(messages[0].type, RwrWarningType::Jamming);
}

TEST(RwrJamming, CorpseStopsJammingAndOwnPodNeverWarnsItself) {
    World w;
    auto victim = w.world.create();
    victim.add<entities::TransformComponent>()
          .position = f4::geo::WorldPosition{0.0, 0.0, 20000.0};
    victim.add<RwrComponent>();
    victim.add<EcmComponent>();  // the victim's own pod

    auto corpse = w.world.create();
    corpse.add<entities::TransformComponent>()
          .position = f4::geo::WorldPosition{0.0, 10.0 * kFeetPerNm, 20000.0};
    corpse.add<EcmComponent>();
    corpse.add<entities::DamageStateComponent>().killed = true;

    update_rwr(w.world, w.bus, 1.0);
    auto* rwr = victim.get<RwrComponent>();
    ASSERT_NE(rwr, nullptr);
    EXPECT_TRUE(rwr->warnings.empty());
}
