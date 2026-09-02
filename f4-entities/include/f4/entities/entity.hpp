// f4-entities/entity.hpp
//
// Component-based entity system. Replaces FreeFalcon's deep inheritance
// hierarchy (VuEntity -> FalconEntity -> SimBaseClass -> ... -> AircraftClass,
// 200+ member god-classes) with a lightweight ID handle + typed components.
//
// An entity is a stable, generation-tagged handle (EntityId). Behavior and
// data live in components (TransformComponent, CampaignIdentityComponent, ...)
// that can be added, removed, and queried independently. Systems operate on
// entities that have the components they require.
//
// Design notes:
//   - EntityId is a 64-bit value: low 32 bits = slot index, high 32 bits =
//     generation. This makes IDs comparable, copyable, and stable across
//     component mutations; a destroyed slot's generation bumps so stale
//     handles are detected.
//   - Tags are a string-keyed variant bag for coarse classification
//     (role=fighter, team=blue, domain=air). These are NOT components —
//     they're for fast filtering (give me all red fighters), not for
//     per-frame data.
//   - The TransformComponent stores position as f4::geo::WorldPosition — a
//     strong type. This is the design decision made concrete: the source of
//     truth is the sim-local frame, and Earth-frame coordinates are derived
//     views obtained via f4-geo conversions (requiring a TheaterDatum).
//   - Components are named after domain concepts, not file fields.
//     ObjectiveTypeComponent, not obj_flags. SupplyStateComponent, not
//     supply+fuel+losses. The bridge (world_loader.cpp) is the only place
//     where format-derived names (VU_ID, nameid, obj_flags) are resolved.
//   - Components are added conditionally. Not every objective has radar,
//     not every unit has waypoints. The ECS has<T>() check replaces
//     if (field != 0) guards on a god-struct.

#pragma once

#include <atomic>
#include <cstdint>
#include <compare>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <typeindex>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include <f4/geo/position.hpp>
#include "types.hpp"

// Forward declaration — full definition in <f4/messaging/bus.hpp>.
// BehavioralComponentBase::update() takes a MessageBus& so behavioral
// components can publish/subscribe without each one storing its own
// pointer. The library itself never calls any MessageBus method, so a
// forward declaration is sufficient here; consumers (tests, f4-sim)
// include the full header.
namespace f4::messaging { class MessageBus; }

namespace f4::entities {

    // ============================================================================
    // EntityId — stable, generation-tagged handle.
    //
    //   bits [ 0 .. 31 ] = slot index into EntityWorld::entities_
    //   bits [32 .. 63 ] = generation (incremented on destroy/reuse)
    //
    // value == 0 is the "null" id (never handed out by create()).
    // ============================================================================
    struct EntityId {
        uint64_t value = 0;

        [[nodiscard]] bool valid() const noexcept { return value != 0; }
        explicit operator bool() const noexcept { return valid(); }

        [[nodiscard]] uint32_t index() const noexcept {
            return static_cast<uint32_t>(value & 0xFFFFFFFFu);
        }
        [[nodiscard]] uint32_t generation() const noexcept {
            return static_cast<uint32_t>(value >> 32);
        }

        static EntityId make(uint32_t index, uint32_t generation) noexcept {
            EntityId id;
            id.value = (static_cast<uint64_t>(generation) << 32) | index;
            return id;
        }

        auto operator<=>(const EntityId&) const = default;
    };

    // ============================================================================
    // Tags — coarse, string-keyed classification for fast filtering.
    // (role, team, domain, alive, stealth, ...). Not for per-frame data.
    //
    // PERFORMANCE NOTE (M2 fix): Tag comparisons during with_tag() and
    // query() were O(n) per entity with string heap allocations on every
    // TagKey construction. The TagKey now supports both owning (std::string)
    // and non-owning (std::string_view) construction. For hot-path queries,
    // use pre-interned TagKey constants (see tags:: namespace below) which
    // are constructed once and reused, avoiding per-query heap allocation.
    // ============================================================================
    struct TagKey {
        std::string name;
        explicit TagKey(std::string n) : name(std::move(n)) {}
        TagKey(const char* n) : name(n) {}
        TagKey(std::string_view sv) : name(sv) {}  // non-owning construction
        auto operator<=>(const TagKey&) const = default;
    };

    struct TagValue {
        // Type-safe sum type. Replaces the previous hand-rolled tagged union
        // (4 parallel fields, only 1 populated, ~40 bytes wasted per int/bool
        // tag). std::variant gives us auto-visitors, no wasted storage, and
        // compile-time type safety.
        using Variant = std::variant<std::string, int64_t, double, bool>;
        Variant value;

        // Type tag retained for backward compatibility with code that
        // switches on TagValue::Type. Prefer as_string()/as_int()/... or
        // std::visit for new code.
        enum class Type { String, Int, Float, Bool };
        [[nodiscard]] Type type() const noexcept {
            return static_cast<Type>(value.index());
        }

        static TagValue from(std::string s) { TagValue v; v.value = std::move(s); return v; }
        static TagValue from(int64_t i)      { TagValue v; v.value = i;            return v; }
        static TagValue from(double f)        { TagValue v; v.value = f;            return v; }
        static TagValue from(bool b)          { TagValue v; v.value = b;            return v; }

        // Type-safe accessors. Return nullptr if the variant doesn't hold
        // the requested type. Replaces the old str_val/int_val/float_val/
        // bool_val fields (which were default-constructed zeros/empties for
        // the inactive type — silently returning a fake value instead of
        // surfacing the type mismatch).
        [[nodiscard]] const std::string* as_string() const noexcept { return std::get_if<0>(&value); }
        [[nodiscard]] const int64_t*     as_int()    const noexcept { return std::get_if<1>(&value); }
        [[nodiscard]] const double*      as_float()  const noexcept { return std::get_if<2>(&value); }
        [[nodiscard]] const bool*        as_bool()   const noexcept { return std::get_if<3>(&value); }

        auto operator<=>(const TagValue&) const = default;
    };

} // namespace f4::entities

// Custom hasher for TagKey — replaces the previous std::hash specialization
// which was undefined behavior per [namespace.std]/2. Must be visible before
// TagSet is defined below.
namespace f4::entities {
    struct TagKeyHash {
        std::size_t operator()(const TagKey& k) const noexcept {
            return std::hash<std::string>{}(k.name);
        }
    };
} // namespace f4::entities

