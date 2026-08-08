// f4-recorder/tests/test_flight_recorder.cpp
//
// Unit tests for FlightRecorder — recording, queries, and JSON export.

#include <gtest/gtest.h>

#include <f4/recorder/flight_recorder.hpp>

using namespace f4::recorder;

namespace {

FlightSnapshot make_test_snapshot(
    std::uint64_t tick, double time_s,
    std::uint64_t entity_id = 1,
    const std::string& mode = "TakeoffMode",
    const std::string& state = "Taxi")
{
    FlightSnapshot s;
    s.tick = tick;
    s.sim_time_s = time_s;
    s.entity_id = entity_id;
    s.callsign = "Viper1";
    s.position = f4::geo::WorldPosition(100.0, 200.0, 0.0);
    s.heading_rad = 0.0;
    s.vcas_kts = 15.0;
    s.altitude_agl_ft = 0.0;
    s.ai_mode = mode;
    s.ai_state = state;
    s.throttle_cmd = 0.1;
    s.gear_handle_down = true;
    s.on_ground = true;
    return s;
}

} // anonymous namespace

// ============================================================================
// Basic recording
// ============================================================================

TEST(FlightRecorder, EmptyOnConstruction) {
    FlightRecorder rec;
    EXPECT_TRUE(rec.empty());
    EXPECT_EQ(rec.size(), 0u);
}

TEST(FlightRecorder, RecordAndSize) {
    FlightRecorder rec;
    rec.record(make_test_snapshot(0, 0.0));
    rec.record(make_test_snapshot(1, 0.1));
    rec.record(make_test_snapshot(2, 0.2));

    EXPECT_FALSE(rec.empty());
    EXPECT_EQ(rec.size(), 3u);
}

TEST(FlightRecorder, RecordMove) {
    FlightRecorder rec;
    FlightSnapshot s = make_test_snapshot(0, 0.0);
    rec.record(std::move(s));

    EXPECT_EQ(rec.size(), 1u);
}

// ============================================================================
// Queries
// ============================================================================

TEST(FlightRecorder, SnapshotsForEntityId) {
    FlightRecorder rec;
    rec.record(make_test_snapshot(0, 0.0, /*entity_id=*/1));
    rec.record(make_test_snapshot(1, 0.1, /*entity_id=*/2));
    rec.record(make_test_snapshot(2, 0.2, /*entity_id=*/1));
    rec.record(make_test_snapshot(3, 0.3, /*entity_id=*/2));

    auto for_1 = rec.snapshots_for(1);
    auto for_2 = rec.snapshots_for(2);
    auto for_3 = rec.snapshots_for(3);

    EXPECT_EQ(for_1.size(), 2u);
    EXPECT_EQ(for_2.size(), 2u);
    EXPECT_EQ(for_3.size(), 0u);

    EXPECT_EQ(for_1[0].tick, 0u);
    EXPECT_EQ(for_1[1].tick, 2u);
}

TEST(FlightRecorder, SnapshotsInRange) {
    FlightRecorder rec;
    for (int i = 0; i < 10; ++i) {
        rec.record(make_test_snapshot(i, i * 0.1));
    }

    auto in_range = rec.snapshots_in_range(0.3, 0.7);
    EXPECT_EQ(in_range.size(), 5u);  // ticks 3,4,5,6,7
    EXPECT_EQ(in_range[0].tick, 3u);
    EXPECT_EQ(in_range[4].tick, 7u);
}

// ============================================================================
// JSON export (full trace)
// ============================================================================

TEST(FlightRecorder, ToJsonContainsHeader) {
    FlightRecorder rec;
    rec.record(make_test_snapshot(0, 0.0));

    std::string json = rec.to_json("test_takeoff");

    EXPECT_NE(json.find("\"format\":\"f4-flight-recording\""), std::string::npos);
    EXPECT_NE(json.find("\"version\":1"), std::string::npos);
    EXPECT_NE(json.find("\"scenario\":\"test_takeoff\""), std::string::npos);
    EXPECT_NE(json.find("\"snapshot_count\":1"), std::string::npos);
}

