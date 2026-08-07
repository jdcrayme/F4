// f4-flight-model/aircraft_state.hpp
//
// Runtime state of an aircraft. Owned by the FlightModel. This is the
// equivalent of the legacy AirframeClass member variables, organized into
// logical groups.
//
// All quantities are in Imperial units to match the original coefficient
// tables.
//
// Coordinate frames:
//   World: NED (North-East-Down), Z-down, in feet. Altitude = -z.
//   Body:  X-forward, Y-right, Z-down.
//
// Angle conventions:
//   All angles (alpha, beta, sigma, gmma, mu, psi, theta, phi) are stored
//   as the strong type `f4::flight::Angle` (alias for f4::Quantity<f4::Radians>).
//   Canonical storage is radians; use `.to_degrees()` / `.to_radians()` (or
//   `f4::flight::to_degrees(a)` / `to_radians(a)`) at the call site to pick a
//   unit explicitly. This closes the historical hazard where alpha/beta
//   were degrees while the euler angles were radians, all stored as raw
//   `double` and distinguishable only by comment.
//
//   alpha_dot, beta_dot are `AngularRate` (rad/s canonical).
//
//   Body rates p, q, r stay as raw `double` (rad/s). They are integrated
//   into the quaternion by the EOM and never compared with degree-valued
//   quantities elsewhere; typing them would add friction without closing
//   a real correctness gap.
//
// Ported from F4Flight's aircraft_state.h.

#pragma once

#include "f4/flight/constants.hpp"
#include "f4/flight/angle.hpp"
#include "f4/math/vec3.hpp"
#include "f4/math/quat.hpp"
#include "f4/math/filters.hpp"

#include <vector>

// Forward-declared so AeroState::stallState can be typed without pulling
// in the full stall_state.hpp (and its f4::fsm dependency).
// Defined in f4/flight/stall_state.hpp.
namespace f4::flight {
enum class StallState : int;
}

namespace f4::flight {

// ---------------------------------------------------------------------------
// Pilot / AI input
//
// All control inputs are normalized:
//   pstick, rstick, ypedal: [-1, +1]
//   throttle: [0, 1.5] where 1.0 = MIL power, 1.5 = full afterburner
//   speedBrake: [-1, +1] where -1 = retracted, +1 = fully extended
//   gearHandle: [-1, +1] where -1 = up, +1 = down
//   tefCmd, lefCmd: [0, 1] where 0 = retracted, 1 = fully extended
// ---------------------------------------------------------------------------
struct PilotInput {
    double pstick{0.0};      // pitch stick: -1 (nose down) .. +1 (nose up)
    double rstick{0.0};      // roll stick:  -1 (full left) .. +1 (full right)
    double ypedal{0.0};      // rudder pedal: -1 (full left) .. +1 (full right)
    double throttle{0.0};    // 0..1.5 (1.0 = MIL, 1.5 = full AB)
    double speedBrake{-1.0}; // -1 (retract) .. +1 (extend); default retracted
    double gearHandle{1.0};  // -1 (up) .. +1 (down); default down
    double hookHandle{0.0};  // -1 (up) .. +1 (down)

    double tefCmd{0.0};      // trailing-edge flap command, 0..1
    double lefCmd{0.0};      // leading-edge flap command, 0..1

    bool wheelBrakes{false};
    bool parkingBrake{false};
    bool noseSteerOn{true};
    bool refueling{false};

    /// Validate and clamp all inputs to their documented ranges.
    ///
    /// This is the boundary between external input (AI, scripting, network)
    /// and the flight model — the most hardened interface in the system.
    /// Call this before passing PilotInput to FlightModel::update().
    /// Out-of-range inputs are clamped; in debug builds, out-of-range
    /// values also trigger an assertion failure.
    void validate() noexcept;
};

// ---------------------------------------------------------------------------
// Kinematic state: position, velocity, orientation, body rates
// ---------------------------------------------------------------------------
struct KinematicState {
    // Position (ft) in world NED frame. Altitude = -z.
    double x{0.0}, y{0.0}, z{0.0};

    // Velocity (ft/s) along world axes
    double xdot{0.0}, ydot{0.0}, zdot{0.0};

