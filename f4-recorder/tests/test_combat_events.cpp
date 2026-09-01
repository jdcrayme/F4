// f4-recorder/tests/test_combat_events.cpp
//
// Unit tests for the M4 combat-event recording half of FlightRecorder:
//   - CombatEvent recording + queries
//   - JSON round-trip of every kind's payload fields
//   - Missile-track snapshots (the "missile" flag) round-trip
//   - Backward/forward format compatibility:
//       * old documents (no combat_events, no missile keys) load clean
//       * new documents with aircraft-only snapshots serialize
//         byte-identically to the pre-M4 format
//   - the combat debrief section of to_summary_json()

#include <gtest/gtest.h>

#include <f4/recorder/flight_recorder.hpp>

#include <cstddef>
#include <string>

using namespace f4::recorder;

namespace {

CombatEvent make_launch(std::uint64_t tick, double t,
                        std::uint64_t shooter, std::uint64_t target,
                        std::uint64_t missile) {
    CombatEvent e;
    e.tick = tick;
    e.sim_time_s = t;
    e.kind = CombatEventKind::MissileLaunched;
    e.subject_id = shooter;
    e.object_id = target;
    e.missile_id = missile;
    e.weapon_handle = 42;
    e.weapon_name = "AIM-120C";
    e.position = f4::geo::WorldPosition(100.0, 200.0, 10000.0);
    e.speed_ft_s = 900.0;
    return e;
}

CombatEvent make_kill(std::uint64_t tick, double t,
                      std::uint64_t victim, std::uint64_t killer) {
    CombatEvent e;
    e.tick = tick;
    e.sim_time_s = t;
    e.kind = CombatEventKind::EntityKilled;
    e.subject_id = victim;
    e.object_id = killer;
    return e;
}

FlightSnapshot make_aircraft(std::uint64_t tick, double t,
                             std::uint64_t entity_id) {
    FlightSnapshot s;
    s.tick = tick;
    s.sim_time_s = t;
    s.entity_id = entity_id;
    s.callsign = "EAGLE1";
    s.ai_mode = "NavigationMode";
    s.ai_state = "Enroute";
    return s;
}

} // anonymous namespace

// ============================================================================
// Recording + queries
// ============================================================================

TEST(CombatEvents, RecordAndCount) {
    FlightRecorder rec;
    EXPECT_EQ(rec.combat_event_count(), 0u);
    EXPECT_TRUE(rec.combat_events().empty());

    rec.record(make_launch(100, 5.0, 3, 4, 7));
    rec.record(make_kill(200, 40.0, 4, 3));
    EXPECT_EQ(rec.combat_event_count(), 2u);
    EXPECT_EQ(rec.combat_events()[0].kind, CombatEventKind::MissileLaunched);
    EXPECT_EQ(rec.combat_events()[1].kind, CombatEventKind::EntityKilled);
}

TEST(CombatEvents, RecordMove) {
    FlightRecorder rec;
    CombatEvent e = make_launch(100, 5.0, 3, 4, 7);
    rec.record(std::move(e));
    EXPECT_EQ(rec.combat_event_count(), 1u);
}

TEST(CombatEvents, EventsInRange) {
    FlightRecorder rec;
    rec.record(make_launch(50,  5.0, 3, 4, 7));
    rec.record(make_kill (120, 12.0, 4, 3));
    rec.record(make_kill (300, 30.0, 4, 3));

    auto in = rec.combat_events_in_range(10.0, 20.0);
    EXPECT_EQ(in.size(), 1u);
    EXPECT_EQ(in[0].sim_time_s, 12.0);

    // Boundary values are inclusive (same 1 µs tolerance as snapshots).
    auto edges = rec.combat_events_in_range(5.0, 12.0);
    EXPECT_EQ(edges.size(), 2u);

    EXPECT_TRUE(rec.combat_events_in_range(100.0, 200.0).empty());
}

// ============================================================================
// Kind names
// ============================================================================