TEST(FlightRecorder, ToJsonContainsSnapshotData) {
    FlightRecorder rec;
    auto snap = make_test_snapshot(42, 4.2, 1, "TakeoffMode", "TakeRunway");
    snap.vcas_kts = 120.0;
    snap.throttle_cmd = 1.0;
    rec.record(snap);

    std::string json = rec.to_json();

    EXPECT_NE(json.find("\"tick\":42"), std::string::npos);
    EXPECT_NE(json.find("\"sim_time_s\":"), std::string::npos);
    EXPECT_NE(json.find("\"TakeoffMode\""), std::string::npos);
    EXPECT_NE(json.find("\"TakeRunway\""), std::string::npos);
}

TEST(FlightRecorder, ToJsonMultipleSnapshots) {
    FlightRecorder rec;
    for (int i = 0; i < 5; ++i) {
        rec.record(make_test_snapshot(i, i * 0.1));
    }

    std::string json = rec.to_json();
    EXPECT_NE(json.find("\"snapshot_count\":5"), std::string::npos);
}

// ============================================================================
// JSON export (summary)
// ============================================================================

TEST(FlightRecorder, SummaryJsonContainsPhases) {
    FlightRecorder rec;

    // Phase 1: Taxi (ticks 0-9)
    for (int i = 0; i < 10; ++i) {
        rec.record(make_test_snapshot(i, i * 0.1, 1, "TakeoffMode", "Taxi"));
    }
    // Phase 2: TakeRunway (ticks 10-19)
    for (int i = 10; i < 20; ++i) {
        rec.record(make_test_snapshot(i, i * 0.1, 1, "TakeoffMode", "TakeRunway"));
    }

    std::string json = rec.to_summary_json("test_phases");

    EXPECT_NE(json.find("\"format\":\"f4-flight-summary\""), std::string::npos);
    EXPECT_NE(json.find("\"TakeoffMode\""), std::string::npos);  // mode appears
    EXPECT_NE(json.find("\"Taxi\""), std::string::npos);
    EXPECT_NE(json.find("\"TakeRunway\""), std::string::npos);
}

TEST(FlightRecorder, SummaryJsonDetectsAnomalies) {
    FlightRecorder rec;

    // Normal flight for 5 ticks
    for (int i = 0; i < 5; ++i) {
        auto s = make_test_snapshot(i, i * 0.1);
        s.cross_track_error_ft = 10.0;  // within tolerance
        rec.record(s);
    }
    // Anomalous tick
    auto bad = make_test_snapshot(5, 0.5);
    bad.cross_track_error_ft = 200.0;  // exceeds 100ft tolerance
    rec.record(bad);

    std::string json = rec.to_summary_json("test_anomaly", /*cross_track_tol=*/100.0);

    EXPECT_NE(json.find("\"type\":\"path_deviation\""), std::string::npos);
    EXPECT_NE(json.find("\"tick\":5"), std::string::npos);
}

// ============================================================================
// Scenario name
// ============================================================================

TEST(FlightRecorder, ScenarioName) {
    FlightRecorder rec;
    rec.set_scenario_name("takeoff_kunsan_rwy36l");
    EXPECT_EQ(rec.scenario_name(), "takeoff_kunsan_rwy36l");

    // to_json uses scenario_name_ when no override is given
    rec.record(make_test_snapshot(0, 0.0));
    std::string json = rec.to_json();
    EXPECT_NE(json.find("\"scenario\":\"takeoff_kunsan_rwy36l\""), std::string::npos);
}

// ============================================================================
// File I/O (write_json)
// ============================================================================

TEST(FlightRecorder, WriteJsonProducesNonEmptyFile) {
    FlightRecorder rec;
    rec.record(make_test_snapshot(0, 0.0));
    rec.record(make_test_snapshot(1, 0.1));

    // Write to a temp file
    const auto path = std::filesystem::temp_directory_path() / "test_flight_recording.json";
    ASSERT_NO_THROW(rec.write_json(path));

    // Verify the file exists and is non-empty
    ASSERT_TRUE(std::filesystem::exists(path));
    EXPECT_GT(std::filesystem::file_size(path), 0u);

    // Cleanup
    std::filesystem::remove(path);
}

// ============================================================================
// Snapshot field coverage
// ============================================================================