    // Quaternion (body-to-world rotation). Scalar-first Hamilton convention.
    // q rotates a body-frame vector into the world frame.
    math::Quatd quat;

    // Body angular rates (rad/s): p=roll, q=pitch, r=yaw.
    // Kept as raw double — see file-level note. They feed the quaternion
    // integrator and the FCS roll-rate lag, never a degree-valued comparison.
    double p{0.0}, q{0.0}, r{0.0};

    // Euler angles, recovered from the quaternion.
    // sigma = velocity-vector heading, gmma = flight path angle,
    // mu = velocity-vector roll, psi = body yaw, theta = body pitch,
    // phi = body roll.
    Angle sigma{zero_angle()};
    Angle gmma{zero_angle()};
    Angle mu{zero_angle()};
    Angle psi{zero_angle()};
    Angle theta{zero_angle()};
    Angle phi{zero_angle()};

    // Trigonometry cache (computed every frame by trigonometry()).
    // Stored to avoid recomputing sin/cos in multiple subsystems.
    // These are dimensionless ratios — plain doubles, NOT Angle values.
    double sinalp{0.0}, cosalp{1.0};  // alpha
    double sinbet{0.0}, cosbet{1.0};  // beta
    double singam{0.0}, cosgam{1.0};  // flight path angle
    double sinsig{0.0}, cossig{1.0};  // velocity heading
    double sinmu{0.0},  cosmu{1.0};   // velocity roll
    double sinthe{0.0}, costhe{1.0};  // body pitch
    double sinphi{0.0}, cosphi{1.0};  // body roll
    double sinpsi{0.0}, cospsi{1.0};  // body yaw

    // True airspeed (ft/s)
    double vt{0.0};
};

// ---------------------------------------------------------------------------
// Aerodynamic state: coefficients, forces, surface positions
//
// Forces are stored as ACCELERATIONS (ft/s^2 = force/mass) so that the
// EOM can integrate them directly without dividing by mass each frame.
// ---------------------------------------------------------------------------
struct AeroState {
    // Angle of attack and sideslip. Stored as the strong Angle type so the
    // radians-vs-degrees convention is enforced at the type level. The F-16
    // coefficient tables remain degree-indexed; use to_degrees() at the
    // lookup call site (the one place where the degree convention survives).
    Angle alpha{zero_angle()};    // angle of attack
    Angle beta{zero_angle()};     // sideslip angle

    // Time derivatives. AngularRate carries its own units (rad/s canonical);
    // use to_deg_per_s() / to_rad_per_s() at call sites that need a number.
    AngularRate alpha_dot{zero_angular_rate()};
    AngularRate beta_dot{zero_angular_rate()};

    // Aerodynamic coefficients (dimensionless)
    double cl{0.0}, cd{0.0}, cy{0.0};
    double clalpha{0.0};      // dCL/dalpha (per radian), local slope
    double clalph0{0.0};      // static slope at alpha=0..10deg
    double clift0{0.0};       // CL at alpha=0
    double cnalpha{0.0};      // normal-force slope (used by FCS pitch gain)

    // Forces (accelerations, ft/s^2) in body axes
    double xaero{0.0}, yaero{0.0}, zaero{0.0};
    // Forces in stability axes
    double xsaero{0.0}, ysaero{0.0}, zsaero{0.0};
    // Forces in wind axes
    double xwaero{0.0}, ywaero{0.0}, zwaero{0.0};

    // Lift and drag (ft/s^2, i.e. force/mass)
    double lift{0.0}, drag{0.0};

    // Surface positions (normalized 0..1)
    double tefPos{0.0};        // trailing-edge flap
    double lefPos{0.0};        // leading-edge flap
    double dbrake{0.0};        // speed brake
    double gearPos{1.0};       // gear: 0 (up) .. 1 (down)
    double hookPos{0.0};       // hook
    double dragChutePos{0.0};  // drag chute

