// f4-simulation/src/bubble_manager.cpp
//
// Implementation of BubbleManager — see bubble_manager.hpp for the
// architecture rationale.

#include "f4/simulation/bubble_manager.hpp"

#include <f4/entities/entity.hpp>
#include <f4/world_types/aii_config.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace f4::simulation {

std::pair<double, double> bubble_radii_from_aii(
    const std::filesystem::path& aii_path) {
    // Absent path / missing file → the documented defaults, which convert
    // to exactly the radii this module hardcoded before B.0:
    //   1.0 grid × 1024 = 1024 ft, 2.5 grid × 1024 = 2560 ft.
    const auto aii = f4::world_types::AiiConfig::load_if_exists(aii_path);
    return {aii.ground_bubble_size_grid() * CAMPAIGN_GRID_UNIT_FT,
            aii.sim_bubble_size_grid() * CAMPAIGN_GRID_UNIT_FT};
}

BubbleManager::BubbleManager(f4::entities::EntityWorld& world,
                              const f4::world_types::ClassTable& ct,
                              const f4::models::ModelDatabase& db,
                              double ground_radius_ft,
                              double air_radius_ft)
    : world_(world)
    , ct_(ct)
    , db_(db)
    , ground_radius_ft_(ground_radius_ft)
    , air_radius_ft_(air_radius_ft) {}

void BubbleManager::update(const f4::geo::WorldPosition& player_pos) {
    using namespace f4::entities;

    // Walk all units with VehicleCompositionComponent (Battalion / Brigade /
    // TaskForce — Squadrons don't have this component, so they're naturally
    // excluded). For each, decide whether to deaggregate or reaggregate
    // based on distance to the player.
    //
    // We snapshot the unit IDs first because spawn/destroy mutates the
    // world during iteration (the with_component query would be invalidated
    // by the spawn loop).
    const auto unit_ids = world_.with_component<VehicleCompositionComponent>();

    const double ground_r2 = ground_radius_ft_ * ground_radius_ft_;

    for (const auto unit_id : unit_ids) {
        EntityHandle h(unit_id, &world_);
        const auto* tf = h.get<TransformComponent>();
        if (!tf) continue;

        // Squared distance (3D) — ground units have z=0, so the vertical
        // component is typically just player altitude, which is fine.
        const double dx = tf->position.x - player_pos.x;
        const double dy = tf->position.y - player_pos.y;
        const double dz = tf->position.z - player_pos.z;
        const double d2 = dx * dx + dy * dy + dz * dz;

        const bool in_bubble = d2 <= ground_r2;
        const bool is_deagg = deaggregated_.count(unit_id.value) > 0;

        if (in_bubble && !is_deagg) {
            // Entered the bubble — deaggregate.
            deaggregate_(unit_id);
        } else if (!in_bubble && is_deagg) {
            // Left the bubble — reaggregate.
            reaggregate_(unit_id);
        }
        // else: in bubble & already deagg, OR out of bubble & not deagg — no-op.
    }

    // Note: units that are deaggregated but no longer have a
    // VehicleCompositionComponent (shouldn't happen in normal use, but
    // could occur if the world is mutated externally) will stay in
    // deaggregated_ indefinitely. The clear() method exists for full reset.
}

void BubbleManager::force_deaggregate(f4::entities::EntityId unit_id) {
    deaggregate_(unit_id);
}

void BubbleManager::force_reaggregate(f4::entities::EntityId unit_id) {
    reaggregate_(unit_id);
}

void BubbleManager::clear() {
    // Destroy all spawned vehicle entities, then clear the maps.
    // We iterate deaggregated_ (the unit→vehicles map) because vehicle_entities_
    // doesn't tell us which unit each vehicle belongs to (and we don't need
    // to — we're clearing everything).
    for (auto& [unit_val, vehicle_ids] : deaggregated_) {
        for (const auto vid : vehicle_ids) {
            // The vehicle entity might already be destroyed (e.g. by a
            // scenario reload that wiped the world). Guard with alive().
            if (world_.alive(vid)) {
                world_.destroy(vid);
            }
        }
    }
    deaggregated_.clear();
    vehicle_entities_.clear();
}

std::vector<f4::entities::EntityId>
BubbleManager::deaggregate_(f4::entities::EntityId unit_id) {
    // Idempotent — no-op if already deaggregated.
    if (deaggregated_.count(unit_id.value) > 0) return {};

    auto spawned = spawn_vehicles_from_unit(world_, ct_, db_, unit_id);
    if (spawned.empty()) return {};

    // Record the spawned IDs so we can despawn them later.
    deaggregated_[unit_id.value] = spawned;
    vehicle_entities_.insert(vehicle_entities_.end(),
                              spawned.begin(), spawned.end());
    return spawned;
}

void BubbleManager::reaggregate_(f4::entities::EntityId unit_id) {
    auto it = deaggregated_.find(unit_id.value);
    if (it == deaggregated_.end()) return;

    // Destroy each spawned vehicle entity.
    for (const auto vid : it->second) {
        if (world_.alive(vid)) {
            world_.destroy(vid);
        }
        remove_vehicle_(vid);
    }
    deaggregated_.erase(it);
}

void BubbleManager::remove_vehicle_(f4::entities::EntityId vid) {
    // Swap-and-pop — order in vehicle_entities_ doesn't matter (the
    // renderer iterates it without index assumptions).
    auto it = std::find(vehicle_entities_.begin(), vehicle_entities_.end(), vid);
    if (it == vehicle_entities_.end()) return;
    if (it != vehicle_entities_.end() - 1) {
        *it = std::move(vehicle_entities_.back());
    }
    vehicle_entities_.pop_back();
}

} // namespace f4::simulation