namespace f4::entities {

    using TagSet = std::unordered_map<TagKey, TagValue, TagKeyHash>;

    // Custom hasher for TagValue — needed so we can use TagValue as a key
    // in the Phase D tag_index_ (std::unordered_map<TagValue, std::vector<EntityId>>).
    // std::hash is not defined for std::variant by default, so we hash the
    // variant by combining the type index with the hash of the held value.
    // This is the same approach std::pair's std::hash specialization uses.
    //
    // Defined here (before EntityWorld uses it in a std::unordered_map) so
    // the type is complete when the hash is instantiated.
    struct TagValueHash {
        std::size_t operator()(const TagValue& v) const noexcept {
            // Hash = type_index ^ hash_of_held_value (FNV-style combine).
            // The type index disambiguates e.g. int 0 from bool false from
            // empty string, which all compare unequal as TagValues but would
            // otherwise hash to similar values.
            const auto idx = v.value.index();
            std::size_t h = idx;
            std::visit([&h](const auto& held) {
                using T = std::decay_t<decltype(held)>;
                if constexpr (std::is_same_v<T, std::string>) {
                    h ^= std::hash<std::string>{}(held) + 0x9e3779b9u + (h << 6) + (h >> 2);
                } else if constexpr (std::is_same_v<T, int64_t>) {
                    h ^= std::hash<int64_t>{}(held) + 0x9e3779b9u + (h << 6) + (h >> 2);
                } else if constexpr (std::is_same_v<T, double>) {
                    h ^= std::hash<double>{}(held) + 0x9e3779b9u + (h << 6) + (h >> 2);
                } else if constexpr (std::is_same_v<T, bool>) {
                    h ^= std::hash<bool>{}(held) + 0x9e3779b9u + (h << 6) + (h >> 2);
                }
            }, v.value);
            return h;
        }
    };

    namespace tags {
        inline constexpr const char* ROLE = "role";    // "fighter","bomber","tanker","awacs",...
        inline constexpr const char* TEAM = "team";    // "red","blue"
        inline constexpr const char* OPDOMAIN = "domain";  // "air","ground","naval"
        inline constexpr const char* ALIVE = "alive";   // bool
        inline constexpr const char* STEALTH = "stealth"; // bool

        // --- Phase A tag enrichment (see ECS refactoring plan) ---------------
        // These tags promote commonly-queried per-entity fields out of
        // components and into the tag store, so that non-selected entities
        // (the bulk of what the world viewer iterates each frame) can be
        // rendered/filtered without a component lookup (type_index hash +
        // pointer chase). They are set once by the loader at populate time
        // and never mutated.
        //
        // NAME  — string. The display name of the entity:
        //         teams     → callsign ("ROK", "Japan", ...)
        //         objectives→ ObjectiveTypeComponent::class_name
        //                     ("02_20 Airbase 2", "Highway Strip NS", ...)
        //         units     → UnitCoreComponent::class_name
        //                     ("Armor Battalion", "Patrol", ...)
        //         campaign  → "Campaign"
        //
        // CLASS — int64_t. The fine-grained classification number (the raw
        //         input to the category-name lookup, NOT the display string).
        //         Storing the raw int avoids duplicating the name tables that
        //         already live in f4-world-convert (objective_type_name,
        //         unit_subtype_name). Consumers that need a display string
        //         call those functions with the tag value. Encoding:
        //         objectives→ objective_type (1..39)
        //         units     → unit_subtype (STYPE_UNIT_*; domain is already
        //                     available via tags::OPDOMAIN)
        //         teams/campaign → tag absent
        //
        // ICON  — int64_t. The raw input to the renderer's symbol-dispatch
        //         function, precomputed at load time so consumers can call
        //         symbol_for_objective_type(tag) or symbol_for_unit(tag)
        //         without querying ObjectiveTypeComponent/UnitCoreComponent.
        //         Storing the *input* (not the SymbolKind enum value) keeps
        //         f4-world independent of f4-renderer — the dispatch still
        //         happens at the consumer, but the component fetch is
        //         eliminated. The encoding is:
        //         objectives→ objective_type (1..39), stored as-is
        //         units     → (unit_class << 8) | unit_subtype
        //         teams/campaign → tag absent (no map icon)
        inline constexpr const char* NAME  = "name";
        inline constexpr const char* CLASS = "class";
        inline constexpr const char* ICON  = "icon";
    }

    // detail::next_world_cookie — internal helper used by EntityWorld's
    // custom move ctor/assign to regenerate the world cookie on the
    // destination. Declared here (in the public header) so the inline
    // move ctor can call it; defined out-of-line in entity.cpp where the
    // global atomic counter lives. Not part of the public API — do not
    // call from outside the entity library.
    namespace detail {
        [[nodiscard]] uint64_t next_world_cookie() noexcept;
    }

    // ============================================================================
    // Components — typed data blobs attached to entities.
    // ============================================================================
    struct ComponentBase {
        virtual ~ComponentBase() = default;
        [[nodiscard]] virtual std::type_index type_id() const = 0;
    };

    template<typename Derived>
    struct Component : ComponentBase {
        [[nodiscard]] std::type_index type_id() const override {
            return std::type_index(typeid(Derived));
        }
    };

    // ============================================================================
    // BehavioralComponent — components with per-tick update logic.
    //
    // All existing components (TransformComponent, TeamComponent, ...) are
    // passive data: they hold state but do nothing each tick. A behavioral
    // component adds an `update(dt, bus)` hook called by EntityWorld::
    // update_all() once per tick.
    //
    // Update ordering is controlled by `priority()`:
    //   - priority() == 0  : passive — not iterated by update_all(). This
    //                        is the default for ComponentBase; behavioral
    //                        components override to return a non-zero value.
    //   - priority() >= 75 : runs in pass 1 ("brains"). Brains read world
    //                        state and produce control inputs.
    //   - 0 < priority() < 75 : runs in pass 2 ("physics"). Flight models,
    //                        gear, sensors — anything that consumes the
    //                        brain's outputs and advances simulation state.
    //
    // The two-pass split guarantees that by the time a flight model updates,
    // every brain on every entity has already published its commands. The
    // campaign-scale optimization is DONE: EntityWorld caches a flat vector
    // of behavioral component pointers (rebuilt lazily when components are
    // added/removed or entities created/destroyed), so update_all() no
    // longer dynamic_casts every component of every entity each tick. A
    // populated campaign world (4,374 entities, ~5 components each) went
    // from ~36 ms/tick to ~0.2 ms/tick with 4 spawned aircraft. Pass
    // semantics are unchanged: priorities are re-read every tick (a
    // component whose priority() changes at runtime still moves between
    // passes), and within a pass the visit order matches the entity-index /
    // component-map order the pre-cache implementation walked.
    //
    // on_attached() is called once by EntityHandle::add<T>() after the
    // component is constructed and stored on the entity. Override to
    // capture a back-reference to the owning EntityHandle, so the
    // behavioral component can look up sibling components on the same
    // entity (e.g. a brain looking up its FlightModelComponent).
    // ============================================================================
    class EntityHandle;  // forward-declared for on_attached parameter

