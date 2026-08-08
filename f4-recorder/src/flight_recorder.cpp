// f4-recorder/src/flight_recorder.cpp
//
// FlightRecorder implementation — JSON serialization and queries.

#include "f4/recorder/flight_recorder.hpp"
#include "f4/json/writer.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace f4::recorder {

// ============================================================================
// Queries
// ============================================================================

std::vector<FlightSnapshot> FlightRecorder::snapshots_for(
    std::uint64_t entity_id) const
{
    std::vector<FlightSnapshot> result;
    for (const auto& s : snapshots_) {
        if (s.entity_id == entity_id) {
            result.push_back(s);
        }
    }
    return result;
}

std::vector<FlightSnapshot> FlightRecorder::snapshots_in_range(
    double t0_s, double t1_s) const
{
    // Floating-point tolerance (1 µs) handles cases like 7 * 0.1 ≠ 0.7
    // in double precision. Without this, boundary snapshots can be
    // spuriously excluded due to rounding in the time computation.
    constexpr double eps = 1e-6;
    std::vector<FlightSnapshot> result;
    for (const auto& s : snapshots_) {
        if (s.sim_time_s >= t0_s - eps && s.sim_time_s <= t1_s + eps) {
            result.push_back(s);
        }
    }
    return result;
}

// ============================================================================
// JSON export (full trace)
// ============================================================================

