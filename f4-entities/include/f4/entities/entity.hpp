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

    namespace tags {
        inline constexpr const char* ROLE = "role";    // "fighter","bomber","tanker","awacs",...
        inline constexpr const char* TEAM = "team";    // "red","blue"
        inline constexpr const char* OPDOMAIN = "domain";  // "air","ground","naval"
        inline constexpr const char* ALIVE = "alive";   // bool
        inline constexpr const char* STEALTH = "stealth"; // bool
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
    // every brain on every entity has already published its commands. This
    // is the simplest correct ordering; the eventual optimization (campaign
    // scale, N=1000+) is to cache a priority-sorted vector of behavioral
    // component pointers per EntityRecord, avoiding the dynamic_cast in
    // update_all(). At Phase A scale (N=1-4 entities, ~3 components each)
    // the dynamic_cast is invisible in profiles.
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
    };

    /// Vehicle composition for units with vehicle groups.
    struct VehicleCompositionComponent : Component<VehicleCompositionComponent> {
        std::vector<VehicleGroup> groups;
    };

    /// Per-mission-role scoring (16 uchar values from Falcon4.UCD.Scores).
    struct UnitClassScoreComponent : Component<UnitClassScoreComponent> {
        std::array<uint8_t, 16> scores{};
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
            , cookie_(detail::next_world_cookie())
        {}

        // Move assignment: same reasoning — regenerate the cookie so old
        // handles against `*this` (before the assignment) don't accidentally
        // validate against the new contents.
        EntityWorld& operator=(EntityWorld&& other) noexcept {
            if (this != &other) {
                entities_  = std::move(other.entities_);
                free_list_ = std::move(other.free_list_);
                cookie_    = detail::next_world_cookie();
            }
            return *this;
        }

        [[nodiscard]] EntityHandle create();
        void destroy(EntityId id);

        [[nodiscard]] bool alive(EntityId id) const noexcept;

        // --- Tag queries ---
        [[nodiscard]] std::vector<EntityId> with_tag(const TagKey& key, const TagValue& value) const;
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

        // World cookie: a random 64-bit value generated at EntityWorld construction.
        // EntityHandle captures this cookie at creation. If the EntityWorld is
        // destroyed and a new one happens to be allocated at the same address,
        // the cookie will differ and EntityHandle::valid() will return false,
        // catching the dangling-pointer bug. This is cheaper than shared_ptr/
        // weak_ptr (no allocation, no refcount) and catches the common case.
        uint64_t cookie_;

        [[nodiscard]] const EntityRecord* find(EntityId id) const noexcept;
        [[nodiscard]] EntityRecord* find(EntityId id) noexcept;
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
        std::vector<EntityId> out;
        const auto tid = std::type_index(typeid(T));
        for (uint32_t i = 0; i < entities_.size(); ++i) {
            const auto& rec = entities_[i];
            if (rec.alive && rec.components.count(tid)) {
                out.push_back(EntityId::make(i, rec.generation));
            }
        }
        return out;
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
        // For behavioral components, fire on_attached() BEFORE we move the
        // unique_ptr into the components map. This gives the component a
        // stable back-reference to its owning EntityHandle (via `*this`)
        // so it can look up sibling components (e.g. brain -> flight model)
        // during subsequent update() calls. The component is still owned
        // by the local unique_ptr at this point, so a throw from
        // on_attached() will cleanly destroy it without leaving a half-
        // registered entry in the map.
        if constexpr (std::is_base_of_v<BehavioralComponentBase, T>) {
            comp->on_attached(*this);
        }
        rec->components[std::type_index(typeid(T))] = std::move(comp);
        return ref;
    }

    template<typename T>
    void EntityHandle::remove() {
        if (!world_) return;
        auto* rec = world_->find(id_);
        if (!rec) return;
        rec->components.erase(std::type_index(typeid(T)));
    }

} // namespace f4::entities