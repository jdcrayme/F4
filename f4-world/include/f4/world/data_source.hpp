// f4-world/include/f4/world/data_source.hpp
//
// Format adapter interfaces for the ECS bridge. These abstract the
// data source so the bridge can populate EntityWorld from any provider
// (WorldState, a future BMS adapter, a DIS stream, or procedural
// generation) without depending on the specific format layout.
//
// Design:
//   - Each interface exposes count() + per-item accessors for the fields
//     the bridge actually reads. Not every format-derived field has an
//     accessor — only the ones that map to domain components.
//   - Format residue (VU_ID, entity_type, obj_flags, etc.) is exposed
//     via a format_residue() method that fills a PropertyBag. This keeps
//     the interface clean while preserving round-trip capability.
//   - The bridge operates against these interfaces; WorldStateAdapter
//     (see world_loader.hpp) wraps a concrete WorldState.
//
// Phase 4 — part of the ECS Decoupling Plan §6.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <f4/entities/types.hpp>

namespace f4::world {

// Forward declaration — PropertyBag is defined in entity.hpp, but we
// can't include it here (circular). The adapter fills it indirectly
// via the bridge; the interface only needs to provide the raw values.
// The bridge's format_residue() helper in world_loader.cpp handles
// the PropertyBag population.

// ============================================================================
// ICampaignSource — campaign-level time and TE state
// ============================================================================
struct ICampaignSource {
    virtual int32_t current_time() const = 0;
    virtual int32_t te_start_time() const = 0;
    virtual int32_t te_time_limit() const = 0;
    virtual int32_t te_victory_points() const = 0;
    virtual int32_t te_type() const = 0;
    virtual int32_t te_number_teams() const = 0;
    virtual int32_t te_team() const = 0;
    virtual int32_t te_flags() const = 0;
    virtual const std::vector<int32_t>& te_number_aircraft() const = 0;
    virtual const std::vector<int32_t>& te_team_pts() const = 0;
    virtual ~ICampaignSource() = default;
};

// ============================================================================
// ITeamSource — team slot data
// ============================================================================
struct ITeamSource {
    virtual int team_count() const = 0;

    virtual int slot(int i) const = 0;
    virtual uint8_t flags(int i) const = 0;
    virtual uint8_t colour(int i) const = 0;
    virtual const std::string& name(int i) const = 0;
    virtual const std::string& motto(int i) const = 0;

    // .tea enrichment
    virtual bool tea_loaded(int i) const = 0;
    virtual const std::vector<int16_t>& stance(int i) const = 0;
    virtual const std::vector<uint8_t>& member(int i) const = 0;
    virtual uint8_t air_experience(int i) const = 0;
    virtual uint8_t ground_experience(int i) const = 0;
    virtual uint8_t naval_experience(int i) const = 0;
    virtual uint8_t air_defense_experience(int i) const = 0;
    virtual int16_t first_colonel(int i) const = 0;
    virtual int16_t first_commander(int i) const = 0;
    virtual int16_t first_wingman(int i) const = 0;
    virtual int16_t last_wingman(int i) const = 0;

    virtual ~ITeamSource() = default;
};

// ============================================================================
// IObjectiveSource — objective data for the bridge
// ============================================================================
struct IObjectiveSource {
    virtual int objective_count() const = 0;

    // Position (grid coordinates)
    virtual int16_t x(int i) const = 0;
    virtual int16_t y(int i) const = 0;
    virtual float z(int i) const = 0;

    // Type and identity
    virtual int16_t type(int i) const = 0;
    virtual uint16_t entity_type(int i) const = 0;
    virtual const std::string& class_name(int i) const = 0;

    // Ownership
    virtual uint8_t owner(int i) const = 0;
    virtual uint8_t first_owner(int i) const = 0;

    // Priority
    virtual uint8_t priority(int i) const = 0;
    virtual int16_t nameid(int i) const = 0;
    virtual uint32_t obj_flags(int i) const = 0;
    virtual uint32_t parent_id(int i) const = 0;

    // Supply (conditional)
    virtual bool has_supply(int i) const = 0;
    virtual uint8_t supply(int i) const = 0;
    virtual uint8_t fuel(int i) const = 0;
    virtual uint8_t losses(int i) const = 0;
    virtual int32_t last_repair(int i) const = 0;

    // Damage bitmap (conditional)
    virtual bool has_fstatus(int i) const = 0;
    virtual const std::vector<uint8_t>& fstatus(int i) const = 0;

    // Radar (conditional)
    virtual bool has_radar(int i) const = 0;
    virtual const float* detect_ratio(int i) const = 0;  // array of 8
    virtual float radar_range_km(int i) const = 0;
    virtual const std::string& radar_name(int i) const = 0;
    virtual int16_t radar_type_idx(int i) const = 0;

    // Network links (conditional)
    virtual bool has_links(int i) const = 0;
    virtual const std::vector<f4::entities::ObjectiveLink>& links(int i) const = 0;

    // Ground layout (conditional — airbases)
    virtual bool has_ground_layout(int i) const = 0;
    virtual const std::vector<f4::entities::GroundLayoutList>& ground_layout(int i) const = 0;

    // Feature set (conditional)
    virtual bool has_features(int i) const = 0;
    virtual uint8_t features_count(int i) const = 0;
    virtual uint8_t radar_feature(int i) const = 0;
    virtual uint8_t deag_distance(int i) const = 0;
    virtual uint16_t pt_data_index(int i) const = 0;
    virtual const std::array<uint8_t, 8>& objective_detection(int i) const = 0;
    virtual const std::vector<f4::entities::FeatureEntryState>& features(int i) const = 0;

