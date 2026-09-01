// f4-recorder/src/flight_recorder.cpp
//
// FlightRecorder implementation — JSON serialization and queries.

#include "f4/recorder/flight_recorder.hpp"
#include "f4/json/writer.hpp"
#include "f4/json/reader.hpp"

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

std::vector<CombatEvent> FlightRecorder::combat_events_in_range(
    double t0_s, double t1_s) const
{
    constexpr double eps = 1e-6;  // same tolerance as snapshots_in_range
    std::vector<CombatEvent> result;
    for (const auto& e : combat_events_) {
        if (e.sim_time_s >= t0_s - eps && e.sim_time_s <= t1_s + eps) {
            result.push_back(e);
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
    // Combat events are appended AFTER the snapshots array (and their
    // count emitted with the array, not the header) so recordings without
    // combat end exactly where the pre-M4 format ended — byte-identical
    // output for diff-based regression baselines.

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
        w.raw("    "); w.string("nx"); w.raw(":"); w.number(s.nx);

        // Track type: missile snapshots carry the flag; aircraft snapshots
        // omit the key entirely (pre-M4 byte-compatibility — the aircraft
        // tail "nx: <v>\n" is unperturbed).
        if (s.missile) {
            w.raw(",\n");
            w.raw("    "); w.string("missile"); w.raw(":"); w.raw("true");
        }
        w.raw("\n");

        w.raw("  }");
        if (i + 1 < snapshots_.size()) w.raw(",");
        w.raw("\n");
    }
    w.raw("]\n");

    // Combat event stream (M4). Emitted only when present: old recordings
    // (and combat-free runs) end at the snapshots array exactly as before.
    if (!combat_events_.empty()) {
        w.raw(",\n");
        w.string("combat_event_count"); w.raw(":");
        w.number(static_cast<std::uint64_t>(combat_events_.size())); w.raw(",\n");
        w.string("combat_events"); w.raw(": [\n");
        for (std::size_t i = 0; i < combat_events_.size(); ++i) {
            const auto& e = combat_events_[i];
            w.raw("  {\n");

            // Comma management: every field emitter below writes
            // "    \"key\": value" and relies on next_field() to place the
            // separating comma + newline. The final field of each event
            // object is always the kind-neutral "sim_time_s" tail emitted
            // without a trailing comma — no per-kind special cases.
            bool first = true;
            auto next_field = [&w, &first]() {
                if (!first) w.raw(",\n");
                first = false;
                w.raw("    ");
            };

            next_field(); w.string("tick"); w.raw(":"); w.number(e.tick);
            next_field(); w.string("kind"); w.raw(":");
                w.string(combat_event_kind_name(e.kind));
            next_field(); w.string("subject_id"); w.raw(":");
                w.number(e.subject_id);
            next_field(); w.string("object_id"); w.raw(":");
                w.number(e.object_id);

            if (e.missile_id != 0) {
                next_field(); w.string("missile_id"); w.raw(":");
                    w.number(e.missile_id);
            }

            if (e.kind == CombatEventKind::MissileLaunched) {
                next_field(); w.string("weapon_name"); w.raw(":");
                    w.string(e.weapon_name);
                next_field(); w.string("speed_ft_s"); w.raw(":");
                    w.number(e.speed_ft_s);
            }

            if (e.kind == CombatEventKind::GunFired) {
                next_field(); w.string("weapon_name"); w.raw(":");
                    w.string(e.weapon_name);
                next_field(); w.string("rounds"); w.raw(":");
                    w.number(static_cast<std::uint64_t>(
                        e.rounds < 0 ? 0 : e.rounds));
            }

            if (e.kind == CombatEventKind::MissileLaunched ||
                e.kind == CombatEventKind::MissileDetonated ||
                e.kind == CombatEventKind::GunFired) {
                next_field(); w.string("position"); w.raw(": { ");
                w.string("x"); w.raw(":"); w.number(e.position.x); w.raw(", ");
                w.string("y"); w.raw(":"); w.number(e.position.y); w.raw(", ");
                w.string("z"); w.raw(":"); w.number(e.position.z); w.raw(" }");
            }

            if (e.kind == CombatEventKind::MissileDetonated) {
                next_field(); w.string("end_cause"); w.raw(":");
                    w.string(e.end_cause);
                next_field(); w.string("miss_distance_ft"); w.raw(":");
                    w.number(e.miss_distance_ft);
                next_field(); w.string("flight_time_s"); w.raw(":");
                    w.number(e.flight_time_s);
            }

            if (e.kind == CombatEventKind::DamageApplied) {
                next_field(); w.string("damage"); w.raw(":");
                    w.number(e.damage);
                next_field(); w.string("hit_points_after"); w.raw(":");
                    w.number(e.hit_points_after);
                next_field(); w.string("killed"); w.raw(":");
                    w.raw(e.killed ? "true" : "false");
            }

            if (e.kind == CombatEventKind::RwrLock ||
                e.kind == CombatEventKind::RwrLaunch) {
                next_field(); w.string("range_ft"); w.raw(":");
                    w.number(e.range_ft);
            }

            // Timing tail (always present, never followed by a comma).
            if (!first) w.raw(",\n");
            w.raw("    "); w.string("sim_time_s"); w.raw(":");
            w.number(e.sim_time_s); w.raw("\n");

            w.raw("  }");
            if (i + 1 < combat_events_.size()) w.raw(",");
            w.raw("\n");
        }
        w.raw("]\n");
    }

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

    // Identify unique AIRCRAFT entity ids (first-appearance order). Missile
    // tracks (snapshot.missile == true) are excluded: they are munitions,
    // not flights — the summary's aircraft/phases/anomalies sections stay
    // aircraft-focused. The full missile kinematics live in to_json()
    // where replay hosts read them.
    std::vector<std::uint64_t> entity_ids;
    for (const auto& s : snapshots_) {
        if (s.missile) continue;
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

    // Trace summary (aircraft snapshots only — missile tracks would inject
    // "Missile:guided" transitions into the flight narrative)
    w.string("trace_summary"); w.raw(": {\n");
    std::vector<std::string> state_seq;
    for (const auto& s : snapshots_) {
        if (s.missile) continue;
        const std::string current = s.ai_mode + ":" + s.ai_state;
        if (state_seq.empty() || current != state_seq.back()) {
            state_seq.push_back(current);
        }
    }
    if (!state_seq.empty()) {
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

    // Combat debrief (M4): emitted only when combat events were recorded.
    // Launch outcomes correlate MissileLaunched with the MissileDetonated
    // carrying the same missile_id; kills correlate EntityKilled with the
    // killing DamageApplied (for the weapon that did it). Linear scans are
    // fine — fights produce tens of events, not thousands.
    if (!combat_events_.empty()) {
        w.raw(",\n");
        w.string("combat"); w.raw(": {\n");
        w.raw("  "); w.string("event_count"); w.raw(":");
            w.number(static_cast<std::uint64_t>(combat_events_.size())); w.raw(",\n");
        w.raw("  "); w.string("first_event_s"); w.raw(":");
            w.number(combat_events_.front().sim_time_s); w.raw(",\n");
        w.raw("  "); w.string("last_event_s"); w.raw(":");
            w.number(combat_events_.back().sim_time_s); w.raw(",\n");

        // Launches with outcomes
        w.raw("  "); w.string("launches"); w.raw(": [\n");
        bool first_launch = true;
        for (const auto& launch : combat_events_) {
            if (launch.kind != CombatEventKind::MissileLaunched) continue;

            // Outcome: the detonation of THIS missile (if it happened within
            // the recording).
            const CombatEvent* outcome = nullptr;
            for (const auto& det : combat_events_) {
                if (det.kind == CombatEventKind::MissileDetonated &&
                    det.missile_id == launch.missile_id) {
                    outcome = &det;
                }
            }

            if (!first_launch) w.raw(",\n");
            w.raw("    {\n");
            w.raw("      "); w.string("time_s"); w.raw(":");
                w.number(launch.sim_time_s); w.raw(",\n");
            w.raw("      "); w.string("shooter_id"); w.raw(":");
                w.number(launch.subject_id); w.raw(",\n");
            w.raw("      "); w.string("target_id"); w.raw(":");
                w.number(launch.object_id); w.raw(",\n");
            w.raw("      "); w.string("weapon"); w.raw(":");
                w.string(launch.weapon_name); w.raw(",\n");
            w.raw("      "); w.string("missile_id"); w.raw(":");
                w.number(launch.missile_id); w.raw(",\n");
            if (outcome != nullptr) {
                w.raw("      "); w.string("end_cause"); w.raw(":");
                    w.string(outcome->end_cause); w.raw(",\n");
                w.raw("      "); w.string("miss_distance_ft"); w.raw(":");
                    w.number(outcome->miss_distance_ft); w.raw(",\n");
                w.raw("      "); w.string("flight_time_s"); w.raw(":");
                    w.number(outcome->flight_time_s); w.raw(",\n");
                w.raw("      "); w.string("outcome_time_s"); w.raw(":");
                    w.number(outcome->sim_time_s); w.raw("\n");
            } else {
                w.raw("      "); w.string("end_cause"); w.raw(":");
                    w.string("record_ended"); w.raw("\n");
            }
            w.raw("    }");
            first_launch = false;
        }
        if (!first_launch) w.raw("\n");
        w.raw("  ],\n");

        // Gun bursts (Steps 11-12): one line per burst — the guns fight
        // in the same summary the missile exchange gets. (Damage/kill
        // attribution for gun hits is in the kills array: a gun kill's
        // fatal DamageApplied carries missile_id == 0, resolved to the
        // killer's most recent burst below.)
        w.raw("  "); w.string("gun_bursts"); w.raw(": [\n");
        bool first_burst = true;
        for (const auto& burst : combat_events_) {
            if (burst.kind != CombatEventKind::GunFired) continue;
            if (!first_burst) w.raw(",\n");
            w.raw("    {\n");
            w.raw("      "); w.string("time_s"); w.raw(":");
                w.number(burst.sim_time_s); w.raw(",\n");
            w.raw("      "); w.string("shooter_id"); w.raw(":");
                w.number(burst.subject_id); w.raw(",\n");
            w.raw("      "); w.string("target_id"); w.raw(":");
                w.number(burst.object_id); w.raw(",\n");
            w.raw("      "); w.string("weapon"); w.raw(":");
                w.string(burst.weapon_name); w.raw(",\n");
            w.raw("      "); w.string("rounds"); w.raw(":");
                w.number(static_cast<std::uint64_t>(
                    burst.rounds < 0 ? 0 : burst.rounds)); w.raw("\n");
            w.raw("    }");
            first_burst = false;
        }
        if (!first_burst) w.raw("\n");
        w.raw("  ],\n");

        // Kills
        w.raw("  "); w.string("kills"); w.raw(": [\n");
        bool first_kill = true;
        for (const auto& kill : combat_events_) {
            if (kill.kind != CombatEventKind::EntityKilled) continue;

            // The weapon that did it: the killing DamageApplied's missile,
            // matched back to its launch.
            const CombatEvent* fatal = nullptr;
            for (const auto& d : combat_events_) {
                if (d.kind == CombatEventKind::DamageApplied && d.killed &&
                    d.subject_id == kill.subject_id) {
                    fatal = &d;
                }
            }
            std::string weapon = "unknown";
            if (fatal != nullptr && fatal->missile_id != 0) {
                for (const auto& launch : combat_events_) {
                    if (launch.kind == CombatEventKind::MissileLaunched &&
                        launch.missile_id == fatal->missile_id) {
                        weapon = launch.weapon_name;
                    }
                }
            } else if (fatal != nullptr && fatal->missile_id == 0) {
                // Gun kill (missile_id == 0 is the gun-hit marker): the
                // killer's most recent burst before the fatal damage.
                for (const auto& burst : combat_events_) {
                    if (burst.kind == CombatEventKind::GunFired &&
                        burst.subject_id == kill.object_id &&
                        burst.sim_time_s <= kill.sim_time_s) {
                        weapon = burst.weapon_name;
                    }
                }
            }

            if (!first_kill) w.raw(",\n");
            w.raw("    {\n");
            w.raw("      "); w.string("time_s"); w.raw(":");
                w.number(kill.sim_time_s); w.raw(",\n");
            w.raw("      "); w.string("target_id"); w.raw(":");
                w.number(kill.subject_id); w.raw(",\n");
            w.raw("      "); w.string("killer_id"); w.raw(":");
                w.number(kill.object_id); w.raw(",\n");
            w.raw("      "); w.string("weapon"); w.raw(":");
                w.string(weapon); w.raw("\n");
            w.raw("    }");
            first_kill = false;
        }
        if (!first_kill) w.raw("\n");
        w.raw("  ]\n");

        w.raw("}\n");
    }

    w.raw("}\n");
    return w.str();
}

// ============================================================================
// JSON import (from_json)
// ============================================================================
// Parses the JSON format produced by to_json() using f4::json::Reader.

namespace {

// Parse a {"x":..., "y":..., "z":...} object at the current Reader position.
geo::WorldPosition parse_vec3(json::Reader& r) {
    geo::WorldPosition pos;
    r.expect('{');
    while (!r.consume('}')) {
        auto key = r.read_string();
        r.expect(':');
        if (key == "x")      { pos.x = r.read_number(); }
        else if (key == "y") { pos.y = r.read_number(); }
        else if (key == "z") { pos.z = r.read_number(); }
        else { r.skip_value(); }
        r.consume(',');
    }
    return pos;
}

// Parse a single snapshot object from the current Reader position.
FlightSnapshot parse_snapshot(json::Reader& r) {
    FlightSnapshot snap;
    r.expect('{');
    while (!r.consume('}')) {
        auto key = r.read_string();
        r.expect(':');

        // Timing
        if (key == "tick")              { snap.tick = static_cast<std::uint64_t>(r.read_int()); }
        else if (key == "sim_time_s")   { snap.sim_time_s = r.read_number(); }

        // Identity
        else if (key == "entity_id")    { snap.entity_id = static_cast<std::uint64_t>(r.read_int()); }
        else if (key == "callsign")     { snap.callsign = r.read_string(); }

        // Position
        else if (key == "position")     { snap.position = parse_vec3(r); }

        // Attitude
        else if (key == "heading_rad")  { snap.heading_rad = r.read_number(); }
        else if (key == "pitch_rad")    { snap.pitch_rad = r.read_number(); }
        else if (key == "roll_rad")     { snap.roll_rad = r.read_number(); }

        // Airspeed / altitude
        else if (key == "altitude_agl_ft")   { snap.altitude_agl_ft = r.read_number(); }
        else if (key == "altitude_msl_ft")   { snap.altitude_msl_ft = r.read_number(); }
        else if (key == "vcas_kts")          { snap.vcas_kts = r.read_number(); }
        else if (key == "gs_kts")            { snap.gs_kts = r.read_number(); }
        else if (key == "vt_fps")            { snap.vt_fps = r.read_number(); }
        else if (key == "mach")              { snap.mach = r.read_number(); }

        // Control inputs
        else if (key == "pitch_cmd")         { snap.pitch_cmd = r.read_number(); }
        else if (key == "roll_cmd")          { snap.roll_cmd = r.read_number(); }
        else if (key == "yaw_cmd")           { snap.yaw_cmd = r.read_number(); }
        else if (key == "throttle_cmd")      { snap.throttle_cmd = r.read_number(); }
        else if (key == "speed_brake_cmd")   { snap.speed_brake_cmd = r.read_number(); }
        else if (key == "gear_handle_down")  { snap.gear_handle_down = r.read_bool(); }
        else if (key == "wheel_brakes")      { snap.wheel_brakes = r.read_bool(); }
        else if (key == "nose_steer_on")     { snap.nose_steer_on = r.read_bool(); }

        // AI brain state
        else if (key == "ai_mode")           { snap.ai_mode = r.read_string(); }
        else if (key == "ai_state")          { snap.ai_state = r.read_string(); }
        else if (key == "ai_event")          { snap.ai_event = r.read_string(); }
        else if (key == "ai_guard_result")   { snap.ai_guard_result = r.read_string(); }

        // Intended path
        else if (key == "target_position")        { snap.target_position = parse_vec3(r); }
        else if (key == "target_description")     { snap.target_description = r.read_string(); }
        else if (key == "cross_track_error_ft")   { snap.cross_track_error_ft = r.read_number(); }
        else if (key == "along_track_error_ft")   { snap.along_track_error_ft = r.read_number(); }
        else if (key == "vertical_error_ft")      { snap.vertical_error_ft = r.read_number(); }

        // Ground state
        else if (key == "on_ground")          { snap.on_ground = r.read_bool(); }
        else if (key == "ground_speed_kts")   { snap.ground_speed_kts = r.read_number(); }

        // Engine
        else if (key == "engine_rpm")         { snap.engine_rpm = r.read_number(); }
        else if (key == "afterburner_lit")    { snap.afterburner_lit = r.read_bool(); }
        else if (key == "fuel_lbs")           { snap.fuel_lbs = r.read_number(); }

        // G-loads
        else if (key == "nz")                 { snap.nz = r.read_number(); }
        else if (key == "nx")                 { snap.nx = r.read_number(); }

        // Track type (M4; missiles only — aircraft snapshots omit the key)
        else if (key == "missile")            { snap.missile = r.read_bool(); }

        // Unknown field — skip
        else { r.skip_value(); }

        r.consume(',');  // optional trailing comma
    }
    return snap;
}

// Parse a single CombatEvent object from the current Reader position.
// Unknown keys are skipped, so events written by newer emitters (extra
// per-kind payload fields) load with their payload fields defaulted —
// the same forward-compatibility rule snapshots follow.
CombatEvent parse_combat_event(json::Reader& r) {
    CombatEvent e;
    r.expect('{');
    while (!r.consume('}')) {
        auto key = r.read_string();
        r.expect(':');

        if (key == "tick")          { e.tick = static_cast<std::uint64_t>(r.read_int()); }
        else if (key == "sim_time_s") { e.sim_time_s = r.read_number(); }
        else if (key == "kind") {
            const auto name = r.read_string();
            // Kind names are the stable wire strings; unknown names (a
            // newer writer added a kind) keep the default but still parse.
            for (std::uint8_t k = 0; k <= 8; ++k) {
                const auto kk = static_cast<CombatEventKind>(k);
                if (name == combat_event_kind_name(kk)) {
                    e.kind = kk;
                    break;
                }
            }
        }
        else if (key == "subject_id")  { e.subject_id = static_cast<std::uint64_t>(r.read_int()); }
        else if (key == "object_id")   { e.object_id  = static_cast<std::uint64_t>(r.read_int()); }
        else if (key == "missile_id")  { e.missile_id = static_cast<std::uint64_t>(r.read_int()); }

        // Launch payload
        else if (key == "weapon_name")   { e.weapon_name = r.read_string(); }
        else if (key == "speed_ft_s")    { e.speed_ft_s  = r.read_number(); }
        else if (key == "position")      { e.position    = parse_vec3(r); }
        else if (key == "rounds")        { e.rounds      = static_cast<int>(r.read_int()); }

        // Detonation payload
        else if (key == "end_cause")      { e.end_cause = r.read_string(); }
        else if (key == "miss_distance_ft") { e.miss_distance_ft = r.read_number(); }
        else if (key == "flight_time_s")  { e.flight_time_s = r.read_number(); }

        // Damage payload
        else if (key == "damage")          { e.damage = r.read_number(); }
        else if (key == "hit_points_after") { e.hit_points_after = r.read_number(); }
        else if (key == "killed")          { e.killed = r.read_bool(); }

        // RWR payload
        else if (key == "range_ft")        { e.range_ft = r.read_number(); }

        // Unknown field (e.g. weapon_handle) — skip
        else { r.skip_value(); }

        r.consume(',');  // optional trailing comma
    }
    return e;
}

} // anonymous namespace

FlightRecorder FlightRecorder::from_json(const std::string& json_str) {
    FlightRecorder rec;
    json::Reader r(json_str);
    r.skip_ws();

    r.expect('{');
    while (!r.consume('}')) {
        auto key = r.read_string();
        r.expect(':');

        if (key == "format") {
            (void)r.read_string();  // "f4-flight-recording"
        } else if (key == "version") {
            (void)r.read_int();     // 1
        } else if (key == "scenario") {
            rec.set_scenario_name(r.read_string());
        } else if (key == "snapshot_count") {
            (void)r.read_int();     // informational; we just parse the array
        } else if (key == "combat_event_count") {
            (void)r.read_int();     // informational; we just parse the array
        } else if (key == "snapshots") {
            r.expect('[');
            while (!r.consume(']')) {
                rec.record(parse_snapshot(r));
                r.consume(',');
            }
        } else if (key == "combat_events") {
            r.expect('[');
            while (!r.consume(']')) {
                rec.record(parse_combat_event(r));
                r.consume(',');
            }
        } else {
            r.skip_value();
        }

        r.consume(',');  // optional trailing comma
    }

    return rec;
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