    namespace update_phase {
        // Components with priority >= BRAIN_THRESHOLD run in pass 1.
        // Components with priority > 0 and < BRAIN_THRESHOLD run in pass 2.
        // Components with priority == 0 are passive (not iterated).
        inline constexpr int BRAIN_THRESHOLD = 75;

        // Conventional priorities for built-in component families.
        // (Informational; not enforced — any value > 0 means "active".)
        inline constexpr int BRAIN_PRIORITY   = 100;  // AI brains, decision logic
        inline constexpr int PHYSICS_PRIORITY = 50;  // flight models, gear, sensors
    } // namespace update_phase

    struct BehavioralComponentBase : ComponentBase {
        // Higher priority runs earlier in the tick. Default 0 means
        // "passive" — update_all() skips this component. Behavioral
        // components override to return a value from update_phase::
        // BRAIN_PRIORITY or update_phase::PHYSICS_PRIORITY (or a custom
        // value that respects the threshold convention).
        [[nodiscard]] virtual int priority() const noexcept { return 0; }

        // Per-tick update. Called by EntityWorld::update_all() with the
        // sim tick length (seconds) and the message bus. The bus is shared
        // across all entities and components — synchronous publishes fire
        // their handlers within this call; deferred publishes accumulate
        // until the caller (f4-sim main loop) flushes them.
        virtual void update(double dt, messaging::MessageBus& bus) = 0;

        // Lifecycle hook: called once after EntityHandle::add<T>() stores
        // this component on the entity. Override to capture a back-reference
        // for sibling-component lookup. Default is a no-op.
        virtual void on_attached(EntityHandle& self) { (void)self; }
    };

    template<typename Derived>
    struct BehavioralComponent : BehavioralComponentBase {
        [[nodiscard]] std::type_index type_id() const override {
            return std::type_index(typeid(Derived));
        }
    };

    // --- Core components ---------------------------------------------------------

    /// Spatial transform in the sim-local frame. The single source of truth for
    /// where an entity is. Position is the strong type f4::geo::WorldPosition.
    struct TransformComponent : Component<TransformComponent> {
        f4::geo::WorldPosition position{};
        // Orientation as a quaternion (Hamilton convention, body-to-world).
        double qw = 1.0, qx = 0.0, qy = 0.0, qz = 0.0;
        // World-frame velocity (ft/s) and body-frame rates (rad/s).
        double vx = 0.0, vy = 0.0, vz = 0.0;
        double p = 0.0, q = 0.0, r = 0.0;

        /// Convenience: world-frame velocity as a WorldPosition.
        /// Semantically velocity is not position (different physical dimension),
        /// but both share the ENU frame and the same struct layout.
        /// This accessor makes the intent explicit at call sites.
        [[nodiscard]] f4::geo::WorldPosition velocity() const noexcept {
            return f4::geo::WorldPosition(vx, vy, vz);
        }
    };

    /// Links a sim entity to its campaign-level identity.
    /// Narrowed: only team_id + callsign. unit_type_name removed — it
    /// belonged on TeamComponent (for teams) or will live on a future
    /// unit-class component (for units). See ECS_DECOUPLING_PLAN §3.
    struct CampaignIdentityComponent : Component<CampaignIdentityComponent> {
        int team_id = 0;
        std::string callsign;
    };

    // --- Team component (Phase 1) -----------------------------------------------

    /// Team-specific data. Every team entity carries this. Previously this
    /// data was stuffed into CampaignIdentityComponent (which served two
    /// roles). Now each entity kind gets its own focused component.
    struct TeamComponent : Component<TeamComponent> {
        int slot = 0;                    // 0..7
        uint8_t flags = 0;
        uint8_t colour = 0;
        std::string motto;
        std::vector<int16_t> stance;     // stance toward each other team
        std::vector<uint8_t> member;     // country memberships
        uint8_t air_experience = 0;
        uint8_t ground_experience = 0;
        uint8_t naval_experience = 0;
        uint8_t air_defense_experience = 0;
        int16_t first_colonel = 0;
        int16_t first_commander = 0;
        int16_t first_wingman = 0;
        int16_t last_wingman = 0;
    };

    // --- Objective components (Phase 2b) -----------------------------------------

    /// Classifies the objective type (airbase, bridge, city, ...).
    struct ObjectiveTypeComponent : Component<ObjectiveTypeComponent> {
        int16_t type = 0;               // entity_type (class table index, 100+)
        int16_t class_table_index = 0;
        std::string class_name;         // e.g. "02_20 Airbase 2"
    };

    /// Who owns this objective (current + first/original owner).
    struct OwnershipComponent : Component<OwnershipComponent> {
        uint8_t team = 0;               // current owner (0=neutral, 1=enemy, 2=friendly, ...)
        uint8_t first_owner = 0;        // original owner
    };

    /// Supply/fuel/losses state for objectives that have logistics.
    struct SupplyStateComponent : Component<SupplyStateComponent> {
        uint8_t supply = 0;
        uint8_t fuel = 0;
        uint8_t losses = 0;
        int32_t last_repair = 0;
    };

    /// Per-feature damage bitmap (raw 2-bit-per-feature bitmap from .obj).
    struct DamageBitmapComponent : Component<DamageBitmapComponent> {
        std::vector<uint8_t> fstatus;   // packed 2 bits per feature
    };

