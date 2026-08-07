// f4-flight-model/flight_model.hpp
//
// Top-level flight model orchestrator.
//
// Owns all subsystems (atmosphere, aero, FCS, engine, gear, EOM) and runs
// the per-frame update. The host program constructs one FlightModel per
// aircraft, calls init() once, then calls update() every frame.
//
// Ported from F4Flight's flight_model.h/cpp, which is a port of FreeFalcon's
// AirframeClass::Exec().

#pragma once

#include "f4/flight/aerodynamics.hpp"
#include "f4/flight/aircraft_state.hpp"
#include "f4/flight/atmosphere.hpp"
#include "f4/flight/constants.hpp"
#include "f4/flight/engine.hpp"
#include "f4/flight/eom.hpp"
#include "f4/flight/fcs.hpp"
#include "f4/flight/gear.hpp"
#include "f4/flight/stall_state.hpp"
#include "f4/flight/messages.hpp"
#include "f4/data/aircraft_config.hpp"
#include "f4/messaging/bus.hpp"

#include <cstdint>

namespace f4::flight {

/// Flight model orchestrator. Construct one per aircraft.
///
/// Usage:
///   FlightModel fm;
///   fm.init(config, initialAltitude_ft, initialVt_ftps, initialHeading_rad, inAir);
///   while (running) {
///       fm.update(dt, pilotInput, groundZ_ft, groundNormal);
///       // read fm.state() for results
///   }
class FlightModel {
public:
    FlightModel();

    /// Initialise the flight model with the given configuration.
    ///
    ///   initialAltitude_ft : initial altitude AGL (feet, positive up)
    ///   initialVt_ftps     : initial true airspeed (ft/s)
    ///   initialHeading_rad : initial heading (radians, 0 = North)
    ///   inAir              : if true, start airborne; otherwise on ground
    void init(const data::AircraftConfig& cfg,
              double initialAltitude_ft,
              double initialVt_ftps,
              double initialHeading_rad,
              bool inAir);

    /// Run the per-frame update.
    ///
    /// The host calls this once per major frame. The model sub-steps
    /// internally at the minor-frame rate (default: 6 sub-steps per major
    /// frame, giving a 360 Hz minor frame at 60 Hz major).
    ///
    ///   dt           : major-frame step (seconds)
    ///   input        : pilot/AI input
    ///   groundZ_ft   : terrain altitude at the aircraft position (ft)
    ///   groundNormal : up vector at the terrain point (unit, world frame)
    void update(double dt, const PilotInput& input,
                double groundZ_ft, const math::Vec3d& groundNormal);

    // --- Accessors ---
    const AircraftState& state() const noexcept { return state_; }
    AircraftState&       state()       noexcept { return state_; }
    const data::AircraftConfig& config() const noexcept { return cfg_; }
    const FlightControlSystem& fcs() const noexcept { return fcs_; }

    // --- Sub-stepping controls ---
    void   setMinorPerMajor(int n) noexcept { minorPerMajor_ = std::max(1, n); }
    int    minorPerMajor() const noexcept { return minorPerMajor_; }
    double minorFrameTime() const noexcept { return minorFrameTime_; }

    /// Set the ground state (terrain altitude + normal).
    void setGround(double groundZ_ft, const math::Vec3d& normal) {
        state_.gear.groundZ_ft = groundZ_ft;
        state_.gear.groundNormal = normal;
    }

    /// Recompute load factors from the current force state.
    void computeLoadFactors();

    /// Trim the aircraft at the current state.
    ///
    /// Iterates alpha and throttle to find a steady-state 1-G condition.
    /// Returns true on convergence, false if the trim solver didn't converge
    /// within the iteration limit.
    bool trim();

    // --- Stall state machine access ---

    /// The stall state machine. Hosts may attach a trace for debugging:
    ///   f4::fsm::Trace<StallState, StallEvent> trace;
    ///   fm.stallSM().set_trace(&trace);
    ///   trace.set_trace_rejections(true);  // to diagnose "why no transition?"
    StallSM&       stallSM()       noexcept { return stallSM_; }
    const StallSM& stallSM() const noexcept { return stallSM_; }

    /// The stall config (tunable thresholds).
    StallConfig&       stallConfig()       noexcept { return stallCfg_; }
    const StallConfig& stallConfig() const noexcept { return stallCfg_; }

