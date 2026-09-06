// f4-simulation/include/f4/simulation/bubble_manager.hpp
//
// BubbleManager — per-tick deaggregation/reaggregation of campaign units
// based on the player aircraft's position.
//
// FreeFalcon's UnitClass::Deaggregate() promotes a low-fidelity campaign
// unit (one icon, no individual vehicles) to high fidelity (full vehicle
// entities) when the player aircraft enters a "sim bubble" around the
// unit. The bubble radius is documented in Falcon4.AII as:
//
//   [Sim]
//   SIM_BUBBLE_SIZE    = 2.5    ; air-sim deagg bubble, grid units
//   GROUND_BUBBLE_SIZE = 1.0    ; ground-sim deagg bubble, grid units
//
// One grid unit = 1024 ft (the campaign's grid coordinate scale), so:
//   • GROUND_BUBBLE_SIZE = 1.0  → 1024 ft (~312 m, ~0.17 NM)
//   • SIM_BUBBLE_SIZE    = 2.5  → 2560 ft (~780 m, ~0.42 NM)
//
// Falcon4.AII IS parsed as of B.0 (f4-world-convert's AiiConfig) — see
// bubble_radii_from_aii() below. When no AII path is configured (or the
// file is absent), the documented defaults apply unchanged — the exact
// radii this class hardcoded before the parser existed.
//
// The BubbleManager owns:
//   • A reference to the EntityWorld (for spawn/destroy).
//   • A reference to the ClassTable + ModelDatabase (for vehicle spawning).
//   • A map of unit EntityId → spawned-vehicle EntityIds (so it can despawn
//     when a unit leaves the bubble).
//   • A flat vector of currently-spawned vehicle EntityIds (for the renderer).
//
// Each tick, the host calls update(player_pos) with the ownship's ENU
// position. The manager:
//   1. Queries the world for all unit entities (Battalion/Brigade/TaskForce/
//      Squadron) within the bubble radius (air radius for Squadrons, ground
//      radius for the others).
//   2. For units inside the bubble but not yet deaggregated: calls
//      spawn_vehicles_from_unit() (ground/naval) or the Squadron spawn path
//      (handled by the host — the manager only knows about the ground/naval
//      case for now; Squadron deagg is a separate flow because it needs the
//      airfield's parking spots).
//   3. For units outside the bubble but still deaggregated: destroys their
//      spawned vehicle entities and removes them from the map.
//
// The Squadron case is intentionally NOT handled here because it requires
// the ScenarioAirfield + ScenarioAircraft + AircraftConfig — those live on
// the Simulation, not the BubbleManager. The host calls
// spawn_aircraft_from_squadrons() separately (typically once at initialize,
// not per-tick — Squadrons don't move).
//
// Dependencies: f4-entities, f4-world-convert, f4-models, f4-geo, f4-math.

#pragma once

#include <f4/simulation/campaign_bridge.hpp>      // spawn_vehicles_from_unit
#include <f4/simulation/visual_model_component.hpp>

#include <f4/entities/entity.hpp>
#include <f4/world_types/class_table.hpp>
#include <f4/geo/position.hpp>

#include <cstdint>
#include <filesystem>
#include <utility>
#include <unordered_map>
#include <vector>

namespace f4::simulation {

/// One campaign grid unit in feet — the campaign's grid coordinate scale
/// (bubble radii are documented in grid units; the BubbleManager speaks
/// feet). Same conversion the class doc derives: 1.0 grid → 1024 ft.
inline constexpr double CAMPAIGN_GRID_UNIT_FT = 1024.0;

/// Resolve the BubbleManager radii (feet) from an optional Falcon4.AII
/// path. Empty path or missing file → the documented defaults
/// (GROUND_BUBBLE_SIZE 1.0 grid, SIM_BUBBLE_SIZE 2.5 grid → 1024/2560 ft,
/// byte-identical to the pre-B.0 hardcoded radii). A present file is
/// parsed by f4-world-convert's AiiConfig (loud on malformed input) and
/// its bubble sizes convert grid→feet here. Side-effect-free apart from
/// the optional file read. Returns {ground_radius_ft, air_radius_ft}.
std::pair<double, double> bubble_radii_from_aii(
    const std::filesystem::path& aii_path);

/// Per-tick deaggregation/reaggregation manager for ground/naval units.
///
/// Constructed once (typically by Simulation) with the EntityWorld and
/// the shared ClassTable + ModelDatabase. Each tick, the host calls
/// update(player_pos) with the ownship's ENU position. The manager
/// deaggregates units entering the bubble, reaggregates units leaving it.
///
/// Squadrons (air-domain units) are NOT managed here — they require the
/// ScenarioAirfield + AircraftConfig, which live on Simulation. The host
/// calls spawn_aircraft_from_squadrons() separately (typically once at
/// initialize — Squadrons don't move, so per-tick re-deaggregation is
/// unnecessary).
class BubbleManager {
public:
    /// Construct a BubbleManager.
    ///
    /// \param world  The EntityWorld. Must outlive the manager — the manager
    ///               holds a reference and mutates the world on update().
    /// \param ct     The ClassTable for entity_type → vis_type resolution.
    /// \param db     The ModelDatabase for vis_type → ModelRecord.
    /// \param ground_radius_ft  Deagg radius for ground/naval units
    ///                          (default 1024 ft = GROUND_BUBBLE_SIZE).
    /// \param air_radius_ft     Deagg radius for air units (default 2560 ft
    ///                          = SIM_BUBBLE_SIZE). Currently unused —
    ///                          Squadron deagg is handled out-of-band.
    BubbleManager(f4::entities::EntityWorld& world,
                    const f4::world_types::ClassTable& ct,
                    double ground_radius_ft = 1024.0,
                    double air_radius_ft = 2560.0);