    // Stall state
    //   `stalled` is the per-frame DETECTION flag (computed by aero each
    //   frame from vcas/stallSpeed/alpha). `stallState` is the LIFECYCLE
    //   state from the f4-state-machine stall SM (None -> EnteringDeepStall
    //   -> DeepStall -> Spinning/FlatSpin -> Recovering -> None). The aero
    //   force model reads stallState to modify lift (FlatSpin -> lift=0).
    bool   stalled{false};
    double stallSpeed{0.0};    // kcas
    StallState stallState{StallState{0}};  // typed lifecycle state (0 = None)

    // Incremental CD from external stores (set by host for weapons/fuel tanks)
    double cdStores{0.0};
};

// ---------------------------------------------------------------------------
// Engine state
//
// The RPM spool dynamics use a first-order lag filter. The thrust value
// stored here is already an acceleration (lbf/slug = ft/s^2) so the EOM
// can use it directly.
// ---------------------------------------------------------------------------
struct EngineState {
    double rpm{0.0};           // 0..1+ (1 = MIL, >1 = afterburner)
    double rpmCmd{0.0};        // commanded RPM (before lag)
    double thrust{0.0};        // thrust acceleration (ft/s^2) = thrust_lbf / mass
    double fuelFlow{0.0};      // lb/hr
    double ftit{0.0};          // 0..10 normalized turbine temp
    double nozzlePos{0.0};     // 0..1 nozzle position (for vectored thrust)
    bool   aburnLit{false};    // afterburner lit
    bool   engLit{true};       // engine running
    bool   flameout{false};    // engine flamed out

    // Spool filter state (first-order lag on RPM)
    math::LagFilter rpmLag;

    // Per-state seed flag: false until the first update() call that sees
    // a nonzero rpm. When false, update() seeds rpmLag to the current rpm
    // to avoid a spool-up transient from 0. Lives on EngineState (not
    // EngineModel) so that resetting or replacing the state also resets
    // the seed — critical for retrims and scenario resets.
    bool   rpmLagSeeded{false};
};

// ---------------------------------------------------------------------------
// Fuel state
// ---------------------------------------------------------------------------
struct FuelState {
    double fuel_lbs{0.0};          // internal fuel
    double externalFuel_lbs{0.0};  // external tank fuel
    double epuFuel_lbs{0.0};       // emergency power unit fuel
    double weight_lbs{0.0};        // gross weight (empty + fuel + stores)
    double mass_slugs{0.0};        // mass = weight / g
    double emptyWeight_lbs{0.0};   // empty weight
    double loadingFraction{1.0};   // weight / emptyWeight
};

// ---------------------------------------------------------------------------
// FCS state: filter states, gains, and command values
//
// The FCS uses several discrete-time filters (lag, lead-lag, Adams-Bashforth
// integrator) that carry state between frames. These must persist across
// update() calls and be reset on init/trim.
// ---------------------------------------------------------------------------
struct FcsState {
    // --- Pitch channel ---
    math::LagFilter       pitchRateLag;      // filters the pitch-rate command
    math::AdamsBash2Filter pitchIntegral;    // NZ error integrator (Adams-Bashforth 2nd order)
    math::LeadLagFilter   pitchAlphaLag;     // F7Tust lead-lag on alpha command

    // Pitch gains (computed by computeGains, read by runPitch)
    double kp01{1.0}, kp02{1.0}, kp03{2.0}, kp05{1.0};
    double tp01{0.2}, tp02{0.2}, tp03{0.2};  // lead-lag time constants
    double zp01{0.9};                         // pitch damping ratio
    double pshape{0.0};                       // shaped pitch stick input
    double ptcmd{0.0};                        // commanded pitch (G or alpha — G is dimensionless)
    Angle aoacmd{zero_angle()};               // commanded alpha
    bool   aoaCmdModeRuntime{false};          // true = alpha-command, false = G-command

    // --- Roll channel ---
    math::LagFilter rollRateLag;
    double kr01{1.0}, kr02{1.0};  // roll gains
    double tr01{0.25};             // roll rate lag time constant
    double rshape{0.0};            // shaped roll stick input
    double pscmd{0.0};             // commanded roll rate (rad/s)
    double pstab{0.0};             // filtered roll rate
    double maxRoll{80.0};          // roll limit (deg) — set by steering layer
    double maxRollDelta{5.0};      // roll-rate damping window (deg)
    // startRoll accumulates the integrated body-axis roll rate (rad). Kept
    // as raw double — it is the time integral of p (rad/s) and is only ever
    // compared with the deg-valued maxRollDelta via an explicit RTD scaling
    // at the comparison site (see fcs.cpp runRoll).
    double startRoll{0.0};