TEST(FlightSnapshot, DegreeAccessors) {
    FlightSnapshot s;
    s.heading_rad = 1.5707963267948966;  // 90 degrees
    s.pitch_rad = 0.0;
    s.roll_rad = -0.5235987755982988;  // -30 degrees

    EXPECT_NEAR(s.heading_deg(), 90.0, 0.001);
    EXPECT_NEAR(s.pitch_deg(), 0.0, 0.001);
    EXPECT_NEAR(s.roll_deg(), -30.0, 0.001);
}

// ============================================================================
// Round-trip tests: to_json → from_json
// ============================================================================

TEST(RoundTrip, EmptyRecorder) {
    FlightRecorder rec;
    std::string json = rec.to_json("empty_scenario");
    FlightRecorder rec2 = FlightRecorder::from_json(json);
    EXPECT_TRUE(rec2.empty());
    EXPECT_EQ(rec2.size(), 0u);
}

TEST(RoundTrip, SingleSnapshot) {
    FlightRecorder rec;
    FlightSnapshot s;
    s.tick = 42;
    s.sim_time_s = 4.2;
    s.entity_id = 7;
    s.callsign = "Viper1";
    s.position = f4::geo::WorldPosition(100.0, 200.0, 300.0);
    s.heading_rad = 1.57;
    s.pitch_rad = 0.05;
    s.roll_rad = -0.3;
    s.altitude_agl_ft = 5000.0;
    s.altitude_msl_ft = 5020.0;
    s.vcas_kts = 250.0;
    s.gs_kts = 245.0;
    s.vt_fps = 420.0;
    s.mach = 0.4;
    s.pitch_cmd = 0.2;
    s.roll_cmd = -0.1;
    s.yaw_cmd = 0.0;
    s.throttle_cmd = 0.85;
    s.speed_brake_cmd = -1.0;
    s.gear_handle_down = false;
    s.wheel_brakes = false;
    s.nose_steer_on = true;
    s.ai_mode = "CruiseMode";
    s.ai_state = "Tracking";
    s.ai_event = "wp_reached";
    s.ai_guard_result = "PASS: range<2nm";
    s.target_position = f4::geo::WorldPosition(500.0, 600.0, 700.0);
    s.target_description = "WP3";
    s.cross_track_error_ft = 12.5;
    s.along_track_error_ft = 3400.0;
    s.vertical_error_ft = -3.2;
    s.on_ground = false;
    s.ground_speed_kts = 240.0;
    s.engine_rpm = 0.92;
    s.afterburner_lit = false;
    s.fuel_lbs = 5000.0;
    s.nz = 1.0;
    s.nx = 0.01;
    rec.record(s);

    std::string json = rec.to_json("single_test");
    FlightRecorder rec2 = FlightRecorder::from_json(json);

    ASSERT_EQ(rec2.size(), 1u);
    const auto& s2 = rec2.snapshots()[0];

    EXPECT_EQ(s2.tick, 42u);
    EXPECT_NEAR(s2.sim_time_s, 4.2, 1e-9);
    EXPECT_EQ(s2.entity_id, 7u);
    EXPECT_EQ(s2.callsign, "Viper1");
    EXPECT_NEAR(s2.position.x, 100.0, 1e-9);
    EXPECT_NEAR(s2.position.y, 200.0, 1e-9);
    EXPECT_NEAR(s2.position.z, 300.0, 1e-9);
    EXPECT_NEAR(s2.heading_rad, 1.57, 1e-9);
    EXPECT_NEAR(s2.vcas_kts, 250.0, 1e-9);
    EXPECT_NEAR(s2.altitude_agl_ft, 5000.0, 1e-9);
    EXPECT_NEAR(s2.pitch_cmd, 0.2, 1e-9);
    EXPECT_NEAR(s2.throttle_cmd, 0.85, 1e-9);
    EXPECT_EQ(s2.gear_handle_down, false);
    EXPECT_EQ(s2.ai_mode, "CruiseMode");
    EXPECT_EQ(s2.ai_state, "Tracking");
    EXPECT_NEAR(s2.target_position.x, 500.0, 1e-9);
    EXPECT_NEAR(s2.target_position.y, 600.0, 1e-9);
    EXPECT_NEAR(s2.target_position.z, 700.0, 1e-9);
    EXPECT_EQ(s2.on_ground, false);
    EXPECT_NEAR(s2.engine_rpm, 0.92, 1e-9);
    EXPECT_NEAR(s2.nz, 1.0, 1e-9);
    EXPECT_NEAR(s2.nx, 0.01, 1e-9);
}

