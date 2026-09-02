// f4-world/include/f4/world/world_adapters.hpp
//
// WorldState → I*Source adapters — OPT-IN public header.
//
// The adapter structs originally lived in world_loader.cpp as an
// implementation detail (the header-leak fix, see world_loader.hpp §13).
// They are still deliberately kept OUT of the umbrella f4_world.hpp —
// including this header pulls in the full WorldState layout — but they
// are now available to consumers that legitimately need to hand a
// WorldState to an interface-based consumer. First of those: f4-campaign
// (the dynamic campaign consumes the IDataSource interfaces, never
// EntityWorld components, per NEXT_PHASE_PLAN.md §B's boundary rule).
//
// Usage:
//   #include <f4/world/world_adapters.hpp>   // opt-in
//   f4::world::WorldState ws;                // detail header comes with it
//   ws.load(json_path);
//   f4::world::WorldStateAdapters adapters(ws);
//   consume(adapters.teams);                 // const ITeamSource&
//
// Lifetime: the adapters borrow `ws` — keep it alive.

#pragma once

#include <f4/world/data_source.hpp>
#include <f4/world/detail/world_state.hpp>

namespace f4::world {

struct CampaignAdapter : ICampaignSource {
    explicit CampaignAdapter(const WorldState& ws) : ws_(&ws) {}

    int32_t current_time() const override { return ws_->campaign.current_time; }
    int32_t te_start_time() const override { return ws_->campaign.te_start_time; }
    int32_t te_time_limit() const override { return ws_->campaign.te_time_limit; }
    int32_t te_victory_points() const override { return ws_->campaign.te_victory_points; }
    int32_t te_type() const override { return ws_->campaign.te_type; }
    int32_t te_number_teams() const override { return ws_->campaign.te_number_teams; }
    int32_t te_team() const override { return ws_->campaign.te_team; }
    int32_t te_flags() const override { return ws_->campaign.te_flags; }
    const std::vector<int32_t>& te_number_aircraft() const override { return ws_->campaign.te_number_aircraft; }
    const std::vector<int32_t>& te_team_pts() const override { return ws_->campaign.te_team_pts; }

private:
    const WorldState* ws_;
};

struct TeamAdapter : ITeamSource {
    explicit TeamAdapter(const WorldState& ws) : ws_(&ws) {}

    int team_count() const override { return static_cast<int>(ws_->teams.size()); }
    int slot(int i) const override { return ws_->teams[i].slot; }
    uint8_t flags(int i) const override { return ws_->teams[i].flags; }
    uint8_t colour(int i) const override { return ws_->teams[i].colour; }
    const std::string& name(int i) const override { return ws_->teams[i].name; }
    const std::string& motto(int i) const override { return ws_->teams[i].motto; }
    bool tea_loaded(int i) const override { return ws_->teams[i].tea_loaded; }
    const std::vector<int16_t>& stance(int i) const override { return ws_->teams[i].stance; }
    const std::vector<uint8_t>& member(int i) const override { return ws_->teams[i].member; }
    uint8_t air_experience(int i) const override { return ws_->teams[i].air_experience; }
    uint8_t ground_experience(int i) const override { return ws_->teams[i].ground_experience; }
    uint8_t naval_experience(int i) const override { return ws_->teams[i].naval_experience; }
    uint8_t air_defense_experience(int i) const override { return ws_->teams[i].air_defense_experience; }
    int16_t first_colonel(int i) const override { return ws_->teams[i].first_colonel; }
    int16_t first_commander(int i) const override { return ws_->teams[i].first_commander; }
    int16_t first_wingman(int i) const override { return ws_->teams[i].first_wingman; }
    int16_t last_wingman(int i) const override { return ws_->teams[i].last_wingman; }

private:
    const WorldState* ws_;
};

struct ObjectiveAdapter : IObjectiveSource {
    explicit ObjectiveAdapter(const WorldState& ws) : ws_(&ws) {}

