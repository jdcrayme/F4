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

#pragma once

#include <cstdint>
#include <compare>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <f4/geo/position.hpp>

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
    // ============================================================================
    struct TagKey {
        std::string name;
        explicit TagKey(std::string n) : name(std::move(n)) {}
        TagKey(const char* n) : name(n) {}
        auto operator<=>(const TagKey&) const = default;
    };

    struct TagValue {
        enum class Type { String, Int, Float, Bool };
        Type type = Type::Bool;
        std::string str_val;
        int64_t int_val = 0;
        double float_val = 0.0;
        bool bool_val = false;

        static TagValue from(std::string s) { TagValue v; v.type = Type::String; v.str_val = std::move(s); return v; }
        static TagValue from(int64_t i) { TagValue v; v.type = Type::Int;    v.int_val = i;            return v; }
        static TagValue from(double f) { TagValue v; v.type = Type::Float;  v.float_val = f;          return v; }
        static TagValue from(bool b) { TagValue v; v.type = Type::Bool;   v.bool_val = b;           return v; }

        auto operator<=>(const TagValue&) const = default;
    };

} // namespace f4::entities

// std::hash specialization for TagKey — required for unordered_map<TagKey,_>.
// Must be in namespace std and visible before TagSet is defined below.
namespace std {
    template<>
    struct hash<f4::entities::TagKey> {
        std::size_t operator()(const f4::entities::TagKey& k) const noexcept {
            return std::hash<std::string>{}(k.name);
        }
    };
} // namespace std

namespace f4::entities {

    using TagSet = std::unordered_map<TagKey, TagValue>;

    namespace tags {
        inline constexpr const char* ROLE = "role";    // "fighter","bomber","tanker","awacs",...
        inline constexpr const char* TEAM = "team";    // "red","blue"
        //inline constexpr const char* DOMAIN = "domain";  // "air","ground","naval"
        inline constexpr const char* ALIVE = "alive";   // bool
        inline constexpr const char* STEALTH = "stealth"; // bool
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
    struct CampaignIdentityComponent : Component<CampaignIdentityComponent> {
        int team_id = 0;
        std::string unit_type_name;   // e.g. "F-16C_50"
        std::string callsign;
    };

    // ============================================================================
    // EntityWorld — owns all entities, their components, and tags.
    // ============================================================================
    class EntityHandle;

    class EntityWorld {
    public:
        EntityWorld() = default;

        // Non-copyable (holds unique_ptrs); movable.
        EntityWorld(const EntityWorld&) = delete;
        EntityWorld& operator=(const EntityWorld&) = delete;
        EntityWorld(EntityWorld&&) noexcept = default;
        EntityWorld& operator=(EntityWorld&&) noexcept = default;

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

        [[nodiscard]] const EntityRecord* find(EntityId id) const noexcept;
        [[nodiscard]] EntityRecord* find(EntityId id) noexcept;
    };

    // ============================================================================
    // EntityHandle — convenience view onto an entity in a world.
    // ============================================================================
    class EntityHandle {
    public:
        EntityHandle() = default;
        EntityHandle(EntityId id, EntityWorld* world) : id_(id), world_(world) {}

        [[nodiscard]] bool valid() const noexcept;
        [[nodiscard]] EntityId id() const noexcept { return id_; }

        // --- Components ---
        template<typename T> [[nodiscard]] T* get() const;
        template<typename T> [[nodiscard]] T& require() const;
        template<typename T> [[nodiscard]] bool has() const;
        template<typename T, typename... Args> T& add(Args&&... args);
        template<typename T> void remove();

        // --- Tags ---
        void set_tag(const TagKey& key, TagValue value);
        [[nodiscard]] std::optional<TagValue> get_tag(const TagKey& key) const;
        [[nodiscard]] bool has_tag(const TagKey& key) const;

    private:
        EntityId id_{};
        EntityWorld* world_ = nullptr;
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

    template<typename T, typename... Args>
    T& EntityHandle::add(Args&&... args) {
        if (!world_) throw std::runtime_error("EntityHandle::add: no world");
        auto* rec = world_->find(id_);
        if (!rec) throw std::runtime_error("EntityHandle::add: invalid entity");
        auto comp = std::make_unique<T>(std::forward<Args>(args)...);
        T& ref = *comp;
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