std::string FlightRecorder::to_json(const std::string& scenario_name) const {
    json::Writer w;
    w.raw("{\n");

    // Header
    w.string("format"); w.raw(":"); w.string("f4-flight-recording"); w.raw(",\n");
    w.string("version"); w.raw(":"); w.number(1); w.raw(",\n");
    w.string("scenario"); w.raw(":"); w.string(scenario_name.empty() ? scenario_name_ : scenario_name); w.raw(",\n");
    w.string("snapshot_count"); w.raw(":"); w.number(static_cast<std::uint64_t>(snapshots_.size())); w.raw(",\n");

    // Snapshots array
    w.string("snapshots"); w.raw(": [\n");
    for (std::size_t i = 0; i < snapshots_.size(); ++i) {
        const auto& s = snapshots_[i];
        w.raw("  {\n");

        // Timing
        w.raw("    "); w.string("tick"); w.raw(":"); w.number(s.tick); w.raw(",\n");
        w.raw("    "); w.string("sim_time_s"); w.raw(":"); w.number(s.sim_time_s); w.raw(",\n");

        // Identity
        w.raw("    "); w.string("entity_id"); w.raw(":"); w.number(s.entity_id); w.raw(",\n");
        w.raw("    "); w.string("callsign"); w.raw(":"); w.string(s.callsign); w.raw(",\n");

        // Position
        w.raw("    "); w.string("position"); w.raw(": { ");
        w.string("x"); w.raw(":"); w.number(s.position.x); w.raw(", ");
        w.string("y"); w.raw(":"); w.number(s.position.y); w.raw(", ");
        w.string("z"); w.raw(":"); w.number(s.position.z); w.raw(" },\n");

        // Attitude
        w.raw("    "); w.string("heading_rad"); w.raw(":"); w.number(s.heading_rad); w.raw(",\n");
        w.raw("    "); w.string("pitch_rad"); w.raw(":"); w.number(s.pitch_rad); w.raw(",\n");
        w.raw("    "); w.string("roll_rad"); w.raw(":"); w.number(s.roll_rad); w.raw(",\n");

        // Airspeed / altitude
        w.raw("    "); w.string("altitude_agl_ft"); w.raw(":"); w.number(s.altitude_agl_ft); w.raw(",\n");
        w.raw("    "); w.string("altitude_msl_ft"); w.raw(":"); w.number(s.altitude_msl_ft); w.raw(",\n");
        w.raw("    "); w.string("vcas_kts"); w.raw(":"); w.number(s.vcas_kts); w.raw(",\n");
        w.raw("    "); w.string("gs_kts"); w.raw(":"); w.number(s.gs_kts); w.raw(",\n");
        w.raw("    "); w.string("vt_fps"); w.raw(":"); w.number(s.vt_fps); w.raw(",\n");
        w.raw("    "); w.string("mach"); w.raw(":"); w.number(s.mach); w.raw(",\n");

        // Control inputs
        w.raw("    "); w.string("pitch_cmd"); w.raw(":"); w.number(s.pitch_cmd); w.raw(",\n");
        w.raw("    "); w.string("roll_cmd"); w.raw(":"); w.number(s.roll_cmd); w.raw(",\n");
        w.raw("    "); w.string("yaw_cmd"); w.raw(":"); w.number(s.yaw_cmd); w.raw(",\n");
        w.raw("    "); w.string("throttle_cmd"); w.raw(":"); w.number(s.throttle_cmd); w.raw(",\n");
        w.raw("    "); w.string("speed_brake_cmd"); w.raw(":"); w.number(s.speed_brake_cmd); w.raw(",\n");
        w.raw("    "); w.string("gear_handle_down"); w.raw(":"); w.raw(s.gear_handle_down ? "true" : "false"); w.raw(",\n");
        w.raw("    "); w.string("wheel_brakes"); w.raw(":"); w.raw(s.wheel_brakes ? "true" : "false"); w.raw(",\n");
        w.raw("    "); w.string("nose_steer_on"); w.raw(":"); w.raw(s.nose_steer_on ? "true" : "false"); w.raw(",\n");

        // AI brain state
        w.raw("    "); w.string("ai_mode"); w.raw(":"); w.string(s.ai_mode); w.raw(",\n");
        w.raw("    "); w.string("ai_state"); w.raw(":"); w.string(s.ai_state); w.raw(",\n");
        w.raw("    "); w.string("ai_event"); w.raw(":"); w.string(s.ai_event); w.raw(",\n");
        w.raw("    "); w.string("ai_guard_result"); w.raw(":"); w.string(s.ai_guard_result); w.raw(",\n");

        // Intended path
        w.raw("    "); w.string("target_position"); w.raw(": { ");
        w.string("x"); w.raw(":"); w.number(s.target_position.x); w.raw(", ");
        w.string("y"); w.raw(":"); w.number(s.target_position.y); w.raw(", ");
        w.string("z"); w.raw(":"); w.number(s.target_position.z); w.raw(" },\n");
        w.raw("    "); w.string("target_description"); w.raw(":"); w.string(s.target_description); w.raw(",\n");
        w.raw("    "); w.string("cross_track_error_ft"); w.raw(":"); w.number(s.cross_track_error_ft); w.raw(",\n");
        w.raw("    "); w.string("along_track_error_ft"); w.raw(":"); w.number(s.along_track_error_ft); w.raw(",\n");
        w.raw("    "); w.string("vertical_error_ft"); w.raw(":"); w.number(s.vertical_error_ft); w.raw(",\n");

        // Ground state
        w.raw("    "); w.string("on_ground"); w.raw(":"); w.raw(s.on_ground ? "true" : "false"); w.raw(",\n");
        w.raw("    "); w.string("ground_speed_kts"); w.raw(":"); w.number(s.ground_speed_kts); w.raw(",\n");

        // Engine
        w.raw("    "); w.string("engine_rpm"); w.raw(":"); w.number(s.engine_rpm); w.raw(",\n");
        w.raw("    "); w.string("afterburner_lit"); w.raw(":"); w.raw(s.afterburner_lit ? "true" : "false"); w.raw(",\n");
        w.raw("    "); w.string("fuel_lbs"); w.raw(":"); w.number(s.fuel_lbs); w.raw(",\n");

        // G-loads
        w.raw("    "); w.string("nz"); w.raw(":"); w.number(s.nz); w.raw(",\n");
        w.raw("    "); w.string("nx"); w.raw(":"); w.number(s.nx); w.raw("\n");

        w.raw("  }");
        if (i + 1 < snapshots_.size()) w.raw(",");
        w.raw("\n");
    }
    w.raw("]\n");
    w.raw("}\n");

    return w.str();
}

// ============================================================================
// JSON export (LLM-friendly summary)
// ============================================================================