TEST(RoundTrip, MultipleSnapshots) {
    FlightRecorder rec;
    for (int i = 0; i < 5; ++i) {
        auto s = make_test_snapshot(i, i * 0.1);
        s.vcas_kts = 100.0 + i * 10.0;
        rec.record(s);
    }

    std::string json = rec.to_json();
    FlightRecorder rec2 = FlightRecorder::from_json(json);

    ASSERT_EQ(rec2.size(), 5u);
    EXPECT_EQ(rec2.snapshots()[0].tick, 0u);
    EXPECT_EQ(rec2.snapshots()[4].tick, 4u);
    EXPECT_NEAR(rec2.snapshots()[2].vcas_kts, 120.0, 1e-9);
}

TEST(RoundTrip, ScenarioNamePreserved) {
    FlightRecorder rec;
    rec.set_scenario_name("takeoff_kunsan_rwy36l");
    rec.record(make_test_snapshot(0, 0.0));

    std::string json = rec.to_json();
    FlightRecorder rec2 = FlightRecorder::from_json(json);

    EXPECT_EQ(rec2.scenario_name(), "takeoff_kunsan_rwy36l");
}

TEST(RoundTrip, MultiEntity) {
    FlightRecorder rec;
    for (int i = 0; i < 3; ++i) {
        auto s = make_test_snapshot(i, i * 0.1, /*entity_id=*/1);
        rec.record(s);
    }
    for (int i = 0; i < 2; ++i) {
        auto s = make_test_snapshot(i + 10, (i + 10) * 0.1, /*entity_id=*/2);
        s.callsign = "Eagle1";
        rec.record(s);
    }

    std::string json = rec.to_json();
    FlightRecorder rec2 = FlightRecorder::from_json(json);

    ASSERT_EQ(rec2.size(), 5u);
    auto for_1 = rec2.snapshots_for(1);
    auto for_2 = rec2.snapshots_for(2);
    EXPECT_EQ(for_1.size(), 3u);
    EXPECT_EQ(for_2.size(), 2u);
    EXPECT_EQ(for_2[0].callsign, "Eagle1");
}

TEST(RoundTrip, AllBooleanFields) {
    FlightRecorder rec;
    FlightSnapshot s;
    s.tick = 0;
    s.sim_time_s = 0.0;
    s.gear_handle_down = true;
    s.wheel_brakes = true;
    s.nose_steer_on = true;
    s.on_ground = true;
    s.afterburner_lit = true;
    rec.record(s);

    std::string json = rec.to_json();
    FlightRecorder rec2 = FlightRecorder::from_json(json);

    ASSERT_EQ(rec2.size(), 1u);
    const auto& s2 = rec2.snapshots()[0];
    EXPECT_TRUE(s2.gear_handle_down);
    EXPECT_TRUE(s2.wheel_brakes);
    EXPECT_TRUE(s2.nose_steer_on);
    EXPECT_TRUE(s2.on_ground);
    EXPECT_TRUE(s2.afterburner_lit);
}

TEST(RoundTrip, FileRoundTrip) {
    FlightRecorder rec;
    rec.set_scenario_name("file_test");
    auto s = make_test_snapshot(0, 0.0);
    s.vcas_kts = 300.0;
    s.entity_id = 99;
    rec.record(s);

    const auto path = std::filesystem::temp_directory_path() / "test_roundtrip.json";
    ASSERT_NO_THROW(rec.write_json(path));
    FlightRecorder rec2 = FlightRecorder::load_json(path);

    ASSERT_EQ(rec2.size(), 1u);
    EXPECT_EQ(rec2.scenario_name(), "file_test");
    EXPECT_NEAR(rec2.snapshots()[0].vcas_kts, 300.0, 1e-9);
    EXPECT_EQ(rec2.snapshots()[0].entity_id, 99u);

    // Cleanup
    std::filesystem::remove(path);
}