    /// Radar detection arcs and range. Only on objectives with radar.
    struct RadarComponent : Component<RadarComponent> {
        float detect_ratio[8] = {0.0f};
        float range_km = 0.0f;
        std::string name;               // e.g. "APG-68", "Pat Hand"
        int16_t radar_type_idx = -1;    // index into Falcon4.RCD
    };

    /// Road/rail network connections to neighboring objectives.
    struct NetworkLinksComponent : Component<NetworkLinksComponent> {
        std::vector<ObjectiveLink> links;
    };

    /// Airbase ground layout (runway/taxiway/parking). Only on airbases.
    struct GroundLayoutComponent : Component<GroundLayoutComponent> {
        std::vector<GroundLayoutList> layouts;
    };

    /// Per-objective feature placements (buildings, structures).
    struct FeatureSetComponent : Component<FeatureSetComponent> {
        uint8_t features_count = 0;
        uint8_t radar_feature = 0;      // which feature provides radar (255=none)
        uint8_t deag_distance = 0;
        uint16_t pt_data_index = 0;
        std::array<uint8_t, 8> objective_detection{};
        std::vector<FeatureEntryState> features;
        // A-G employment (M5): the LIVE per-feature hit-point ledger. Empty
        // = untouched (the common pristine case — nothing initializes it
        // until the first bomb impact, so worlds without ordnance carry
        // zero extra state). On first damage, f4-weapons lazily fills it
        // from each feature's class hit_points (FCD) or a 100-hp default,
        // then decrements it with each blast. FeatureEntryState::
        // damage_state + DamageBitmapComponent::fstatus stay in sync as the
        // wire-visible face of the same ledger (VIS states from f4vu.h:
        // 0 normal, 1 repaired, 2 damaged, 3 destroyed).
        std::vector<double> feature_hp;
    };

    /// Objective priority and name index.
    struct ObjectivePriorityComponent : Component<ObjectivePriorityComponent> {
        uint8_t priority = 0;
        int16_t nameid = 0;             // index into the name table
        uint32_t obj_flags = 0;         // opaque bitmap from .obj
        uint32_t parent_id = 0;         // VU_ID.num of parent objective (0 if none)
    };

    // --- Unit components (Phase 2b) ----------------------------------------------

    /// Core identity shared by all unit types.
    struct UnitCoreComponent : Component<UnitCoreComponent> {
        UnitClass unit_class = UnitClass::Unknown;
        uint8_t domain = 0;             // VU_DOMAIN (2=air, 3=land, 4=sea)
        uint8_t unit_subtype = 0;       // STYPE_UNIT_* (armor/infantry/fighter/bomber/...)
        int16_t class_table_index = 0;  // entity_type from class table
        uint32_t roster = 0;            // packed 2 bits/group x 16 groups
        std::string class_name;         // e.g. "Armor Battalion"
    };

    /// Waypoint plan for units with waypoints.
    struct WaypointPlanComponent : Component<WaypointPlanComponent> {
        std::vector<WaypointState> waypoints;
    };

    /// Ground tactical state for Battalion, Brigade, TaskForce.
    struct GroundTacticalComponent : Component<GroundTacticalComponent> {
        uint8_t supply = 0;
        uint8_t morale = 0;
        uint8_t fatigue = 0;
        uint8_t heading = 0;           // current heading (0-255, *1.4 deg)
        uint8_t final_heading = 0;     // commanded heading
        uint8_t position = 0;          // formation position slot
        int32_t last_move = 0;         // CampaignTime of last move
        int32_t last_combat = 0;       // CampaignTime of last combat
    };

    /// Parent/children hierarchy for Battalion -> Brigade.
    ///
    /// The raw VU_ID fields (parent_id, element_ids) that previously lived
    /// here were removed — they duplicated the resolved EntityId fields
    /// (parent, children) and were never cleared after the bridge's second
    /// pass, creating a permanent "is this a live VU_ID or a stale one?"
    /// ambiguity. The bridge now queries the IUnitSource directly during
    /// the second pass instead of storing raw IDs on the component.
    /// Consumers that need the raw VU_ID (e.g. for display) can look it
    /// up via PopulatedWorld::unit_id_map, which maps VU_ID.num → EntityId.
    struct HierarchyComponent : Component<HierarchyComponent> {
        EntityId parent;                        // resolved EntityId of parent
        std::vector<EntityId> children;         // resolved EntityIds of children
    };

    /// Squadron-specific state.
    ///
    /// airbase_id (raw VU_ID) was removed — use the resolved `airbase`
    /// EntityId field instead. The bridge resolves it during the second
    /// pass; consumers that need the raw VU_ID for display can look it
    /// up via PopulatedWorld::objective_id_map.
    struct SquadronComponent : Component<SquadronComponent> {
        EntityId airbase;                       // resolved EntityId of home airbase
        uint8_t specialty = 0;
        int16_t aa_kills = 0;
        int16_t ag_kills = 0;
        int16_t as_kills = 0;
        int16_t an_kills = 0;
        int16_t missions_flown = 0;
        int16_t mission_score = 0;
        uint8_t total_losses = 0;
        uint8_t pilot_losses = 0;
        uint8_t squadron_patch = 0;
        int32_t fuel = 0;
        std::vector<PilotState> pilots;
    };

    /// Flight-specific state (mission element within a package).
    ///
    /// package_id / squadron_id (raw VU_IDs) were removed — use the
    /// resolved `package` / `squadron` EntityId fields instead.
    struct FlightPlanComponent : Component<FlightPlanComponent> {
        float altitude = 0.0f;                  // flight altitude (feet)
        int32_t fuel_burnt = 0;
        int32_t time_on_target = 0;             // CampaignTime
        int32_t mission_over_time = 0;          // CampaignTime
        int16_t mission_target = 0;             // target ID slot
        uint8_t loadouts = 0;                   // # of loadout entries
        uint8_t mission = 0;                    // MissionType enum
        uint8_t flight_priority = 0;
        uint8_t mission_id = 0;
        uint8_t eval_flags = 0;
        uint8_t callsign_id = 0;
        uint8_t callsign_num = 0;
        // Cross-references (resolved in bridge second pass)
        EntityId package;                       // resolved EntityId of parent Package
        EntityId squadron;                      // resolved EntityId of owning Squadron
        /// Resolved EntityId of the mission target — the objective (or, for
        /// unit-targeted missions like CAS/BAI, the unit) whose VU_ID.num is
        /// `mission_target`. Invalid (0) when the flight carries no target or
        /// the target VU_ID doesn't resolve. Campaign-QC rendering and the
        /// B.3 spawner's route builder both consume this. (B.3 tranche)
        EntityId target;