TEST(CombatEvents, KindNamesAreStableWireStrings) {
    EXPECT_STREQ(combat_event_kind_name(CombatEventKind::TrackAcquired),
                 "track_acquired");
    EXPECT_STREQ(combat_event_kind_name(CombatEventKind::TrackDropped),
                 "track_dropped");
    EXPECT_STREQ(combat_event_kind_name(CombatEventKind::RwrLock), "rwr_lock");
    EXPECT_STREQ(combat_event_kind_name(CombatEventKind::RwrLaunch),
                 "rwr_launch");
    EXPECT_STREQ(combat_event_kind_name(CombatEventKind::MissileLaunched),
                 "missile_launched");
    EXPECT_STREQ(combat_event_kind_name(CombatEventKind::MissileDetonated),
                 "missile_detonated");
    EXPECT_STREQ(combat_event_kind_name(CombatEventKind::DamageApplied),
                 "damage_applied");
    EXPECT_STREQ(combat_event_kind_name(CombatEventKind::EntityKilled),
                 "entity_killed");
}

// ============================================================================
// JSON round-trip: every kind's payload
// ============================================================================

TEST(CombatEvents, JsonRoundTripEveryKind) {
    FlightRecorder rec;

    CombatEvent track;
    track.tick = 10; track.sim_time_s = 1.0;
    track.kind = CombatEventKind::TrackAcquired;
    track.subject_id = 3; track.object_id = 4;
    rec.record(track);

    CombatEvent rwr;
    rwr.tick = 20; rwr.sim_time_s = 2.0;
    rwr.kind = CombatEventKind::RwrLaunch;
    rwr.subject_id = 4; rwr.object_id = 3;
    rwr.range_ft = 78901.5;
    rec.record(rwr);

    rec.record(make_launch(30, 3.0, 3, 4, 7));

    CombatEvent det;
    det.tick = 40; det.sim_time_s = 40.0;
    det.kind = CombatEventKind::MissileDetonated;
    det.subject_id = 3; det.object_id = 4; det.missile_id = 7;
    det.end_cause = "target_hit";
    det.position = f4::geo::WorldPosition(500.0, 600.0, 9000.0);
    det.miss_distance_ft = 12.5;
    det.flight_time_s = 37.0;
    rec.record(det);

    CombatEvent dmg;
    dmg.tick = 41; dmg.sim_time_s = 40.05;
    dmg.kind = CombatEventKind::DamageApplied;
    dmg.subject_id = 4; dmg.object_id = 3; dmg.missile_id = 7;
    dmg.damage = 18.0;
    dmg.hit_points_after = 0.0;
    dmg.killed = true;
    rec.record(dmg);

    rec.record(make_kill(42, 40.1, 4, 3));

    const auto json = rec.to_json("roundtrip");
    const auto loaded = FlightRecorder::from_json(json);

    ASSERT_EQ(loaded.combat_event_count(), 6u);
    const auto& events = loaded.combat_events();

    // Track acquired
    EXPECT_EQ(events[0].kind, CombatEventKind::TrackAcquired);
    EXPECT_EQ(events[0].tick, 10u);
    EXPECT_EQ(events[0].sim_time_s, 1.0);
    EXPECT_EQ(events[0].subject_id, 3u);
    EXPECT_EQ(events[0].object_id, 4u);

    // RWR launch
    EXPECT_EQ(events[1].kind, CombatEventKind::RwrLaunch);
    EXPECT_DOUBLE_EQ(events[1].range_ft, 78901.5);
    EXPECT_EQ(events[1].subject_id, 4u);
    EXPECT_EQ(events[1].object_id, 3u);

    // Launch
    EXPECT_EQ(events[2].kind, CombatEventKind::MissileLaunched);
    EXPECT_EQ(events[2].missile_id, 7u);
    EXPECT_EQ(events[2].weapon_name, "AIM-120C");
    EXPECT_EQ(events[2].speed_ft_s, 900.0);
    EXPECT_DOUBLE_EQ(events[2].position.x, 100.0);
    EXPECT_DOUBLE_EQ(events[2].position.y, 200.0);
    EXPECT_DOUBLE_EQ(events[2].position.z, 10000.0);

    // Detonation
    EXPECT_EQ(events[3].kind, CombatEventKind::MissileDetonated);
    EXPECT_EQ(events[3].end_cause, "target_hit");
    EXPECT_DOUBLE_EQ(events[3].miss_distance_ft, 12.5);
    EXPECT_DOUBLE_EQ(events[3].flight_time_s, 37.0);
    EXPECT_DOUBLE_EQ(events[3].position.z, 9000.0);

    // Damage
    EXPECT_EQ(events[4].kind, CombatEventKind::DamageApplied);
    EXPECT_DOUBLE_EQ(events[4].damage, 18.0);
    EXPECT_DOUBLE_EQ(events[4].hit_points_after, 0.0);
    EXPECT_TRUE(events[4].killed);

    // Kill
    EXPECT_EQ(events[5].kind, CombatEventKind::EntityKilled);
    EXPECT_EQ(events[5].subject_id, 4u);
    EXPECT_EQ(events[5].object_id, 3u);
}

