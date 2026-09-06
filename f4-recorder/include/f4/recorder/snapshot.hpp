// f4-recorder/include/f4/recorder/snapshot.hpp
//
// FlightSnapshot — a per-tick recording of one aircraft's complete observable
// state: kinematics, AI control outputs, and AI brain state.
//
// This is the fundamental unit of a flight recording. A vector of snapshots
// (sampled at a fixed rate, typically 10-60 Hz) constitutes the full trace
// of a simulation run. The recording is engine-agnostic — it depends only
// on f4-geo (for WorldPosition) and standard types.
//
// Design principles:
//   - Every field is a value type — snapshots are copyable and storable.
//   - Strings (ai_mode, ai_state, etc.) are human-readable for LLM consumption.
//   - Position is WorldPosition (sim-local ENU feet), matching EntityWorld.
//   - Angles are stored as radians (matching f4::flight::Angle convention)
//     with degree accessor convenience methods.
//   - Intended path data (target_position, cross_track_error) lets viewers
//     render "what the AI was aiming for" alongside the actual trajectory.
//   - M4: `missile` marks a track that is a MISSILE, not an aircraft —
//     recorded per tick while a live missile flies (position/speed/status;
//     kinematic + AI fields stay default). Replay hosts draw missile tracks
//     from the same snapshot stream; the summary/analysis paths filter
//     missiles out of the per-aircraft sections.
//
// Dependencies: f4-geo (WorldPosition). C++20.

#pragma once

#include <cstdint>
#include <string>

#include <f4/geo/position.hpp>

namespace f4::recorder {

// ============================================================================
// FlightSnapshot — one tick of one aircraft's observable state.
// ============================================================================
struct FlightSnapshot {
    // --- Timing ---
    std::uint64_t tick{0};
    double sim_time_s{0.0};

    // --- Identity ---
    std::uint64_t entity_id{0};
    std::string callsign;

    // --- Track type (M4) ---------------------------------------------------
    // False for aircraft (the default and the only kind the format carried
    // before M4). True for missile tracks: callsign carries the weapon name
    // ("AIM-120C"), ai_state the flyout status ("guided"/"ballistic"), and
    // the kinematic fields the missile's. Serialized ONLY when true so
    // aircraft-only recordings stay byte-identical to the pre-M4 format.
    bool missile{false};

    // --- Kinematic state (from TransformComponent + AircraftState) ---
    geo::WorldPosition position;      // sim-local ENU, feet, z-up
    double heading_rad{0.0};         // body yaw, radians
    double pitch_rad{0.0};           // body pitch, radians
    double roll_rad{0.0};            // body roll, radians
    double altitude_agl_ft{0.0};     // above ground level, feet
    double altitude_msl_ft{0.0};     // above mean sea level, feet
    double vcas_kts{0.0};            // calibrated airspeed, knots
    double gs_kts{0.0};              // ground speed, knots
    double vt_fps{0.0};              // true airspeed, ft/s
    double mach{0.0};                // Mach number

    // --- Control inputs (from AIControlOutput -> PilotInput) ---
    double pitch_cmd{0.0};           // [-1, +1] normalized
    double roll_cmd{0.0};            // [-1, +1] normalized
    double yaw_cmd{0.0};             // [-1, +1] normalized
    double throttle_cmd{0.0};        // [0, 1.5] 1.0=MIL, 1.5=AB
    double speed_brake_cmd{-1.0};    // [-1, +1] -1=retracted
    bool   gear_handle_down{false};
    bool   wheel_brakes{false};
    bool   parking_brake{false};
    bool   nose_steer_on{true};

    // --- Flap commands (Tranche A4) ---
    // Mirrors AIControlOutput::tef_cmd/lef_cmd and PilotInput::tefCmd/lefCmd.
    // The FCS CSV trace (FcsTraceSample) already carried these; the replay
    // JSON (FlightSnapshot) did not, leaving flare debugging blind to the
    // landing-configuration schedule. Defaults to 0 (clean) so a snapshot
    // that never set them reads back identically; the writer emits the two
    // keys unconditionally so the replay host's parser is the only contract.
    double tef_cmd{0.0};             // [0, 1] trailing-edge flap
    double lef_cmd{0.0};             // [0, 1] leading-edge flap

    // --- AI brain state (human-readable for LLM debugging) ---
    std::string ai_mode;             // e.g. "TakeoffMode", "LandingMode"
    std::string ai_state;            // e.g. "TakeRunway", "OnFinal"
    std::string ai_event;            // last transition event, or ""
    std::string ai_guard_result;     // e.g. "PASS: range<2nm", or ""

    // --- Intended path (what the AI is steering toward) ---
    geo::WorldPosition target_position;  // current steer point / waypoint
    std::string target_description;      // e.g. "Rwy 36L threshold", "WP3"
    double cross_track_error_ft{0.0};    // lateral deviation from intended path
    double along_track_error_ft{0.0};    // distance along intended path
    double vertical_error_ft{0.0};       // vertical deviation (e.g. from glide slope)

    // --- Gear/ground state ---
    bool on_ground{true};
    bool gear_on_object{false};          // on paved surface (runway/taxiway)
    double ground_speed_kts{0.0};

    // --- Engine state ---
    double engine_rpm{0.0};              // 0..1+ (1=MIL, >1=AB)
    bool   afterburner_lit{false};
    double fuel_lbs{0.0};

    // --- G-loads ---
    double nz{0.0};                      // normal load factor (G)
    double nx{0.0};                      // axial load factor

    // --- Convenience accessors (degree views) ---
    [[nodiscard]] double heading_deg() const noexcept {
        return heading_rad * (180.0 / 3.14159265358979323846);
    }
    [[nodiscard]] double pitch_deg() const noexcept {
        return pitch_rad * (180.0 / 3.14159265358979323846);
    }
    [[nodiscard]] double roll_deg() const noexcept {
        return roll_rad * (180.0 / 3.14159265358979323846);
    }
};

} // namespace f4::recorder
