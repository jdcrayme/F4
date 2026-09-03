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
    // B.3 QC tranche — the campaign bullseye (shared reference point).
    // Default-implemented (optional data, same rule as the package
    // request accessors): sources that predate the tranche keep compiling.
    virtual int32_t bullseye_x() const { return 0; }
    virtual int32_t bullseye_y() const { return 0; }
    virtual int32_t bullseye_name() const { return 0; }
    // C2 (war-loop tasking) — the .cmp header's maintenance timers
    // (absolute campaign times, v>=19 block). last_reinforcement anchors
    // the aircraft-replacement cadence f4-campaign's tasking consumes;
    // last_resupply/last_repair belong to the ground-supply and objective
    // repair tranches (carried now so those consumers need no interface
    // churn later). Default-implemented for the same reason as bullseye.
    virtual int32_t last_resupply() const { return 0; }
    virtual int32_t last_repair() const { return 0; }
    virtual int32_t last_reinforcement() const { return 0; }
    virtual ~ICampaignSource() = default;
};

// ============================================================================
// ITeamSource — team slot data
// ============================================================================

/// The campaign relations vocabulary — FreeFalcon cmpglobl.h RelType,
/// the value domain of every stance entry (.tea AND the .cmp team block;
/// RoEData[][] is indexed by it):
///   0 NoRelations, 1 Allied, 2 Friendly, 3 Neutral, 4 Hostile, 5 War.
/// The wire's i16 stance entries are this enum, NOT a sign convention —
/// the pre-C3 reading "< 0 = at war" misdecoded the garbage columns real
/// saves carry toward unused team slots (TestCamp: every team's stance
/// toward the empty Gorn slot is -5141) as WAR against a phantom side,
/// which made the U.S. a belligerent in a war it is Neutral to (TestCamp:
/// the actual war is ROK-DPRK, mutual 5) and starved target selection of
/// every enemy objective.
enum class Relation : int16_t {
    NoRelations = 0,
    Allied      = 1,
    Friendly    = 2,
    Neutral     = 3,
    Hostile     = 4,
    War         = 5,
};

/// Decode one wire stance value. Values outside 0..5 (e.g. the -5141
/// garbage toward unused slots) decode as NoRelations — the honest
/// reading of "no team there", and the one the reference's RoE table
/// gives a 0 column anyway.
[[nodiscard]] constexpr Relation relation_from_wire(int16_t v) noexcept {
    if (v < 0 || v > 5) return Relation::NoRelations;
    return static_cast<Relation>(v);
}

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

    // C2 (war-loop tasking) — the .cmp team block's aircraft-replacement
    // stock (ushort at team offset 56, v>53; 0 when the team carries
    // none — a legal wire state). Default-implemented: alternative
    // sources that predate the tranche keep compiling.
    virtual uint16_t replacements_avail(int i) const { return 0; }

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
// Unit source interfaces — split by subclass (Phase 5 refactor)
//
// PREVIOUS DESIGN (tagged-union anti-pattern):
//   IUnitSource was a single ~50-method interface. Every adapter (WorldState,
//   future BMS, future procedural) had to implement ALL 50 methods for EVERY
//   unit, returning zeros for the ~35 that didn't apply to the unit's class.
//   This is the exact tagged-union anti-pattern the ECS components were
//   designed to eliminate — recreated one layer up, at the adapter interface.
//
// NEW DESIGN (per-subclass interfaces):
//   IUnitCoreSource   — always-present fields (position, identity, VU_ID,
//                       format residue). Every unit has these.
//   IGroundUnitSource — Battalion / Brigade / TaskForce (supply, morale,
//                       fatigue, heading, hierarchy, last_move/combat).
//   ISquadronSource   — Squadron (airbase, kills, pilots, fuel, patch).
//   IFlightSource     — Flight (altitude, mission, package/squadron refs,
//                       callsign, fuel_burnt, time_on_target).
//   IPackageSource    — Package (wait_cycles, interceptor/awacs/jstar/
//                       ecm/tanker refs).
//
// The core interface exposes `as_ground_unit(i)`, `as_squadron(i)`, etc.
// which return a pointer to the subclass interface (or nullptr if the unit
// isn't that subclass). The bridge queries the subclass interface only when
// the unit is of the matching class.
//
// A WorldState-backed adapter implements ALL interfaces (the WorldState
// UnitState struct has all fields, zero-initialized for unused subclasses).
// A future BMS or procedural adapter only implements the interfaces it has
// data for — `as_*()` returns nullptr for the rest, and the bridge skips
// the corresponding component population.
// ============================================================================