    /// Decoded loadout stations (wire weapon ids + counts, entry 0 of the
    /// save's LoadoutStruct[]). The campaign bridge maps these onto the
    /// engine's WeaponStoreComponent at spawn: stations with a mapped
    /// engine class become droppable, the rest ride as bookkeeping with a
    /// "WPN-<id>" label. (A-G employment tranche)
    std::vector<LoadoutStationState> loadout_stations;
    };

    /// Mission request carried by a Package unit — the ATM's "please build a
    /// package for this" worklist entry that produced the package. Decoded
    /// from the package JSON's `mis_request` block (mission, tot, priority,
    /// action_type, target_num, target_creator, requester_num). Present only
    /// when the save actually carried a request (v71 saves do; synthetic
    /// fixtures may not). (B.3 tranche)
    struct PackageMissionRequest {
        bool present = false;
        uint8_t mission = 0;                    // MissionType enum
        int32_t tot = 0;                        // CampaignTime (absolute)
        uint8_t priority = 0;
        uint16_t action_type = 0;
        uint32_t target_num = 0;                // raw VU_ID.num
        uint32_t target_creator = 0;            // raw VU_ID.creator
        uint32_t requester_num = 0;             // raw VU_ID.num
        // Cross-references (resolved in bridge second pass)
        EntityId target;                        // resolved from target_num
        EntityId requester;                     // resolved from requester_num
    };

    /// Package-specific state (groups multiple Flights).
    ///
    /// The raw VU_ID fields (interceptor_id, awacs_id, jstar_id, ecm_id,
    /// tanker_id) were removed — use the resolved EntityId fields instead.
    struct PackageSupportComponent : Component<PackageSupportComponent> {
        uint8_t wait_cycles = 0;
        // Cross-references (resolved in bridge second pass)
        EntityId interceptor;                   // resolved EntityId
        EntityId awacs;
        EntityId jstar;
        EntityId ecm;
        EntityId tanker;
        /// The package's element flights, in wire order. Resolved from the
        /// package unit's element_ids[] VU_ID vector during the bridge second
        /// pass — same mechanism as Brigade→children, but for air packages.
        /// The Flights also carry the reverse link (FlightPlanComponent::
        /// package), so consumers can walk either direction. (B.3 tranche)
        std::vector<EntityId> elements;
        /// The ATM mission request that produced this package, when present.
        PackageMissionRequest request;
    };

    /// Vehicle composition for units with vehicle groups.
    struct VehicleCompositionComponent : Component<VehicleCompositionComponent> {
        std::vector<VehicleGroup> groups;
    };

    /// Per-mission-role scoring (16 uchar values from Falcon4.UCD.Scores).
    struct UnitClassScoreComponent : Component<UnitClassScoreComponent> {
        std::array<uint8_t, 16> scores{};
    };

    // --- Damage / vitality (combat chain, see Docs/COMBAT_CHAIN_PLAN.md) ----

    /// Hit-point state for any damageable entity (aircraft, vehicle, ship).
    ///
    /// Objective FEATURES use the 2-bit-per-feature DamageBitmapComponent
    /// (the .obj fstatus array); THIS component is the entity-level
    /// counterpart for units/aircraft — the thing a missile or gun round
    /// actually damages.
    ///
    /// f4-weapons writes it (apply_damage -> killed transition +
    /// EntityKilledMessage); f4-world will populate hit_points from the VCD
    /// per-vehicle hit_points field. "Killed" is a component transition, not
    /// an entity destroy: what death means (wreckage, removal, campaign
    /// attrition) belongs to higher layers.
    struct DamageStateComponent : Component<DamageStateComponent> {
        double   hit_points    = 0.0;
        double   max_hit_points = 0.0;   // nominal (VCD hit_points) value
        bool     killed       = false;
        uint64_t killed_by    = 0;       // EntityId::value of the shooter (0 = unknown)
        uint64_t killed_at_tick = 0;     // killer-system-local tick of the killing blow
                                         // (f4-weapons stores the missile's flown ticks)
    };

    /// Movement orders for ground units (Battalion / Brigade / TaskForce).
    ///
    /// Promoted from PropertyBag residue (Phase 5 cleanup). Previously these
    /// fields were dumped into PropertyBag with hardcoded string keys
    /// ("dest_x", "dest_y", "movement_speed", "max_range", "movement_type",
    /// "movement_type_name"). They have clear domain meaning — they describe
    /// where a ground unit is ordered to move and how fast it can get there —
    /// so they deserve a typed component.
    ///
    /// Conditionally added: only when at least one field is non-zero/non-empty.
    struct MovementOrdersComponent : Component<MovementOrdersComponent> {
        int16_t dest_x = 0;             // destination grid X
        int16_t dest_y = 0;             // destination grid Y
        int32_t movement_type = 0;      // MoveType enum (from Falcon4.UCD)
        int16_t movement_speed = 0;     // max speed (units TBD by MoveType)
        int16_t max_range = 0;          // max travel range
        std::string movement_type_name; // human-readable MoveType name
    };

    // --- Utility components (Phase 2b) -------------------------------------------

    /// Typed key-value bag for unstable/reverse-engineering fields. Fields
    /// that vary across Falcon versions or lack a clear domain meaning go
    /// here. Once proven, they are promoted to proper components.
    struct PropertyBag : Component<PropertyBag> {
        std::unordered_map<std::string, int64_t>    ints;
        std::unordered_map<std::string, double>     floats;
        std::unordered_map<std::string, std::string> strings;
    };

    /// Campaign-level time and TE (Tactical Engagement) state.
    struct CampaignStateComponent : Component<CampaignStateComponent> {
        int32_t current_time = 0;
        int32_t te_start_time = 0;
        int32_t te_time_limit = 0;
        int32_t te_victory_points = 0;
        int32_t te_type = 0;
        int32_t te_number_teams = 0;
        int32_t te_team = 0;
        int32_t te_flags = 0;
        std::vector<int32_t> te_number_aircraft;
        std::vector<int32_t> te_team_pts;
        /// B.3 QC tranche — the campaign's shared bullseye reference point
        /// (grid coordinates; the viewer draws it as a crosshair).
        int32_t bullseye_x = 0;
        int32_t bullseye_y = 0;
        int32_t bullseye_name = 0;
    };

