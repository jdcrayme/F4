// f4-flight-api/i_aircraft_state.hpp
//
// IAircraftState — read-only interface for the aircraft state subset
// consumed by AI modules.
//
// Phase 2 (H2): AI modules (TakeoffModule, future NavigationModule,
// CombatModule, etc.) depend on this thin interface rather than the
// full AircraftState struct (35+ fields). This:
//   1. Prevents AI from accidentally reading internal flight-model
//      fields (aero coefficients, FCS filter states, etc.)
//   2. Makes the AI's data contract explicit and auditable
//   3. Removes the AI's dependency on aircraft_state.hpp (and its
//      transitive includes: angle.hpp, math/quat.hpp, math/filters.hpp)
//   4. Eliminates NED↔ENU conversion scattered through AI code — the
//      interface presents position in ENU (the AI's natural frame)
//
// Position convention: ENU (East-North-Up), feet. This matches
// f4::geo::WorldPosition, which is the AI's working coordinate frame.
// The flight model stores position internally as NED (North-East-Down);
// the implementation of this interface performs the conversion so the
// AI never needs to think about NED.
//
// Phase 2+: Moved to f4-flight-api so f4-ai can depend on this
// lightweight API instead of the full f4-flight-model library.
//
// C++20.

#pragma once

namespace f4::flight {

// ============================================================================
// IAircraftState
// ============================================================================
class IAircraftState {
public:
    virtual ~IAircraftState() = default;

    // --- Position in ENU frame (feet) ---
    // x = east,  y = north,  z = up (= altitude MSL)
    // These are the components of f4::geo::WorldPosition. We expose
    // them as individual doubles rather than returning a WorldPosition
    // to avoid coupling this interface to f4-geo. Callers that need
    // a WorldPosition construct one from the three values:
    //   geo::WorldPosition(s.position_east_ft(), s.position_north_ft(),
    //                      s.altitude_msl_ft());

    /// Easting in the simulation-local ENU frame (feet).
    virtual double position_east_ft() const = 0;

    /// Northing in the simulation-local ENU frame (feet).
    virtual double position_north_ft() const = 0;

    // --- Altitude ---

    /// Altitude above mean sea level (feet, positive up).
    /// In the ENU frame, this is the z component (= -NED_z).
    virtual double altitude_msl_ft() const = 0;

    /// Altitude above ground level (feet, positive up).
    /// = altitude_msl_ft() - terrain_altitude_ft.
    virtual double altitude_agl_ft() const = 0;

    // --- Airspeed ---

    /// Calibrated airspeed (knots).
    virtual double vcas_kts() const = 0;

    // --- Attitude ---

    /// Heading angle (radians, 0 = North, clockwise positive).
    /// This is the yaw angle ψ projected onto the horizontal plane.
    virtual double heading_rad() const = 0;

    // --- Ground state ---

    /// True if any gear is in contact with the ground.
    virtual bool on_ground() const = 0;
};

} // namespace f4::flight