    // --- Message bus integration (optional) ---
    //
    // The FlightModel is the first real consumer of f4-messaging. When a
    // MessageBus is attached, the FlightModel publishes a small set of
    // event messages (StallStateChangeMessage, StallWarningMessage) on
    // interesting transitions. When no bus is attached (the default), the
    // FlightModel behaves exactly as before — the bus is a nullptr and
    // the publish path is a single branch skipped.
    //
    // Threading: the bus must outlive the FlightModel. The FlightModel
    // publishes via publish() (same-thread, synchronous) — this matches
    // §13.2's "sim thread owns the sim bus, calls flush_pending() at the
    // top of the tick, then runs the per-frame update". Other threads
    // that want flight-model events subscribe to the same sim bus and
    // use publish_deferred()/flush_pending() for cross-thread delivery.
    //
    // Optional context fields:
    //   aircraft_id : host-supplied entity ID, included in every published
    //                 message. Defaults to 0 (unassigned). Set this when
    //                 the host creates the FlightModel for a specific
    //                 entity so consumers can route by aircraft.
    //   sim_time_s  : host-supplied sim time, included in every published
    //                 message. Defaults to 0. Set this each tick before
    //                 calling update() so messages carry an accurate
    //                 timestamp. (We do NOT compute sim time inside the
    //                 FlightModel — §13.2 makes SimClock the single
    //                 source of truth for time.)
    void set_message_bus(messaging::MessageBus* bus) noexcept { bus_ = bus; }
    [[nodiscard]] messaging::MessageBus* message_bus() const noexcept { return bus_; }

    void set_aircraft_id(std::uint64_t id) noexcept { aircraft_id_ = id; }
    [[nodiscard]] std::uint64_t aircraft_id() const noexcept { return aircraft_id_; }

    void set_sim_time(double t) noexcept { sim_time_s_ = t; }
    [[nodiscard]] double sim_time() const noexcept { return sim_time_s_; }

private:
    /// Run one minor-frame step.
    void minorStep(double dt, const PilotInput& input);

    /// Compute atmosphere + update aerodynamic state fields.
    void updateAtmosphere();

    /// Recompute strut compression and gear-on-ground flags.
    void updateGear(double dt);

    /// Compute load factors (G) from current force state.
    void accelerometers();

    /// Update the stall state machine: poll flight state, emit events,
    /// write the SM's current state back to AeroState for next frame's
    /// force modification. Called once per minor frame, after aero.
    void updateStallSM(double dt, const PilotInput& input);

    // --- init() sub-functions (pure refactor, no logic change) ---

    /// Validate config, reset state, reconstruct subsystems, rebuild stall SM.
    void initSubsystems(const data::AircraftConfig& cfg);

    /// Set initial position, velocity, heading, and quaternion (level wings).
    void initKinematics(double initialAltitude_ft,
                        double initialVt_ftps,
                        double initialHeading_rad);

    /// Initialise fuel state from config.
    void initFuelState();

    /// Initialise gear and engine to idle / on-ground or in-air state.
    void initGearAndEngine(bool inAir);

    /// Atmosphere, trim alpha, FCS filter init, quaternion rebuild from euler,
    /// trig cache, aero recompute, and load factors.
    void initTrimAndAtmosphere(double initialAltitude_ft);

    data::AircraftConfig cfg_;
    AircraftState        state_;

    Aerodynamics          aero_;
    EngineModel           engine_;
    FlightControlSystem   fcs_;
    GearModel             gear_;
    EquationsOfMotion     eom_;

    // Stall state machine (f4-state-machine)
    StallSM    stallSM_;
    StallConfig stallCfg_;
    double     stallTimer_{0.0};  // time in current stall state (seconds)

    // Previous-frame aero stall flag, for rising-edge detection on the
    // StallWarningMessage publish. Reset to false in init().
    bool       prevAeroStalled_{false};

    // Optional MessageBus for cross-subsystem event publishing. nullptr
    // by default — the FlightModel runs standalone until the host attaches
    // a bus.
    messaging::MessageBus* bus_{nullptr};
    std::uint64_t          aircraft_id_{0};
    double                 sim_time_s_{0.0};

    /// Cached copy of the most recent PilotInput (needed by updateGear
    /// which runs before minorStep but needs the brake handle state).
    PilotInput lastInput_{};

    int    minorPerMajor_{6};
    double minorFrameTime_{1.0 / 360.0};  // 6 sub-steps of 1/360s each = 1/60s major (60 Hz major, 360 Hz minor)
};

}  // namespace f4::flight