    // ============================================================================
    // EntityWorld — owns all entities, their components, and tags.
    // ============================================================================
    // EntityHandle forward-declared earlier (before BehavioralComponentBase)
    // so on_attached() can take it by reference.

    class EntityWorld {
    public:
        EntityWorld();

        // Non-copyable (holds unique_ptrs); movable.
        EntityWorld(const EntityWorld&) = delete;
        EntityWorld& operator=(const EntityWorld&) = delete;

        // Custom move constructor: regenerates the cookie so that
        // EntityHandles captured against the source (moved-from) world
        // fail validation against the destination. The defaulted move
        // ctor would copy the cookie, defeating the use-after-free
        // detection that the cookie is there to provide.
        //
        // We deliberately do NOT invalidate handles captured against the
        // source BEFORE the move — those handles already hold the source's
        // cookie, and after the move the source's destructor runs (or its
        // cookie becomes stale), so the handles correctly fail validation
        // against the destination. The new cookie simply ensures handles
        // captured against the destination are bound to ITS cookie, not
        // the source's.
        EntityWorld(EntityWorld&& other) noexcept
            : entities_(std::move(other.entities_))
            , free_list_(std::move(other.free_list_))
            , tag_index_(std::move(other.tag_index_))
            , component_index_(std::move(other.component_index_))
            , cookie_(detail::next_world_cookie())
        {
            // The moved-from world no longer owns the component objects its
            // cache points at — clear it so a stray update_all() on the
            // source rebuilds (into an empty world) instead of walking
            // dangling pointers. The destination rebuilds lazily.
            other.behavioral_cache_.clear();
            other.behavioral_cache_dirty_ = true;
            behavioral_cache_dirty_ = true;
        }

        // Move assignment: same reasoning — regenerate the cookie so old
        // handles against `*this` (before the assignment) don't accidentally
        // validate against the new contents.
        EntityWorld& operator=(EntityWorld&& other) noexcept {
            if (this != &other) {
                entities_   = std::move(other.entities_);
                free_list_  = std::move(other.free_list_);
                tag_index_  = std::move(other.tag_index_);
                component_index_ = std::move(other.component_index_);
                cookie_     = detail::next_world_cookie();
                // Both sides' caches are now stale (this: old components
                // destroyed; other: nodes transferred here). See move ctor.
                behavioral_cache_.clear();
                behavioral_cache_dirty_ = true;
                other.behavioral_cache_.clear();
                other.behavioral_cache_dirty_ = true;
            }
            return *this;
        }

        [[nodiscard]] EntityHandle create();
        void destroy(EntityId id);

        [[nodiscard]] bool alive(EntityId id) const noexcept;

        // --- Tag queries ---
        // with_tag() returns a COPY of the matching entity IDs. Use this when
        // you need to iterate without holding a reference into the world, or
        // when you might mutate the world during iteration (the copy is a
        // snapshot).
        [[nodiscard]] std::vector<EntityId> with_tag(const TagKey& key, const TagValue& value) const;

        // with_tag_ref() returns a CONST REFERENCE to the internal vector of
        // matching entity IDs — O(1) lookup, zero allocation, zero copy.
        //
        // Phase D: this is the fast path that the per-frame render loops
        // should use. The reference is valid only until the next mutation
        // that touches this (key, value) bucket — i.e. the next set_tag()
        // that adds or removes an entity from this bucket, or the next
        // destroy() of an entity in this bucket. Callers that need a
        // stable snapshot across a mutation should use with_tag() (copy)
        // instead.
        //
        // If no entities match, returns a reference to a stable empty vector
        // (so the caller can always iterate without a null check).
        [[nodiscard]] const std::vector<EntityId>& with_tag_ref(const TagKey& key, const TagValue& value) const;

        [[nodiscard]] std::vector<EntityId> query(std::function<bool(const TagSet&)> pred) const;

        // --- Component queries ---
        template<typename T>
        [[nodiscard]] std::vector<EntityId> with_component() const;

        // --- Spatial queries (forwarded to SpatialIndex when present) ---
        [[nodiscard]] std::vector<EntityId> within_radius(double cx, double cy, double cz,
            double radius) const;

        // --- Sim tick primitive ---
        // Calls update(dt, bus) on every behavioral component of every live
        // entity, in two passes:
        //   Pass 1: components with priority() >= update_phase::BRAIN_THRESHOLD
        //           (brains — produce control inputs).
        //   Pass 2: components with 0 < priority() < BRAIN_THRESHOLD
        //           (physics — consume the brain's outputs).
        // Components with priority() == 0 (passive data: TransformComponent,
        // TeamComponent, ...) are skipped.
        //
        // Threading: NOT thread-safe. Call from the sim thread only.
        // Bus flushing: this call does NOT call bus.flush_pending(). The
        // caller is responsible for draining deferred messages after the
        // tick completes (typical pattern: update_all(dt, bus);
        // bus.flush_pending();).
        void update_all(double dt, messaging::MessageBus& bus);

        [[nodiscard]] std::size_t size() const noexcept { return entities_.size(); }
        [[nodiscard]] std::size_t capacity() const noexcept { return entities_.capacity(); }

    private:
        friend class EntityHandle;

        struct EntityRecord {
                        EntityRecord() = default;

            TagSet tags;
            std::unordered_map<std::type_index, std::unique_ptr<ComponentBase>> components;
            uint32_t generation = 0;
            bool alive = false;

            // unique_ptr members make this non-copyable. Explicitly delete
            // copy so MSVC does not attempt to instantiate the unordered_map
            // copy constructor (which would try to copy unique_ptr).
            EntityRecord(const EntityRecord&) = delete;
            EntityRecord& operator=(const EntityRecord&) = delete;
            EntityRecord(EntityRecord&&) = default;
            EntityRecord& operator=(EntityRecord&&) = default;
        };

        std::vector<EntityRecord> entities_;
        std::vector<uint32_t> free_list_;   // indices of dead slots, LIFO