    // VU_ID for cross-reference resolution
    virtual uint32_t id_num(int i) const = 0;
    virtual uint32_t id_creator(int i) const = 0;

    // Additional format residue for PropertyBag
    virtual int16_t camp_id(int i) const = 0;

    // ObjectiveType enum (1-39), 0 if unknown. Used for symbol selection
    // and inspector display. Stored in PropertyBag as "objective_type".
    virtual uint8_t objective_type(int i) const = 0;

    virtual ~IObjectiveSource() = default;
};

// ============================================================================
// IUnitSource — unit data for the bridge
// ============================================================================
struct IUnitSource {
    virtual int unit_count() const = 0;

    // Position (grid coordinates)
    virtual int16_t x(int i) const = 0;
    virtual int16_t y(int i) const = 0;
    virtual float z(int i) const = 0;

    // Core identity
    virtual f4::entities::UnitClass unit_class(int i) const = 0;
    virtual uint8_t domain(int i) const = 0;
    virtual uint8_t unit_subtype(int i) const = 0;
    virtual uint16_t entity_type(int i) const = 0;
    virtual uint32_t roster(int i) const = 0;
    virtual const std::string& class_name(int i) const = 0;

    // Ownership
    virtual uint8_t owner(int i) const = 0;

    // Waypoints (conditional)
    virtual bool has_waypoints(int i) const = 0;
    virtual const std::vector<f4::entities::WaypointState>& waypoints(int i) const = 0;

    // Ground tactical (Battalion/Brigade/TaskForce)
    virtual uint8_t supply(int i) const = 0;
    virtual uint8_t morale(int i) const = 0;
    virtual uint8_t fatigue(int i) const = 0;
    virtual uint8_t heading(int i) const = 0;
    virtual uint8_t final_heading(int i) const = 0;
    virtual uint8_t position(int i) const = 0;
    virtual int32_t last_move(int i) const = 0;
    virtual int32_t last_combat(int i) const = 0;

    // Hierarchy
    virtual uint32_t parent_id(int i) const = 0;
    virtual const std::vector<uint32_t>& element_ids(int i) const = 0;

    // Squadron
    virtual uint32_t airbase_id(int i) const = 0;
    virtual uint8_t specialty(int i) const = 0;
    virtual int16_t aa_kills(int i) const = 0;
    virtual int16_t ag_kills(int i) const = 0;
    virtual int16_t as_kills(int i) const = 0;
    virtual int16_t an_kills(int i) const = 0;
    virtual int16_t missions_flown(int i) const = 0;
    virtual int16_t mission_score(int i) const = 0;
    virtual uint8_t total_losses(int i) const = 0;
    virtual uint8_t pilot_losses(int i) const = 0;
    virtual uint8_t squadron_patch(int i) const = 0;
    virtual int32_t fuel(int i) const = 0;
    virtual const std::vector<f4::entities::PilotState>& pilots(int i) const = 0;

    // Flight
    virtual float flight_altitude(int i) const = 0;
    virtual int32_t fuel_burnt(int i) const = 0;
    virtual int32_t time_on_target(int i) const = 0;
    virtual int32_t mission_over_time(int i) const = 0;
    virtual int16_t mission_target(int i) const = 0;
    virtual uint8_t loadouts(int i) const = 0;
    virtual uint8_t mission(int i) const = 0;
    virtual uint8_t flight_priority(int i) const = 0;
    virtual uint8_t mission_id(int i) const = 0;
    virtual uint8_t eval_flags(int i) const = 0;
    virtual uint32_t package_id(int i) const = 0;
    virtual uint32_t squadron_id(int i) const = 0;
    virtual uint8_t callsign_id(int i) const = 0;
    virtual uint8_t callsign_num(int i) const = 0;

    // Package
    virtual uint8_t wait_cycles(int i) const = 0;
    virtual uint32_t interceptor_id(int i) const = 0;
    virtual uint32_t awacs_id(int i) const = 0;
    virtual uint32_t jstar_id(int i) const = 0;
    virtual uint32_t ecm_id(int i) const = 0;
    virtual uint32_t tanker_id(int i) const = 0;

    // Vehicle composition (conditional)
    virtual bool has_vehicle_groups(int i) const = 0;
    virtual const std::vector<f4::entities::VehicleGroup>& vehicle_groups(int i) const = 0;

    // Unit class scores
    virtual const std::array<uint8_t, 16>& unit_class_scores(int i) const = 0;

    // VU_ID for cross-reference
    virtual uint32_t id_num(int i) const = 0;
    virtual uint32_t id_creator(int i) const = 0;

    // Additional format residue
    virtual int16_t camp_id(int i) const = 0;
    virtual int16_t name_id(int i) const = 0;
    virtual int16_t reinforcement(int i) const = 0;
    virtual int16_t dest_x(int i) const = 0;
    virtual int16_t dest_y(int i) const = 0;
    virtual int32_t movement_type(int i) const = 0;
    virtual int16_t movement_speed(int i) const = 0;
    virtual int16_t max_range(int i) const = 0;
    virtual const std::string& movement_type_name(int i) const = 0;

    // Unit losses
    virtual uint8_t losses(int i) const = 0;
    virtual uint8_t wp_count(int i) const = 0;
    virtual uint8_t elements(int i) const = 0;

    virtual ~IUnitSource() = default;
};

} // namespace f4::world