    /// Per-tick update: deaggregate units entering the bubble, reaggregate
    /// units leaving it.
    ///
    /// Walks all entities with a VehicleCompositionComponent (Battalion/
    /// Brigade/TaskForce). For each unit:
    ///   - If the unit is within ground_radius_ft of player_pos AND not
    ///     currently deaggregated: spawn its vehicles via
    ///     spawn_vehicles_from_unit(), record the spawned IDs.
    ///   - If the unit is outside ground_radius_ft AND currently
    ///     deaggregated: destroy each spawned vehicle entity, remove the
    ///     unit from the deaggregated map.
    ///   - Otherwise (in bubble and already deaggregated, or out of bubble
    ///     and not deaggregated): no-op.
    ///
    /// The Squadron case is NOT handled here — see class doc.
    ///
    /// \param player_pos  The ownship's ENU position (feet, z-up).
    void update(const f4::geo::WorldPosition& player_pos);

    /// Force-deaggregate a specific unit immediately (ignores bubble radius).
    /// Useful for tests, scenario setup, and "always deaggregate this objective
    /// garrison" overrides. The unit will still be reaggregated by update()
    /// if the player moves away.
    void force_deaggregate(f4::entities::EntityId unit_id);

    /// Force-reaggregate a specific unit immediately (ignores bubble radius).
    /// Destroys the unit's spawned vehicle entities. No-op if the unit isn't
    /// currently deaggregated.
    void force_reaggregate(f4::entities::EntityId unit_id);

    /// Reaggregate everything — destroy ALL spawned vehicle entities.
    /// Called by the Simulation destructor (or before a scenario reload)
    /// to clean up. After this call, deaggregated_units() is empty.
    void clear();

    /// All currently-deaggregated vehicle EntityIds (flat vector).
    /// The renderer iterates this (after converting to EntityMeshDraw via
    /// VisualModelComponent + TransformComponent) for draw_entity_meshes().
    [[nodiscard]] const std::vector<f4::entities::EntityId>&
    vehicle_entities() const noexcept { return vehicle_entities_; }

    /// Map of unit EntityId → spawned vehicle EntityIds.
    /// Exposed for inspection / debugging / tests. Don't mutate directly —
    /// use update() / force_deaggregate() / force_reaggregate().
    [[nodiscard]] const std::unordered_map<uint64_t, std::vector<f4::entities::EntityId>>&
    deaggregated_units() const noexcept { return deaggregated_; }

    /// Number of units currently deaggregated.
    [[nodiscard]] std::size_t deaggregated_unit_count() const noexcept {
        return deaggregated_.size();
    }

    /// The configured ground-bubble radius (ft).
    [[nodiscard]] double ground_radius_ft() const noexcept {
        return ground_radius_ft_;
    }

    /// Override the ground-bubble radius (ft). V-3DLIVE: the map viewer
    /// drives the bubble from the CAMERA instead of the ownship, and
    /// scales the radius with the zoom level (deaggregate what the user
    /// is looking at — a quarter of the visible extent, clamped). The
    /// ownship path never calls this, so the radius stays the AII /
    /// documented default there. Clamped to a sane minimum (128 ft).
    void set_ground_radius_ft(double ft) noexcept {
        ground_radius_ft_ = ft < 128.0 ? 128.0 : ft;
    }

    /// The configured air-bubble radius (ft).
    [[nodiscard]] double air_radius_ft() const noexcept {
        return air_radius_ft_;
    }

private:
    /// Deaggregate a single unit. Idempotent — no-op if already deaggregated.
    /// Returns the spawned vehicle EntityIds (empty if spawn failed or the
    /// unit was already deaggregated).
    std::vector<f4::entities::EntityId>
    deaggregate_(f4::entities::EntityId unit_id);

    /// Reaggregate a single unit. Destroys all its spawned vehicles.
    /// No-op if not currently deaggregated.
    void reaggregate_(f4::entities::EntityId unit_id);

    /// Remove a vehicle EntityId from vehicle_entities_ (preserves order
    /// via swap-and-pop — order doesn't matter for rendering).
    void remove_vehicle_(f4::entities::EntityId vid);

    f4::entities::EntityWorld& world_;
    const f4::world_types::ClassTable& ct_;
    double ground_radius_ft_;
    double air_radius_ft_;

    /// Map: unit EntityId.value → spawned vehicle EntityIds.
    /// Keyed by .value (uint64) so we can use unordered_map without a
    /// custom hash for EntityId.
    std::unordered_map<uint64_t, std::vector<f4::entities::EntityId>> deaggregated_;

    /// Flat vector of all currently-spawned vehicle EntityIds (across all
    /// deaggregated units). Updated incrementally by deaggregate_ /
    /// reaggregate_. The renderer iterates this each frame.
    std::vector<f4::entities::EntityId> vehicle_entities_;
};

} // namespace f4::simulation