// ---------------------------------------------------------------------------
// IGroundUnitSource — Battalion / Brigade / TaskForce
// ---------------------------------------------------------------------------
struct IGroundUnitSource {
    virtual uint8_t supply(int i) const = 0;
    virtual uint8_t morale(int i) const = 0;
    virtual uint8_t fatigue(int i) const = 0;
    virtual uint8_t heading(int i) const = 0;
    virtual uint8_t final_heading(int i) const = 0;
    virtual uint8_t position(int i) const = 0;
    virtual int32_t last_move(int i) const = 0;
    virtual int32_t last_combat(int i) const = 0;

    // Hierarchy (Battalion has parent brigade, Brigade has child battalions)
    virtual uint32_t parent_id(int i) const = 0;
    virtual const std::vector<uint32_t>& element_ids(int i) const = 0;

    virtual ~IGroundUnitSource() = default;
};

// ---------------------------------------------------------------------------
// ISquadronSource — Squadron
// ---------------------------------------------------------------------------
struct ISquadronSource {
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

    virtual ~ISquadronSource() = default;
};

// ---------------------------------------------------------------------------
// IFlightSource — Flight (mission element within a package)
// ---------------------------------------------------------------------------
struct IFlightSource {
    virtual float flight_altitude(int i) const = 0;
    virtual int32_t fuel_burnt(int i) const = 0;
    virtual int32_t time_on_target(int i) const = 0;
    virtual int32_t mission_over_time(int i) const = 0;
    virtual int16_t mission_target(int i) const = 0;
    virtual uint8_t loadouts(int i) const = 0;
    /// Decoded loadout stations (wire weapon ids + counts; empty for
    /// clean-wing flights). The campaign bridge maps these onto the
    /// engine's WeaponStoreComponent at spawn. (A-G tranche)
    virtual std::vector<f4::entities::LoadoutStationState>
    loadout_stations(int i) const = 0;
    virtual uint8_t mission(int i) const = 0;
    virtual uint8_t flight_priority(int i) const = 0;
    virtual uint8_t mission_id(int i) const = 0;
    virtual uint8_t eval_flags(int i) const = 0;
    virtual uint32_t package_id(int i) const = 0;
    virtual uint32_t squadron_id(int i) const = 0;
    virtual uint8_t callsign_id(int i) const = 0;
    virtual uint8_t callsign_num(int i) const = 0;

    virtual ~IFlightSource() = default;
};

// ---------------------------------------------------------------------------
// IPackageSource — Package (groups multiple Flights)
// ---------------------------------------------------------------------------
struct IPackageSource {
    virtual uint8_t wait_cycles(int i) const = 0;
    virtual uint32_t interceptor_id(int i) const = 0;
    virtual uint32_t awacs_id(int i) const = 0;
    virtual uint32_t jstar_id(int i) const = 0;
    virtual uint32_t ecm_id(int i) const = 0;
    virtual uint32_t tanker_id(int i) const = 0;

    // B.3 tranche — package element flights + the ATM mission request.
    // element_ids: the package's element Flight VU_ID.nums, in wire order
    // (same vector the Brigade subclass uses for its child battalions —
    // one field, two class-dependent meanings on the wire). The bridge
    // resolves these to EntityIds on PackageSupportComponent::elements.
    //
    // These are OPTIONAL data with default implementations (unlike the five
    // pure virtuals above): a request-less / element-less package is a legal
    // wire state, not an unsupported adapter, so alternative sources that
    // predate the B.3 tranche keep compiling and simply report "nothing
    // carried" rather than becoming abstract classes. The WorldState-backed
    // UnitAdapter overrides every one of them.
    virtual const std::vector<uint32_t>& element_ids(int) const {
        static const std::vector<uint32_t> kEmpty;
        return kEmpty;
    }
    virtual bool request_present(int) const { return false; }
    virtual uint8_t request_mission(int) const { return 0; }
    virtual int32_t request_tot(int) const { return 0; }
    virtual uint8_t request_priority(int) const { return 0; }
    virtual uint16_t request_action_type(int) const { return 0; }
    virtual uint32_t request_target_num(int) const { return 0; }
    virtual uint32_t request_target_creator(int) const { return 0; }
    virtual uint32_t request_requester_num(int) const { return 0; }