std::string FlightRecorder::to_summary_json(
    const std::string& scenario_name,
    double cross_track_tolerance_ft,
    double vertical_tolerance_ft) const
{
    json::Writer w;
    w.raw("{\n");

    w.string("format"); w.raw(":"); w.string("f4-flight-summary"); w.raw(",\n");
    w.string("version"); w.raw(":"); w.number(1); w.raw(",\n");
    w.string("scenario"); w.raw(":"); w.string(scenario_name.empty() ? scenario_name_ : scenario_name); w.raw(",\n");

    // Identify unique aircraft
    std::vector<std::uint64_t> entity_ids;
    for (const auto& s : snapshots_) {
        if (std::find(entity_ids.begin(), entity_ids.end(), s.entity_id) == entity_ids.end()) {
            entity_ids.push_back(s.entity_id);
        }
    }

    // Aircraft info
    w.string("aircraft"); w.raw(": [\n");
    for (std::size_t ei = 0; ei < entity_ids.size(); ++ei) {
        auto ac_snaps = snapshots_for(entity_ids[ei]);
        if (ac_snaps.empty()) continue;

        w.raw("  {\n");
        w.raw("    "); w.string("entity_id"); w.raw(":"); w.number(ac_snaps[0].entity_id); w.raw(",\n");
        w.raw("    "); w.string("callsign"); w.raw(":"); w.string(ac_snaps[0].callsign); w.raw(",\n");
        w.raw("    "); w.string("snapshot_count"); w.raw(":"); w.number(static_cast<std::uint64_t>(ac_snaps.size())); w.raw(",\n");

        if (!ac_snaps.empty()) {
            w.raw("    "); w.string("duration_s"); w.raw(":");
            w.number(ac_snaps.back().sim_time_s - ac_snaps.front().sim_time_s); w.raw(",\n");
        }

        // Phase summary: group contiguous ticks by (ai_mode, ai_state)
        w.raw("    "); w.string("phases"); w.raw(": [\n");
        std::size_t phase_start = 0;
        std::size_t phase_count = 0;
        for (std::size_t i = 0; i <= ac_snaps.size(); ++i) {
            bool same_phase = (i < ac_snaps.size() && i > 0 &&
                ac_snaps[i].ai_mode == ac_snaps[phase_start].ai_mode &&
                ac_snaps[i].ai_state == ac_snaps[phase_start].ai_state);

            if (!same_phase && phase_start < ac_snaps.size()) {
                // Emit the completed phase
                const auto& first = ac_snaps[phase_start];
                const auto& last = ac_snaps[i - 1];
                double duration = last.sim_time_s - first.sim_time_s;

                // Compute max cross-track and vertical error for this phase
                double max_xte = 0.0, max_ve = 0.0;
                for (std::size_t j = phase_start; j < i; ++j) {
                    max_xte = std::max(max_xte, std::abs(ac_snaps[j].cross_track_error_ft));
                    max_ve = std::max(max_ve, std::abs(ac_snaps[j].vertical_error_ft));
                }

                w.raw("      {\n");
                w.raw("        "); w.string("mode"); w.raw(":"); w.string(first.ai_mode); w.raw(",\n");
                w.raw("        "); w.string("state"); w.raw(":"); w.string(first.ai_state); w.raw(",\n");
                w.raw("        "); w.string("tick_range"); w.raw(": [");
                w.number(first.tick); w.raw(", "); w.number(last.tick); w.raw("],\n");
                w.raw("        "); w.string("duration_s"); w.raw(":"); w.number(duration); w.raw(",\n");
                w.raw("        "); w.string("max_cross_track_error_ft"); w.raw(":"); w.number(max_xte); w.raw(",\n");
                w.raw("        "); w.string("max_vertical_error_ft"); w.raw(":"); w.number(max_ve); w.raw(",\n");
                w.raw("        "); w.string("target"); w.raw(":"); w.string(first.target_description); w.raw("\n");
                w.raw("      }");
                if (i < ac_snaps.size()) w.raw(",");
                w.raw("\n");

                phase_start = i;
                ++phase_count;
            }
        }
        w.raw("    ],\n");

        // Anomalies
        w.raw("    "); w.string("anomalies"); w.raw(": [\n");
        bool first_anomaly = true;
        for (std::size_t i = 0; i < ac_snaps.size(); ++i) {
            const auto& s = ac_snaps[i];
            double xte = std::abs(s.cross_track_error_ft);
            double ve = std::abs(s.vertical_error_ft);
            if (xte > cross_track_tolerance_ft || ve > vertical_tolerance_ft) {
                if (!first_anomaly) w.raw(",\n");
                w.raw("      {\n");
                w.raw("        "); w.string("tick"); w.raw(":"); w.number(s.tick); w.raw(",\n");
                w.raw("        "); w.string("type"); w.raw(":"); w.string("path_deviation"); w.raw(",\n");
                w.raw("        "); w.string("cross_track_error_ft"); w.raw(":"); w.number(s.cross_track_error_ft); w.raw(",\n");
                w.raw("        "); w.string("vertical_error_ft"); w.raw(":"); w.number(s.vertical_error_ft); w.raw(",\n");
                w.raw("        "); w.string("ai_state"); w.raw(":"); w.string(s.ai_state); w.raw(",\n");
                w.raw("        "); w.string("target"); w.raw(":"); w.string(s.target_description); w.raw("\n");
                w.raw("      }");
                first_anomaly = false;
            }
        }
        if (!first_anomaly) w.raw("\n");
        w.raw("    ]\n");

        w.raw("  }");
        if (ei + 1 < entity_ids.size()) w.raw(",");
        w.raw("\n");
    }
    w.raw("],\n");

    // Trace summary
    w.string("trace_summary"); w.raw(": {\n");
    if (!snapshots_.empty()) {
        // Collect the sequence of (mode, state) changes
        std::vector<std::string> state_seq;
        state_seq.push_back(snapshots_[0].ai_mode + ":" + snapshots_[0].ai_state);
        for (std::size_t i = 1; i < snapshots_.size(); ++i) {
            std::string current = snapshots_[i].ai_mode + ":" + snapshots_[i].ai_state;
            if (current != state_seq.back()) {
                state_seq.push_back(current);
            }
        }

        w.raw("  "); w.string("state_sequence"); w.raw(": [\n");
        for (std::size_t i = 0; i < state_seq.size(); ++i) {
            w.raw("    "); w.string(state_seq[i]);
            if (i + 1 < state_seq.size()) w.raw(",");
            w.raw("\n");
        }
        w.raw("  ],\n");
        w.raw("  "); w.string("unique_states"); w.raw(":"); w.number(static_cast<std::uint64_t>(state_seq.size())); w.raw("\n");
    }
    w.raw("}\n");

    w.raw("}\n");
    return w.str();
}