        // ── Phase D: per-tag-value index ──────────────────────────────────
        //
        // Maps (TagKey → TagValue → vector<EntityId>). Maintained incrementally
        // by set_tag() and destroy(). Lets with_tag_ref() return a const-ref
        // to the matching vector in O(1) (two hash lookups) instead of the
        // O(N) linear scan the old with_tag() did.
        //
        // The vectors store EntityId (a 64-bit value: index | generation).
        // Removal is O(n) in the bucket size, but tag mutations are rare
        // (set once at load time, destroyed basically never in the viewer),
        // so this is a deliberate trade: cheap queries, slightly more
        // expensive mutations.
        //
        // Invariants:
        //   1. If entity E has tag K=V, then tag_index_[K][V] contains E's
        //      EntityId exactly once.
        //   2. If entity E does NOT have tag K=V, then E's EntityId is NOT
        //      in tag_index_[K][V].
        //   3. Destroyed entities are removed from every bucket they were in.
        //   4. When set_tag() overwrites K=V_old with K=V_new on entity E,
        //      E is removed from tag_index_[K][V_old] and added to
        //      tag_index_[K][V_new].
        //
        // The empty_buckets_ vector is a stable home for empty result vectors
        // returned by with_tag_ref() when no entities match — so the caller
        // can always iterate without a null check. (We can't return a reference
        // to a temporary, and creating a static empty vector per call would
        // be a small but real allocation. One shared empty vector per world
        // is the cheapest correct answer.)
        using TagValueIndex = std::unordered_map<TagValue, std::vector<EntityId>, TagValueHash>;
        std::unordered_map<TagKey, TagValueIndex, TagKeyHash> tag_index_;
        std::vector<EntityId> empty_buckets_;  // stable empty vector for misses

        // ── Behavioral-component cache (campaign-scale update_all) ────────
        //
        // update_all() used to dynamic_cast every component of every entity
        // twice per tick. At campaign scale (a populated save: ~4,374
        // entities, ~5 components each) that was ~52,000 dynamic_casts +
        // hash-map walks per tick — ~36 ms, dwarfing the actual simulation
        // work. The cache holds one flat vector of the behavioral components
        // (visit order = entity index order, then component-map order, the
        // same order the uncached loop walked), rebuilt lazily:
        //
        //   * add<T>() / remove<T>() where T derives from
        //     BehavioralComponentBase -> invalidate_behavioral_cache()
        //   * create() / destroy()                    -> invalidate
        //   * move ctor/assign                         -> clear + invalidate
        //
        // Raw pointers are safe because component objects live in heap
        // nodes owned by each EntityRecord's component map: destroying a
        // component is only possible through remove<T>()/destroy() (both
        // invalidate), and growing entities_ moves the records but
        // transfers the map nodes (element addresses stable).
        //
        // THE invariant this relies on (already required by the uncached
        // implementation): the world is NEVER mutated during update_all()
        // itself — entity creation/destruction and component mutation
        // happen between ticks (deferred bus messages are flushed after
        // update_all returns; missile spawns happen outside the loop; see
        // Simulation::tick). If that invariant is ever broken, both the
        // cached and the uncached version are UB; the cache adds no new
        // requirement.
        std::vector<BehavioralComponentBase*> behavioral_cache_;
        bool behavioral_cache_dirty_ = true;

        void invalidate_behavioral_cache() noexcept { behavioral_cache_dirty_ = true; }
        void rebuild_behavioral_cache();

        // ── Component-type index (with_component at campaign scale) ──────
        //
        // Maps type_index → the ids of live entities carrying that
        // component. Built lazily per type on the first with_component<T>()
        // query (untouched types never build), then maintained
        // incrementally:
        //   * add<T>()     → append when order allows, else drop the bucket
        //                    for a lazy rebuild (slot reuse can insert out
        //                    of entity-index order)
        //   * remove<T>()  → erase the id from the bucket
        //   * destroy()    → erase the id from EVERY bucket its entity had
        //                    components in
        //   * move ops     → moved along with entities_ (ids are values,
        //                    not pointers — no dangling)
        //
        // with_component<T>() collapses from an O(entities × components)
        // walk (a populated save: ~4,400 entities × ~5 map buckets, six
        // such queries per Simulation::tick — the profiler measured ~7 of
        // the ~11.5 ms/tick in the A-G QC run) to one hash lookup plus a
        // copy of the bucket. The copy preserves the snapshot-by-value
        // contract callers rely on (sweeps destroy entities while
        // iterating the returned vector).
        //
        // Invariants (mirror the tag index's):
        //   1. If bucket B for type T exists, B contains exactly the live
        //      entities carrying T, in entity-index order (the order the
        //      uncached scan walked).
        //   2. A bucket is either correct or absent (never stale).
        //   3. create() adds no components → no maintenance needed.
        // Threading: same rule as update_all — sim thread only.
        mutable std::unordered_map<std::type_index, std::vector<EntityId>>
            component_index_;

        // Index maintenance (called from EntityHandle::add/remove — the
        // friend declaration covers them — and from destroy()).
        void component_index_on_add(std::type_index tid, EntityId id,
                                    bool replacing);
        void component_index_on_remove(std::type_index tid, EntityId id);

        // World cookie: a random 64-bit value generated at EntityWorld construction.
        // EntityHandle captures this cookie at creation. If the EntityWorld is
        // destroyed and a new one happens to be allocated at the same address,
        // the cookie will differ and EntityHandle::valid() will return false,
        // catching the dangling-pointer bug. This is cheaper than shared_ptr/
        // weak_ptr (no allocation, no refcount) and catches the common case.
        uint64_t cookie_;

        [[nodiscard]] const EntityRecord* find(EntityId id) const noexcept;
        [[nodiscard]] EntityRecord* find(EntityId id) noexcept;

        // --- Tag index maintenance (called by set_tag / destroy) ---
        // Add id to the (key, value) bucket. Called after a tag is set.
        void index_tag_add(const TagKey& key, const TagValue& value, EntityId id);
        // Remove id from the (key, value) bucket. Called before a tag is
        // overwritten or when an entity is destroyed.
        void index_tag_remove(const TagKey& key, const TagValue& value, EntityId id);
    };