    virtual ~IPackageSource() = default;
};

// ---------------------------------------------------------------------------
// IUnitCoreSource — always-present fields for every unit
//
// This is the PRIMARY interface the bridge consumes. The `as_*()` methods
// return subclass interfaces when the unit is of the matching class, or
// nullptr otherwise. The bridge never calls a subclass method without
// first checking the pointer.
// ---------------------------------------------------------------------------
struct IUnitCoreSource {
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

    // Conditional (applies to multiple subclasses)
    virtual bool has_waypoints(int i) const = 0;
    virtual const std::vector<f4::entities::WaypointState>& waypoints(int i) const = 0;
    virtual bool has_vehicle_groups(int i) const = 0;
    virtual const std::vector<f4::entities::VehicleGroup>& vehicle_groups(int i) const = 0;
    virtual const std::array<uint8_t, 16>& unit_class_scores(int i) const = 0;

    // VU_ID for cross-reference
    virtual uint32_t id_num(int i) const = 0;
    virtual uint32_t id_creator(int i) const = 0;

    // Format residue (stored in PropertyBag by the bridge)
    virtual int16_t camp_id(int i) const = 0;
    virtual int16_t name_id(int i) const = 0;
    virtual int16_t reinforcement(int i) const = 0;
    virtual int16_t dest_x(int i) const = 0;
    virtual int16_t dest_y(int i) const = 0;
    virtual int32_t movement_type(int i) const = 0;
    virtual int16_t movement_speed(int i) const = 0;
    virtual int16_t max_range(int i) const = 0;
    virtual const std::string& movement_type_name(int i) const = 0;
    virtual uint8_t losses(int i) const = 0;
    virtual uint8_t wp_count(int i) const = 0;
    virtual uint8_t elements(int i) const = 0;

    // C3 (war-loop routing) — the UCD threat-model arrays, indexed by
    // MoveType (NoMove=0, Foot=1, Wheeled=2, Tracked=3, LowAir=4, Air=5,
    // Naval=6, Rail=7). An air-defense battalion's weapon_range[LowAir]/
    // [Air] are its SAM/AAA engagement rings and hit_chance[LowAir]/[Air]
    // gate the ring (nonzero = can hit at range). Default-implemented
    // (zeros): sources that predate the tranche keep compiling — a
    // zeroed map simply has no threats, the same as a save whose world
    // JSON carried no theater-db enrichment.
    virtual const std::array<uint8_t, 8>& unit_hit_chance(int) const {
        static const std::array<uint8_t, 8> kZero{};
        return kZero;
    }
    virtual const std::array<uint8_t, 8>& unit_weapon_range(int) const {
        static const std::array<uint8_t, 8> kZero{};
        return kZero;
    }

    // --- Subclass accessors ---
    // Return the subclass interface if unit i is of the matching class,
    // nullptr otherwise. The bridge checks the pointer before calling any
    // subclass-specific method.
    //
    // A WorldState-backed adapter implements all four subclass interfaces
    // and returns `this` when the class matches. A future adapter that
    // only supports some subclasses returns nullptr for the others.
    virtual const IGroundUnitSource* as_ground_unit(int i) const = 0;
    virtual const ISquadronSource*  as_squadron(int i) const = 0;
    virtual const IFlightSource*    as_flight(int i) const = 0;
    virtual const IPackageSource*   as_package(int i) const = 0;

    virtual ~IUnitCoreSource() = default;
};

// Backward-compatibility alias. Existing code that references IUnitSource
// will need to migrate to IUnitCoreSource; this alias ensures a clear
// compile error pointing to the new name rather than a mysterious
// "IUnitSource was not declared".
//
// NOTE: This alias is intentionally NOT defined — the split changes the
// interface contract (subclass methods moved to separate interfaces), so
// a blind name swap would not be sufficient. Search-and-replace
// IUnitSource → IUnitCoreSource, then fix the subclass method calls.

} // namespace f4::world
