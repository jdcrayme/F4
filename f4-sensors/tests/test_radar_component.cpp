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

    explicit HeadOn(double range_ft, std::uint32_t seed = 0x46344ull,
                    double phase = 0.0) {
        radar = w.world.create();
        radar.add<entities::TransformComponent>()
             .position = f4::geo::WorldPosition{0.0, 0.0, 20000.0};
        auto& r = radar.add<RadarSimComponent>();
        r.rng_seed = seed;         // lazy-baked on first update
        r.scan_interval_s = 1.0;
        r.scan_phase_s = phase;    // AGG-2b: the per-unit sweep phase

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

// ============================================================================
// FID-OPT-3: the scan's candidate walk pre-applies the clutter + range
// gates on the component-type ref bucket. The pins: clutter NEVER tracks,
// and the detection timeline is INVARIANT to the clutter population (the
// pre-gates are RNG-neutral, so adding clutter cannot shift the rolls).
// ============================================================================
namespace {
/// HeadOn plus `n` parked ground entities scattered between the radar and
/// the target (zero velocity, near the ground — the clutter predicate).
struct HeadOnWithClutter {
    World w;
    entities::EntityHandle radar;
    entities::EntityHandle target;

    explicit HeadOnWithClutter(double range_ft, int n,
                               std::uint32_t seed = 0x46344ull) {
        radar = w.world.create();
        radar.add<entities::TransformComponent>()
             .position = f4::geo::WorldPosition{0.0, 0.0, 20000.0};
        auto& r = radar.add<RadarSimComponent>();
        r.rng_seed = seed;
        r.scan_interval_s = 1.0;

        for (int i = 0; i < n; ++i) {
            auto g = w.world.create();
            auto& tf = g.add<entities::TransformComponent>();
            // Parked vehicles: stationary, near the ground, spread across
            // the theater (some inside the cutoff ball, some outside —
            // both classes must reject).
            tf.position = f4::geo::WorldPosition{
                static_cast<double>(i % 7) * 20000.0 - 60000.0,
                static_cast<double>(i) * 2500.0,
                100.0};
        }

        target = w.world.create();
        auto& tf = target.add<entities::TransformComponent>();
        tf.position = f4::geo::WorldPosition{0.0, range_ft, 20000.0};
        tf.vy = -400.0;
        target.set_tag(entities::tags::TEAM,
                       f4::entities::TagValue::from(std::string("red")));
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
} // namespace

TEST(RadarScan, ClutterNeverTracksEvenThroughTheRefWalk) {
    HeadOnWithClutter s{30.0 * kFeetPerNm, /*n=*/300};
    s.run(3.0);  // three scans

    // The air target established a track; no clutter entity ever did.
    EXPECT_NE(s.r().tracks().find(s.target.id().value), nullptr)
        << "the in-volume air target was not tracked";
    for (const auto* t : s.r().tracks().live()) {
        if (t->entity_id == s.target.id().value) continue;
        auto h = entities::EntityHandle(entities::EntityId{t->entity_id},
                                        &s.w.world);
        const auto* tf = h.get<entities::TransformComponent>();
        if (tf != nullptr) {
            EXPECT_FALSE(tf->is_ground_clutter())
                << "a ground-clutter entity entered the track store";
        }
    }
}

TEST(RadarScan, DetectionTimelineInvariantToClutterPopulation) {
    // THE byte-safety pin for the FID-OPT-3 scan walk: the pre-gates
    // consume no RNG, so the detection timeline for the SAME seed must be
    // identical whether the theater holds 0 or 2,000 parked ground
    // entities. (Pre-OPT-3 this held because the roll loop drew in
    // candidate order; post-OPT-3 the pre-gated walk must preserve that
    // order exactly.)
    auto run_once = [](int clutter) {
        HeadOnWithClutter s{44.0 * kFeetPerNm, clutter, 0xABCD};
        std::vector<bool> detected;
        for (int i = 0; i < 12; ++i) {
            s.run(1.0);
            const auto* t = s.r().tracks().find(s.target.id().value);
            const bool saw_now =
                t != nullptr &&
                t->last_detected_s >= RadarSimComponent::sim_time() - 0.5;
            detected.push_back(saw_now);
        }
        return detected;
    };

    const auto bare = run_once(0);
    const auto cluttered = run_once(2000);
    EXPECT_EQ(bare, cluttered)
        << "adding ground clutter changed the detection timeline — the "
           "pre-gated walk reordered or re-streamed the rolls";
    EXPECT_FALSE(bare.empty());
}

// ============================================================================
// AGG-2b: the candidate walk rides the world's air-picture roster
// (f4-entities' AirPictureRoster — the wired SpatialIndex term). The
// pins: the roster's members ARE the pre-AGG-2b candidate pool (set,
// order, and RNG stream preserved — the invariance pin above now runs
// through the roster path), a behavioral flip (the taxi launch) joins
// at the roster's revalidation cadence, the Track mode is untouched,
// and the per-unit sweep phase shifts the schedule without breaking
// the cadence.
// ============================================================================

TEST(RadarScan, BehavioralFlipJoinsAtTheRosterCadence) {
    // A parked LOW target: clutter at the roster's prime, so the first
    // scans see nothing even though the geometry is the HeadOn pin's own
    // (20 NM head-on would detect immediately once it is a candidate).
    // It starts moving mid-window: the flip joins at the revalidation
    // cadence — AGG-2b's documented, bounded latency — and detection
    // follows on the next scan.
    World w;
    auto radar = w.world.create();
    radar.add<entities::TransformComponent>()
         .position = f4::geo::WorldPosition{0.0, 0.0, 20000.0};
    auto& r = radar.add<RadarSimComponent>();
    r.rng_seed = 0x46344ull;
    r.scan_interval_s = 1.0;
    r.roster_revalidate_s = 2.0;  // a wide, test-controlled window

    auto target = w.world.create();
    target.add<entities::TransformComponent>()
          .position = f4::geo::WorldPosition{0.0, 20.0 * kFeetPerNm, 0.0};
    target.set_tag(entities::tags::TEAM,
                   f4::entities::TagValue::from(std::string("red")));

    auto run = [&](double seconds) {
        for (double i = 0; i < seconds; i += 0.2) {
            RadarSimComponent::set_sim_time(RadarSimComponent::sim_time() +
                                            0.2);
            w.world.update_all(0.2, w.bus);
        }
    };

    run(1.5);  // first scan at t=1.0: primes the roster — target is clutter
    EXPECT_EQ(r.tracks().live_count(), 0u);

    // The taxi launch at t=1.5 (moving south, head-on — the detectable
    // shape). The t=2.0 scan sits INSIDE the window primed at t=1.0:
    // no revalidation, still not a candidate.
    target.get<entities::TransformComponent>()->vy = -400.0;
    run(1.0);  // scan at t=2.0
    EXPECT_EQ(r.tracks().live_count(), 0u)
        << "a behavioral flip became a candidate before the revalidation "
           "cadence elapsed";

    // The t=3.0 scan: 3.0 - 1.0 >= 2.0 — the window elapses, the
    // revalidation catches the flip, the scan detects.
    run(1.0);  // scan at t=3.0
    ASSERT_EQ(r.tracks().live_count(), 1u);
    const auto* t = r.tracks().find(target.id().value);
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->state, TrackState::Tentative);  // first detection
}

TEST(RadarScan, LandedMemberLeavesTheCandidatePoolImmediately) {
    // The opposite flip: a tracked airborne target LANDS (stops, low).
    // The roster keeps it until revalidation, but the scan's FRESH
    // clutter gate drops it as a candidate the very next scan — no new
    // detection lands after touchdown, and the track decays out on the
    // normal schedule (the membership latency never resurrects a landed
    // contact into a live track).
    HeadOn s{20.0 * kFeetPerNm};
    s.run(2.5);  // scans at t=1.0, 2.0: established
    ASSERT_EQ(s.r().tracks().live_count(), 1u);
    const double last_airborne_detection =
        s.r().tracks().find(s.target.id().value)->last_detected_s;

    // Touchdown: velocity 0, below the clutter floor.
    auto* tf = s.target.get<entities::TransformComponent>();
    tf->vy = 0.0;
    tf->position.z = 0.0;
    s.run(3.0);  // scans at t=3.0..5.0: not one re-detects it
    const auto* t = s.r().tracks().find(s.target.id().value);
    ASSERT_NE(t, nullptr);  // still decaying (tau 8 s, stale 20 s)
    EXPECT_EQ(t->last_detected_s, last_airborne_detection)
        << "a landed member was re-detected after touchdown";
}

TEST(RadarScan, ScanPhaseShiftsTheSweepScheduleExactly) {
    // Phase 0 (the default): the pre-AGG-2b schedule — interval edges
    // on 0.2 s ticks (first sweep at t=1.0).
    HeadOn zero{20.0 * kFeetPerNm};
    zero.run(0.6);
    EXPECT_EQ(zero.r().scans_performed(), 0u);
    zero.run(0.4);  // t=1.0
    EXPECT_EQ(zero.r().scans_performed(), 1u);

    // Phase 0.5 s: the first sweep lands half an interval into the
    // timer (0.5 + 0.2 k) — on this tick grid at t=0.6 — and the
    // cadence stays EXACTLY 1.0 s apart thereafter (the fmod carry;
    // run(0.8) = four updates land at t=1.4, one update short of the
    // second edge at t=1.6).
    HeadOn half{20.0 * kFeetPerNm, 0x46344ull, /*phase=*/0.5};
    half.run(0.6);
    ASSERT_EQ(half.r().scans_performed(), 1u);  // first sweep early
    half.run(0.8);                              // t=1.4: no second yet
    EXPECT_EQ(half.r().scans_performed(), 1u);
    half.run(0.2);                              // t=1.6: second sweep
    EXPECT_EQ(half.r().scans_performed(), 2u);
}

TEST(RadarScan, PhaseShiftMovesTimingNotOutcomes) {
    // The stagger is a SCHEDULING change only: same world, same seed,
    // same geometry — the scan's candidate set, order, and RNG stream
    // are phase-independent, so the per-scan outcomes are identical;
    // only WHEN they land moves. The target sits STATIONARY AT ALTITUDE
    // (a roster member — the clutter rule's documented edge — with
    // closure 0, a constant Pd in the ramp), so the k-th roll of both
    // radars sees the SAME Pd with the SAME stream: the detection
    // sequence is the seed's, not the phase's.
    auto mk = [](double phase, World& w, entities::EntityHandle& radar_out,
                 std::uint64_t& target_id) {
        radar_out = w.world.create();
        radar_out.add<entities::TransformComponent>()
             .position = f4::geo::WorldPosition{0.0, 0.0, 20000.0};
        auto& r = radar_out.add<RadarSimComponent>();
        r.rng_seed = 0xABCD;
        r.scan_interval_s = 1.0;
        r.scan_phase_s = phase;
        auto target = w.world.create();
        target.add<entities::TransformComponent>()
              .position = f4::geo::WorldPosition{
                  0.0, 20.0 * kFeetPerNm, 20000.0};  // stationary, high:
                                                     // in volume, Pd ~ 1
        target.set_tag(entities::tags::TEAM,
                       f4::entities::TagValue::from(std::string("red")));
        target_id = target.id().value;
    };
    World wa, wb;
    entities::EntityHandle ra, rb;
    std::uint64_t ta_id = 0, tb_id = 0;
    mk(0.0, wa, ra, ta_id);
    mk(0.5, wb, rb, tb_id);
    auto* rpa = ra.get<RadarSimComponent>();
    auto* rpb = rb.get<RadarSimComponent>();

    for (int i = 0; i < 12; ++i) {
        RadarSimComponent::set_sim_time(RadarSimComponent::sim_time() + 1.0);
        wa.world.update_all(1.0, wa.bus);
        wb.world.update_all(1.0, wb.bus);
    }
    // Equal scan counts (one per window on both grids), and the roll
    // outcomes identical: same accumulated detections, same track state.
    EXPECT_EQ(rpa->scans_performed(), rpb->scans_performed());
    const auto* ta = rpa->tracks().find(ta_id);
    const auto* tb = rpb->tracks().find(tb_id);
    ASSERT_NE(ta, nullptr);
    ASSERT_NE(tb, nullptr);
    EXPECT_EQ(ta->quality, tb->quality);
    EXPECT_EQ(ta->state, tb->state);
}