    int objective_count() const override { return static_cast<int>(ws_->objectives.size()); }
    int16_t x(int i) const override { return ws_->objectives[i].x; }
    int16_t y(int i) const override { return ws_->objectives[i].y; }
    float z(int i) const override { return ws_->objectives[i].z; }
    int16_t type(int i) const override { return ws_->objectives[i].type; }
    uint16_t entity_type(int i) const override { return ws_->objectives[i].entity_type; }
    const std::string& class_name(int i) const override { return ws_->objectives[i].class_name; }
    uint8_t owner(int i) const override { return ws_->objectives[i].owner; }
    uint8_t first_owner(int i) const override { return ws_->objectives[i].first_owner; }
    uint8_t priority(int i) const override { return ws_->objectives[i].priority; }
    int16_t nameid(int i) const override { return ws_->objectives[i].nameid; }
    uint32_t obj_flags(int i) const override { return ws_->objectives[i].obj_flags; }
    uint32_t parent_id(int i) const override { return ws_->objectives[i].parent_id; }
    bool has_supply(int i) const override {
        const auto& o = ws_->objectives[i];
        return o.supply != 0 || o.fuel != 0 || o.losses != 0 || o.last_repair != 0;
    }
    uint8_t supply(int i) const override { return ws_->objectives[i].supply; }
    uint8_t fuel(int i) const override { return ws_->objectives[i].fuel; }
    uint8_t losses(int i) const override { return ws_->objectives[i].losses; }
    int32_t last_repair(int i) const override { return ws_->objectives[i].last_repair; }
    bool has_fstatus(int i) const override { return !ws_->objectives[i].fstatus.empty(); }
    const std::vector<uint8_t>& fstatus(int i) const override { return ws_->objectives[i].fstatus; }
    bool has_radar(int i) const override { return ws_->objectives[i].has_radar; }
    const float* detect_ratio(int i) const override { return ws_->objectives[i].detect_ratio; }
    float radar_range_km(int i) const override { return ws_->objectives[i].radar_range_km; }
    const std::string& radar_name(int i) const override { return ws_->objectives[i].radar_name; }
    int16_t radar_type_idx(int i) const override { return ws_->objectives[i].radar_type_idx; }
    bool has_links(int i) const override { return !ws_->objectives[i].links.empty(); }
    const std::vector<f4::entities::ObjectiveLink>& links(int i) const override { return ws_->objectives[i].links; }
    bool has_ground_layout(int i) const override { return !ws_->objectives[i].ground_layout.empty(); }
    const std::vector<f4::entities::GroundLayoutList>& ground_layout(int i) const override { return ws_->objectives[i].ground_layout; }
    bool has_features(int i) const override {
        const auto& o = ws_->objectives[i];
        return o.features_count > 0 || !o.features.empty();
    }
    uint8_t features_count(int i) const override { return ws_->objectives[i].features_count; }
    uint8_t radar_feature(int i) const override { return ws_->objectives[i].radar_feature; }
    uint8_t deag_distance(int i) const override { return ws_->objectives[i].deag_distance; }
    uint16_t pt_data_index(int i) const override { return ws_->objectives[i].pt_data_index; }
    const std::array<uint8_t, 8>& objective_detection(int i) const override { return ws_->objectives[i].objective_detection; }
    const std::vector<f4::entities::FeatureEntryState>& features(int i) const override { return ws_->objectives[i].features; }
    uint32_t id_num(int i) const override { return ws_->objectives[i].id_num; }
    uint32_t id_creator(int i) const override { return ws_->objectives[i].id_creator; }
    int16_t camp_id(int i) const override { return ws_->objectives[i].camp_id; }
    uint8_t objective_type(int i) const override { return ws_->objectives[i].objective_type; }

private:
    const WorldState* ws_;
};

// UnitAdapter — implements ALL unit interfaces (core + 4 subclass).
//
// The WorldState UnitState struct is a flat god-struct with all fields for
// all subclasses, zero-initialized for unused ones. This adapter therefore
// implements every interface trivially — each method reads the field
// directly from UnitState, regardless of unit class.
//
// The as_*() methods return `this` when unit_class(i) matches the subclass,
// nullptr otherwise. A future adapter (BMS, procedural) that only supports
// some subclasses would return nullptr for the others without having to
// implement the unsupported interface at all.
struct UnitAdapter : IUnitCoreSource,
                     IGroundUnitSource,
                     ISquadronSource,
                     IFlightSource,
                     IPackageSource {
    explicit UnitAdapter(const WorldState& ws) : ws_(&ws) {}

    // --- IUnitCoreSource ---
    int unit_count() const override { return static_cast<int>(ws_->units.size()); }
    int16_t x(int i) const override { return ws_->units[i].x; }
    int16_t y(int i) const override { return ws_->units[i].y; }
    float z(int i) const override { return ws_->units[i].z; }
    f4::entities::UnitClass unit_class(int i) const override { return ws_->units[i].unit_class; }
    uint8_t domain(int i) const override { return ws_->units[i].domain; }
    uint8_t unit_subtype(int i) const override { return ws_->units[i].unit_subtype; }
    uint16_t entity_type(int i) const override { return ws_->units[i].entity_type; }
    uint32_t roster(int i) const override { return ws_->units[i].roster; }
    const std::string& class_name(int i) const override { return ws_->units[i].class_name; }
    uint8_t owner(int i) const override { return ws_->units[i].owner; }
    bool has_waypoints(int i) const override { return !ws_->units[i].waypoints.empty(); }
    const std::vector<f4::entities::WaypointState>& waypoints(int i) const override { return ws_->units[i].waypoints; }
    bool has_vehicle_groups(int i) const override { return !ws_->units[i].vehicle_groups.empty(); }
    const std::vector<f4::entities::VehicleGroup>& vehicle_groups(int i) const override { return ws_->units[i].vehicle_groups; }
    const std::array<uint8_t, 16>& unit_class_scores(int i) const override { return ws_->units[i].unit_class_scores; }
    uint32_t id_num(int i) const override { return ws_->units[i].id_num; }
    uint32_t id_creator(int i) const override { return ws_->units[i].id_creator; }
    int16_t camp_id(int i) const override { return ws_->units[i].camp_id; }
    int16_t name_id(int i) const override { return ws_->units[i].name_id; }
    int16_t reinforcement(int i) const override { return ws_->units[i].reinforcement; }
    int16_t dest_x(int i) const override { return ws_->units[i].dest_x; }
    int16_t dest_y(int i) const override { return ws_->units[i].dest_y; }
    int32_t movement_type(int i) const override { return ws_->units[i].movement_type; }
    int16_t movement_speed(int i) const override { return ws_->units[i].movement_speed; }
    int16_t max_range(int i) const override { return ws_->units[i].max_range; }
    const std::string& movement_type_name(int i) const override { return ws_->units[i].movement_type_name; }
    uint8_t losses(int i) const override { return ws_->units[i].losses; }
    uint8_t wp_count(int i) const override { return ws_->units[i].wp_count; }
    uint8_t elements(int i) const override { return ws_->units[i].elements; }

    // --- Subclass accessors ---
    // WorldState-backed adapter implements all subclass interfaces, so
    // return `this` when the class matches. The ambiguous base class is
    // resolved by the explicit static_cast.
    const IGroundUnitSource* as_ground_unit(int i) const override {
        using f4::entities::UnitClass;
        const auto uc = unit_class(i);
        return (uc == UnitClass::Battalion || uc == UnitClass::Brigade || uc == UnitClass::TaskForce)
            ? static_cast<const IGroundUnitSource*>(this) : nullptr;
    }
    const ISquadronSource* as_squadron(int i) const override {
        return (unit_class(i) == f4::entities::UnitClass::Squadron)
            ? static_cast<const ISquadronSource*>(this) : nullptr;
    }
    const IFlightSource* as_flight(int i) const override {
        return (unit_class(i) == f4::entities::UnitClass::Flight)
            ? static_cast<const IFlightSource*>(this) : nullptr;
    }
    const IPackageSource* as_package(int i) const override {
        return (unit_class(i) == f4::entities::UnitClass::Package)
            ? static_cast<const IPackageSource*>(this) : nullptr;
    }

    // --- IGroundUnitSource ---
    uint8_t supply(int i) const override { return ws_->units[i].supply; }
    uint8_t morale(int i) const override { return ws_->units[i].morale; }
    uint8_t fatigue(int i) const override { return ws_->units[i].fatigue; }
    uint8_t heading(int i) const override { return ws_->units[i].heading; }
    uint8_t final_heading(int i) const override { return ws_->units[i].final_heading; }
    uint8_t position(int i) const override { return ws_->units[i].position; }
    int32_t last_move(int i) const override { return ws_->units[i].last_move; }
    int32_t last_combat(int i) const override { return ws_->units[i].last_combat; }
    uint32_t parent_id(int i) const override { return ws_->units[i].parent_id; }
    const std::vector<uint32_t>& element_ids(int i) const override { return ws_->units[i].element_ids; }

    // --- ISquadronSource ---
    uint32_t airbase_id(int i) const override { return ws_->units[i].airbase_id; }
    uint8_t specialty(int i) const override { return ws_->units[i].specialty; }
    int16_t aa_kills(int i) const override { return ws_->units[i].aa_kills; }
    int16_t ag_kills(int i) const override { return ws_->units[i].ag_kills; }
    int16_t as_kills(int i) const override { return ws_->units[i].as_kills; }
    int16_t an_kills(int i) const override { return ws_->units[i].an_kills; }
    int16_t missions_flown(int i) const override { return ws_->units[i].missions_flown; }
    int16_t mission_score(int i) const override { return ws_->units[i].mission_score; }
    uint8_t total_losses(int i) const override { return ws_->units[i].total_losses; }
    uint8_t pilot_losses(int i) const override { return ws_->units[i].pilot_losses; }
    uint8_t squadron_patch(int i) const override { return ws_->units[i].squadron_patch; }
    int32_t fuel(int i) const override { return ws_->units[i].fuel; }
    const std::vector<f4::entities::PilotState>& pilots(int i) const override { return ws_->units[i].pilots; }

    // --- IFlightSource ---
    float flight_altitude(int i) const override { return ws_->units[i].flight_altitude; }
    int32_t fuel_burnt(int i) const override { return ws_->units[i].fuel_burnt; }
    int32_t time_on_target(int i) const override { return ws_->units[i].time_on_target; }
    int32_t mission_over_time(int i) const override { return ws_->units[i].mission_over_time; }
    int16_t mission_target(int i) const override { return ws_->units[i].mission_target; }
    uint8_t loadouts(int i) const override { return ws_->units[i].loadouts; }
    uint8_t mission(int i) const override { return ws_->units[i].mission; }
    uint8_t flight_priority(int i) const override { return ws_->units[i].flight_priority; }
    uint8_t mission_id(int i) const override { return ws_->units[i].mission_id; }
    uint8_t eval_flags(int i) const override { return ws_->units[i].eval_flags; }
    uint32_t package_id(int i) const override { return ws_->units[i].package_id; }
    uint32_t squadron_id(int i) const override { return ws_->units[i].squadron_id; }
    uint8_t callsign_id(int i) const override { return ws_->units[i].callsign_id; }
    uint8_t callsign_num(int i) const override { return ws_->units[i].callsign_num; }

    // --- IPackageSource ---
    uint8_t wait_cycles(int i) const override { return ws_->units[i].wait_cycles; }
    uint32_t interceptor_id(int i) const override { return ws_->units[i].interceptor_id; }
    uint32_t awacs_id(int i) const override { return ws_->units[i].awacs_id; }
    uint32_t jstar_id(int i) const override { return ws_->units[i].jstar_id; }
    uint32_t ecm_id(int i) const override { return ws_->units[i].ecm_id; }
    uint32_t tanker_id(int i) const override { return ws_->units[i].tanker_id; }

private:
    const WorldState* ws_;
};

struct WorldStateAdapters {
    CampaignAdapter campaign;
    TeamAdapter teams;
    ObjectiveAdapter objectives;
    UnitAdapter units;

    explicit WorldStateAdapters(const WorldState& ws)
        : campaign(ws), teams(ws), objectives(ws), units(ws) {}
};

} // namespace f4::world