    // ============================================================================
    // EntityHandle — convenience view onto an entity in a world.
    // ============================================================================
    class EntityHandle {
    public:
        EntityHandle() = default;
        EntityHandle(EntityId id, EntityWorld* world)
            : id_(id), world_(world), cookie_(world ? world->cookie_ : 0) {}

        [[nodiscard]] bool valid() const noexcept;
        [[nodiscard]] EntityId id() const noexcept { return id_; }

        // Accessor for the owning world. Used by behavioral components
        // that need to pass the EntityWorld to subsystems that subscribe
        // to the message bus or query other entities (e.g. BrainComponent
        // passes the world to TakeoffModule::initialize so the module can
        // be wired into the broader sim). Returns nullptr if the handle
        // was default-constructed (not bound to any world).
        [[nodiscard]] EntityWorld* world() noexcept { return world_; }
        [[nodiscard]] const EntityWorld* world() const noexcept { return world_; }

        // --- Components ---
        template<typename T> [[nodiscard]] T* get() const;
        template<typename T> [[nodiscard]] T& require() const;
        template<typename T> [[nodiscard]] bool has() const;
        template<typename I> [[nodiscard]] I* get_interface() const;
        template<typename T, typename... Args> T& add(Args&&... args);
        template<typename T> void remove();

        // --- Tags ---
        void set_tag(const TagKey& key, TagValue value);
        [[nodiscard]] std::optional<TagValue> get_tag(const TagKey& key) const;
        [[nodiscard]] bool has_tag(const TagKey& key) const;

    private:
        EntityId id_{};
        EntityWorld* world_ = nullptr;
        uint64_t cookie_ = 0;  // captures world_->cookie_ at construction
    };

    // ============================================================================
    // Template implementations (header for inlining / explicit instantiation).
    // ============================================================================
    template<typename T>
    std::vector<EntityId> EntityWorld::with_component() const {
        const auto tid = std::type_index(typeid(T));
        auto it = component_index_.find(tid);
        if (it == component_index_.end()) {
            // Lazy per-type build: one walk in entity-index order — the
            // exact order (and result set) the uncached scan produced.
            std::vector<EntityId> bucket;
            for (uint32_t i = 0; i < entities_.size(); ++i) {
                const auto& rec = entities_[i];
                if (rec.alive && rec.components.count(tid)) {
                    bucket.push_back(EntityId::make(i, rec.generation));
                }
            }
            it = component_index_.emplace(tid, std::move(bucket)).first;
        }
        return it->second;  // copy — the snapshot contract the scan offered
    }

    template<typename T>
    T* EntityHandle::get() const {
        if (!world_) return nullptr;
        auto* rec = world_->find(id_);
        if (!rec) return nullptr;
        const auto tid = std::type_index(typeid(T));
        auto it = rec->components.find(tid);
        if (it == rec->components.end()) return nullptr;
        return static_cast<T*>(it->second.get());
    }

    template<typename T>
    T& EntityHandle::require() const {
        T* p = get<T>();
        if (!p) throw std::runtime_error("EntityHandle::require: component missing");
        return *p;
    }

    template<typename T>
    bool EntityHandle::has() const {
        return get<T>() != nullptr;
    }

    /// Interface-based component lookup. Scans all components on this
    /// entity and returns the first one that implements interface I
    /// (detected via dynamic_cast). Returns nullptr if none matches.
    ///
    /// This enables BrainComponent to find IAircraftState and
    /// IPilotInputSink without depending on the concrete
    /// FlightModelComponent type — a key decoupling mechanism.
    ///
    /// Complexity: O(n) where n is the number of components on this
    /// entity (typically 3-5 at Phase A scale). For O(1) lookup,
    /// use get<T>() with the concrete type instead.
    ///
    /// Requires RTTI (enabled by default in C++20; the entity system
    /// already uses std::type_index and typeid()).
    template<typename I>
    I* EntityHandle::get_interface() const {
        if (!world_) return nullptr;
        auto* rec = world_->find(id_);
        if (!rec) return nullptr;
        for (const auto& [tid, comp] : rec->components) {
            if (auto* iface = dynamic_cast<I*>(comp.get())) {
                return iface;
            }
        }
        return nullptr;
    }

    template<typename T, typename... Args>
    T& EntityHandle::add(Args&&... args) {
        if (!world_) throw std::runtime_error("EntityHandle::add: no world");
        auto* rec = world_->find(id_);
        if (!rec) throw std::runtime_error("EntityHandle::add: invalid entity");
        auto comp = std::make_unique<T>(std::forward<Args>(args)...);
        T& ref = *comp;
        const auto tid = std::type_index(typeid(T));
        const bool replacing = rec->components.count(tid) != 0;
        // For behavioral components, fire on_attached() BEFORE we move the
        // unique_ptr into the components map. `self` ALIASES THE CALLER'S
        // EntityHandle (usually a stack local in spawn code) — components
        // that want a back-reference must COPY it (EntityHandle is a cheap
        // value type: id + world pointer + cookie). Storing &self would
        // dangle as soon as the spawning function returns. The component is
        // still owned by the local unique_ptr at this point, so a throw
        // from on_attached() will cleanly destroy it without leaving a
        // half-registered entry in the map.
        if constexpr (std::is_base_of_v<BehavioralComponentBase, T>) {
            comp->on_attached(*this);
        }
        rec->components[tid] = std::move(comp);
        if constexpr (std::is_base_of_v<BehavioralComponentBase, T>) {
            // New behavioral component entered the world — the cache must
            // be rebuilt before the next update_all() (see cache notes).
            world_->invalidate_behavioral_cache();
        }
        // Component-type index maintenance (no-op until the type has been
        // queried once; see the index notes in EntityWorld).
        world_->component_index_on_add(tid, id_, replacing);
        return ref;
    }

    template<typename T>
    void EntityHandle::remove() {
        if (!world_) return;
        auto* rec = world_->find(id_);
        if (!rec) return;
        const auto tid = std::type_index(typeid(T));
        rec->components.erase(tid);
        world_->component_index_on_remove(tid, id_);
        if constexpr (std::is_base_of_v<BehavioralComponentBase, T>) {
            // A behavioral component left the world (its unique_ptr was
            // just destroyed) — the cache's pointer now dangles until
            // rebuilt, so flag the rebuild now.
            world_->invalidate_behavioral_cache();
        }
    }

} // namespace f4::entities