TEST(CombatEvents, UnknownKindNameStillParses) {
    // A newer writer adds a kind: the reader must not choke, the event
    // loads with the default kind (forward compatibility).
    const std::string json = R"({
  "format": "f4-flight-recording",
  "version": 1,
  "combat_events": [ { "kind": "laser_zap", "subject_id": 3,
                       "object_id": 4, "sim_time_s": 9.5 } ]
})";
    const auto loaded = FlightRecorder::from_json(json);
    ASSERT_EQ(loaded.combat_event_count(), 1u);
    EXPECT_EQ(loaded.combat_events()[0].sim_time_s, 9.5);
    EXPECT_EQ(loaded.combat_events()[0].subject_id, 3u);
}

// ============================================================================
// Missile-track snapshots
// ============================================================================

TEST(CombatEvents, MissileSnapshotRoundTrip) {
    FlightRecorder rec;
    rec.record(make_aircraft(0, 0.0, 3));

    FlightSnapshot m;
    m.missile = true;
    m.tick = 1; m.sim_time_s = 0.1;
    m.entity_id = 7;
    m.callsign = "AIM-120C";
    m.ai_mode = "Missile";
    m.ai_state = "guided";
    m.position = f4::geo::WorldPosition(150.0, 250.0, 10000.0);
    m.vt_fps = 2000.0;
    rec.record(m);

    const auto json = rec.to_json();
    // The missile key is present (the missile snapshot) — exactly once,
    // since the aircraft snapshot omits it.
    std::size_t missile_hits = 0;
    for (auto pos = json.find("\"missile\":true");
         pos != std::string::npos;
         pos = json.find("\"missile\": true", pos + 1)) {
        ++missile_hits;
    }
    EXPECT_EQ(missile_hits, 1u);

    const auto loaded = FlightRecorder::from_json(json);
    ASSERT_EQ(loaded.size(), 2u);
    EXPECT_FALSE(loaded.snapshots()[0].missile);  // aircraft default
    ASSERT_TRUE(loaded.snapshots()[1].missile);
    EXPECT_EQ(loaded.snapshots()[1].callsign, "AIM-120C");
    EXPECT_EQ(loaded.snapshots()[1].ai_state, "guided");
    EXPECT_DOUBLE_EQ(loaded.snapshots()[1].vt_fps, 2000.0);
}

TEST(CombatEvents, MissileKeyOmittedForAircraft) {
    // Byte-compatibility: aircraft-only recordings must not carry the key.
    FlightRecorder rec;
    rec.record(make_aircraft(0, 0.0, 3));
    const auto json = rec.to_json();
    EXPECT_EQ(json.find("\"missile\""), std::string::npos);
    EXPECT_EQ(json.find("combat_events"), std::string::npos);
    EXPECT_EQ(json.find("combat_event_count"), std::string::npos);
}

// ============================================================================
// Old-format compatibility
// ============================================================================