// ============================================================================
// JSON import (from_json)
// ============================================================================
// Minimal parser — reads the format produced by to_json(). Uses f4-json Reader
// is not needed here since we control both ends; instead we parse the known
// structure directly for efficiency.
//
// For now, from_json is a placeholder that will be implemented when the
// replay viewer needs it. The write/load round-trip tests will drive the
// implementation.

FlightRecorder FlightRecorder::from_json(const std::string& /*json_str*/) {
    // TODO: Implement when replay viewer needs loading.
    // The JSON format is fully specified above; parsing is straightforward.
    return FlightRecorder{};
}

// ============================================================================
// File I/O
// ============================================================================

void FlightRecorder::write_json(
    const std::filesystem::path& path,
    const std::string& scenario_name) const
{
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        throw std::runtime_error("Cannot open file for writing: " + path.string());
    }
    const auto content = to_json(scenario_name);
    f.write(content.data(), content.size());
}

void FlightRecorder::write_summary_json(
    const std::filesystem::path& path,
    const std::string& scenario_name,
    double cross_track_tolerance_ft,
    double vertical_tolerance_ft) const
{
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        throw std::runtime_error("Cannot open file for writing: " + path.string());
    }
    const auto content = to_summary_json(scenario_name,
                                          cross_track_tolerance_ft,
                                          vertical_tolerance_ft);
    f.write(content.data(), content.size());
}

FlightRecorder FlightRecorder::load_json(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        throw std::runtime_error("Cannot open file for reading: " + path.string());
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return from_json(ss.str());
}

} // namespace f4::recorder