    // --- Yaw channel ---
    math::LagFilter       yawBetaLag;
    math::AdamsBash2Filter yawIntegral;
    double ky02{1.0}, ky03{2.0}, ky05{1.0};  // yaw gains
    double ty02{0.3};                          // yaw lag time constant
    double yshape{0.0};                        // shaped pedal input
    Angle betcmd{zero_angle()};                // commanded beta

    // --- Damper gains (from limiters) ---
    double plsdamp{1.0}, rlsdamp{1.0}, ylsdamp{1.0};
};

// ---------------------------------------------------------------------------
// Load factor state (G loads in body/stability/wind axes)
//
// Gravity is NOT added to these — they represent the aerodynamic + thrust
// accelerations only. Level flight produces nzcgs = 1.0 because lift
// balances gravity.
// ---------------------------------------------------------------------------
struct LoadFactorState {
    double nxcgb{0.0}, nycgb{0.0}, nzcgb{0.0};  // body axes
    double nxcgs{0.0}, nycgs{0.0}, nzcgs{0.0};  // stability axes
    double nxcgw{0.0}, nycgw{0.0}, nzcgw{0.0};  // wind axes
};

// ---------------------------------------------------------------------------
// Gear / ground state
// ---------------------------------------------------------------------------
struct GearState {
    struct Wheel {
        double strutCompression_ft{0.0};  // current strut compression
        double strutVel_fps{0.0};         // compression rate
        double wheelAngle_rad{0.0};       // visual wheel rotation
        bool   onGround{false};
        bool   broken{false};
        bool   stuck{false};
    };

    std::vector<Wheel> wheels;                    // sized to config.gear.size()
    bool   inAir{true};                           // true = airborne
    bool   planted{false};                        // stationary on ground
    double groundZ_ft{0.0};                       // terrain altitude (NED Z-down)
    math::Vec3d groundNormal{0.0, 0.0, -1.0};     // terrain up vector (NED: -Z is up)
    double muFric{0.04};                          // current friction coefficient
    double minHeight_ft{0.0};                     // minimum body clearance
    double nwsAngle_rad{0.0};                     // nose-wheel steering angle
    bool   onObject{false};                       // on carrier deck / hard surface
    bool   overRunway{true};                      // over paved surface (vs grass)
};

// ---------------------------------------------------------------------------
// Aircraft state: the complete runtime state of one aircraft
//
// This struct is owned by FlightModel and passed by reference to each
// subsystem's update() method. The subsystems read their inputs from the
// relevant sub-structs and write their outputs back.
// ---------------------------------------------------------------------------
struct AircraftState {
    KinematicState   kin;
    AeroState        aero;
    EngineState      engine;
    FuelState        fuel;
    FcsState         fcs;
    LoadFactorState  loads;
    GearState        gear;

    // Atmospheric outputs at current altitude/airspeed
    double rho{0.0};    // air density (slugs/ft^3)
    double pa{0.0};     // ambient pressure (lb/ft^2)
    double mach{0.0};   // Mach number
    double qbar{0.0};   // dynamic pressure (lb/ft^2)
    double qsom{0.0};   // normalized dynamic pressure = q*S/m (ft/s^2 per unit CL)
    double qovt{0.0};   // qbar / vt
    double vcas{0.0};   // calibrated airspeed (knots)
    double sound{0.0};  // speed of sound (ft/s)

    // Wind (world frame, ft/s)
    double windX{0.0}, windY{0.0};

    // Misc
    bool   simplified{false};  // use simplified model (for AI)
    bool   trimming{false};    // currently trimming
    double netAccel{0.0};      // last frame's net acceleration (ft/s^2)
    double vtDot{0.0};         // true airspeed rate (ft/s^2)

    /// Reset every field to its default-constructed value.
    void reset() noexcept { *this = AircraftState{}; }
};

}  // namespace f4::flight