TEST(CombatEvents, OldFormatDocLoads) {
    // A pre-M4 document (hand-written, no combat arrays, no missile keys):
    // must load with zero events and no missile flags.
    const std::string old_doc = R"({
  "format": "f4-flight-recording",
  "version": 1,
  "scenario": "old_school",
  "snapshot_count": 1,
  "snapshots": [ {
    "tick": 5, "sim_time_s": 0.5, "entity_id": 3, "callsign": "VIPER1",
    "position": { "x": 1, "y": 2, "z": 3 },
    "ai_mode": "NavigationMode", "ai_state": "Enroute"
  } ]
})";
    const auto loaded = FlightRecorder::from_json(old_doc);
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded.combat_event_count(), 0u);
    EXPECT_FALSE(loaded.snapshots()[0].missile);
    EXPECT_EQ(loaded.scenario_name(), "old_school");
}

// ============================================================================
// Summary: the combat debrief
// ============================================================================

TEST(CombatEvents, SummaryCombatDebrief) {
    FlightRecorder rec;
    rec.record(make_aircraft(0, 0.0, 3));
    rec.record(make_aircraft(0, 0.0, 4));

    rec.record(make_launch(60, 6.0, 3, 4, 7));   // launch at t=6

    CombatEvent det;
    det.tick = 2400; det.sim_time_s = 40.0;
    det.kind = CombatEventKind::MissileDetonated;
    det.subject_id = 3; det.object_id = 4; det.missile_id = 7;
    det.end_cause = "target_hit";
    det.miss_distance_ft = 0.0;
    det.flight_time_s = 34.0;
    rec.record(det);

    CombatEvent dmg;
    dmg.tick = 2401; dmg.sim_time_s = 40.05;
    dmg.kind = CombatEventKind::DamageApplied;
    dmg.subject_id = 4; dmg.object_id = 3; dmg.missile_id = 7;
    dmg.damage = 18.0; dmg.hit_points_after = 0.0; dmg.killed = true;
    rec.record(dmg);

    rec.record(make_kill(2402, 40.1, 4, 3));

    const auto summary = rec.to_summary_json("fight");

    // The combat section exists with the engagement window + counts.
    EXPECT_NE(summary.find("\"combat\""), std::string::npos);
    EXPECT_NE(summary.find("\"event_count\":4"), std::string::npos);
    EXPECT_NE(summary.find("\"first_event_s\":6"), std::string::npos);
    EXPECT_NE(summary.find("\"last_event_s\":40.1"), std::string::npos);

    // The launch entry carries the outcome correlation.
    EXPECT_NE(summary.find("\"weapon\":\"AIM-120C\""), std::string::npos);
    EXPECT_NE(summary.find("\"end_cause\":\"target_hit\""), std::string::npos);
    EXPECT_NE(summary.find("\"flight_time_s\":34"), std::string::npos);

    // The kill entry names the victim, killer, and the weapon.
    EXPECT_NE(summary.find("\"killer_id\":3"), std::string::npos);
    EXPECT_NE(summary.find("\"target_id\":4"), std::string::npos);
}

TEST(CombatEvents, SummaryWithoutCombatHasNoCombatSection) {
    FlightRecorder rec;
    rec.record(make_aircraft(0, 0.0, 3));
    const auto summary = rec.to_summary_json();
    EXPECT_EQ(summary.find("\"combat\""), std::string::npos);
}

TEST(CombatEvents, SummaryExcludesMissilesFromAircraftSection) {
    FlightRecorder rec;
    rec.record(make_aircraft(0, 0.0, 3));       // one aircraft
    rec.record(make_aircraft(1, 0.1, 3));

    FlightSnapshot m;
    m.missile = true;
    m.tick = 2; m.sim_time_s = 0.2;
    m.entity_id = 7;
    m.callsign = "AIM-120C";
    m.ai_mode = "Missile"; m.ai_state = "guided";
    rec.record(m);
    rec.record(m);

    const auto summary = rec.to_summary_json();
    // The missile never appears as an "aircraft" entry (its id/callsign
    // must not show up in the aircraft array), and its "Missile:guided"
    // state never enters the state sequence.
    EXPECT_EQ(summary.find("\"entity_id\":7"), std::string::npos);
    EXPECT_EQ(summary.find("Missile:guided"), std::string::npos);
    EXPECT_NE(summary.find("\"entity_id\":3"), std::string::npos);
}
