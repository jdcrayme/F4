// f4-entities/entity.cpp — non-template implementations.

#include <f4/entities/entity.hpp>
#include <f4/entities/spatial_index.hpp>

#include <algorithm>
#include <cmath>

namespace f4::entities {

// ============================================================================
// EntityWorld
// ============================================================================

// Global atomic counter for generating unique world cookies.
// Each EntityWorld gets a unique cookie at construction, so EntityHandle
// can detect use-after-free if a new world is allocated at the same
// address as a destroyed one.
static std::atomic<uint64_t> g_next_world_cookie{1};

// Used by EntityWorld's custom move ctor/assign to regenerate the cookie
// on the destination (declared as a friend in entity.hpp so it can call
// the static counter). Kept out-of-line here so the cookie counter stays
// file-local — no other TU can mint cookies.
uint64_t f4::entities::detail::next_world_cookie() noexcept {
    return g_next_world_cookie.fetch_add(1, std::memory_order_relaxed);
}

EntityWorld::EntityWorld()
    : cookie_(g_next_world_cookie.fetch_add(1, std::memory_order_relaxed)) {}

EntityHandle EntityWorld::create() {
    uint32_t index;
    if (!free_list_.empty()) {
        index = free_list_.back();
        free_list_.pop_back();
        auto& rec = entities_[index];
        ++rec.generation;           // bump generation so stale handles fail
        rec.alive = true;
        rec.tags.clear();           // Phase D: index maintenance happens via set_tag()
        rec.components.clear();     // (slot reuse: old components were destroyed
                                    // by destroy(), which already invalidated
                                    // the behavioral cache)
    } else {
        index = static_cast<uint32_t>(entities_.size());
        EntityRecord rec;
        rec.generation = 1;         // generation 0 is reserved (null id)
        rec.alive = true;
        entities_.push_back(std::move(rec));
    }
    // A fresh entity has no components, so the behavioral set is unchanged;
    // flag the rebuild anyway — one bool write, and it keeps the cache's
    // invariants trivially true regardless of future changes here.
    invalidate_behavioral_cache();
    return EntityHandle(EntityId::make(index, entities_[index].generation), this);
}

void EntityWorld::destroy(EntityId id) {
    auto* rec = find(id);
    if (!rec || !rec->alive) return;
    // Phase D: remove this entity from every tag bucket it's in, so the
    // index stays consistent and with_tag_ref() never returns a destroyed
    // EntityId. We walk rec->tags (the per-entity tag map) and remove id
    // from the corresponding (key, value) bucket in tag_index_.
    for (const auto& [key, value] : rec->tags) {
        index_tag_remove(key, value, id);
    }
    // Component-type index: same treatment for every component bucket the
    // entity is in (BEFORE the components map is cleared — the walk needs
    // the types it carries). Unbuilt types are skipped inside.
    for (const auto& [tid, comp] : rec->components) {
        component_index_on_remove(tid, id);
    }
    rec->alive = false;
    rec->tags.clear();
    rec->components.clear();       // behavioral components destroyed — the
                                   // cache's pointers dangle until rebuilt
    invalidate_behavioral_cache();
    free_list_.push_back(id.index());
}

bool EntityWorld::alive(EntityId id) const noexcept {
    const auto* rec = find(id);
    return rec && rec->alive;
}

const EntityWorld::EntityRecord* EntityWorld::find(EntityId id) const noexcept {
    if (!id.valid()) return nullptr;
    const uint32_t idx = id.index();
    if (idx >= entities_.size()) return nullptr;
    const auto& rec = entities_[idx];
    if (rec.generation != id.generation() || !rec.alive) return nullptr;
    return &rec;
}

EntityWorld::EntityRecord* EntityWorld::find(EntityId id) noexcept {
    return const_cast<EntityRecord*>(static_cast<const EntityWorld*>(this)->find(id));
}

std::vector<EntityId> EntityWorld::with_tag(const TagKey& key, const TagValue& value) const {
    // Phase D: delegate to the O(1) index lookup and copy the result.
    // The old O(N) linear scan is gone; this now costs one outer hash lookup
    // + one inner hash lookup + one vector copy. For hot-path callers that
    // don't need a copy, use with_tag_ref() directly.
    return with_tag_ref(key, value);
}

const std::vector<EntityId>& EntityWorld::with_tag_ref(const TagKey& key, const TagValue& value) const {
    // O(1) lookup: two hash finds. If either level misses, return a stable
    // empty vector so the caller can iterate without a null check.
    auto kit = tag_index_.find(key);
    if (kit == tag_index_.end()) return empty_buckets_;
    auto vit = kit->second.find(value);
    if (vit == kit->second.end()) return empty_buckets_;
    return vit->second;
}

std::vector<EntityId> EntityWorld::query(std::function<bool(const TagSet&)> pred) const {
    std::vector<EntityId> out;
    for (uint32_t i = 0; i < entities_.size(); ++i) {
        const auto& rec = entities_[i];
        if (rec.alive && pred(rec.tags)) {
            out.push_back(EntityId::make(i, rec.generation));
        }
    }
    return out;
}

std::vector<EntityId> EntityWorld::within_radius(double cx, double cy, double cz,
                                                 double radius) const {
    // Linear scan; the SpatialIndex acceleration structure is available for
    // callers that need it, but the world's own query is the simple correct
    // reference. (EntityWorld owns no SpatialIndex by default — systems that
    // need one create and resync it.)
    std::vector<EntityId> out;
    const auto tid = std::type_index(typeid(TransformComponent));
    const double r2 = radius * radius;
    for (uint32_t i = 0; i < entities_.size(); ++i) {
        const auto& rec = entities_[i];
        if (!rec.alive) continue;
        auto it = rec.components.find(tid);
        if (it == rec.components.end()) continue;
        const auto* tf = static_cast<const TransformComponent*>(it->second.get());
        const double dx = tf->position.x - cx;
        const double dy = tf->position.y - cy;
        const double dz = tf->position.z - cz;
        if (dx*dx + dy*dy + dz*dz <= r2) {
            out.push_back(EntityId::make(i, rec.generation));
        }
    }
    return out;
}

// ============================================================================
// EntityWorld::update_all — the sim tick primitive.
//
// Two-pass implementation over the BEHAVIORAL-COMPONENT CACHE (see the
// cache notes in entity.hpp):
//   Pass 1: every behavioral component with priority >= BRAIN_THRESHOLD
//           (brains — produces control inputs and publishes ATC requests).
//   Pass 2: every behavioral component with 0 < priority < BRAIN_THRESHOLD
//           (physics — consumes the brain's outputs, integrates state).
//
// The cache is a flat vector of BehavioralComponentBase* in the same visit
// order the pre-cache implementation walked (entity index order, then
// component-map order), rebuilt lazily when the behavioral set changes
// (add/remove of a behavioral component, create/destroy, move). This is the
// campaign-scale fix: a populated save (4,374 entities, ~5 components each)
// cost ~52,000 dynamic_casts + hash-map walks per tick (~36 ms) in the old
// loop; the cache collapses the steady-state tick to the behavioral
// components themselves (~12 for a 4-ship).
//
// Semantics preserved exactly:
//   * priority() is re-read every tick, in both passes — a component whose
//     priority changes at runtime still lands in the right pass.
//   * Pass ordering across entities/components is identical to the old
//     loop's iteration order for any given world state.
//
// Note: bus.flush_pending() is NOT called here. The caller owns the bus
// lifecycle and is responsible for draining deferred messages after the
// tick completes. This keeps update_all focused on "iterate components"
// and avoids surprising side effects on the bus.
// ============================================================================
void EntityWorld::update_all(double dt, messaging::MessageBus& bus) {
    if (behavioral_cache_dirty_) rebuild_behavioral_cache();

    // Pass 1: brains (priority >= BRAIN_THRESHOLD).
    for (auto* bc : behavioral_cache_) {
        if (bc->priority() >= update_phase::BRAIN_THRESHOLD) {
            bc->update(dt, bus);
        }
    }

    // Pass 2: physics (0 < priority < BRAIN_THRESHOLD).
    for (auto* bc : behavioral_cache_) {
        const int prio = bc->priority();
        if (prio > 0 && prio < update_phase::BRAIN_THRESHOLD) {
            bc->update(dt, bus);
        }
    }
}

// Rebuild the behavioral cache: walk entities in index order and components
// in map order (the uncached loop's exact visit order), dynamic_cast once,
// and keep every behavioral hit. One walk per mutation batch instead of two
// per tick. Component object addresses are stable across entities_ vector
// growth (the map nodes transfer, not the elements), and destruction only
// happens through paths that invalidate the cache first — so the stored raw
// pointers stay valid for as long as the cache is clean.
void EntityWorld::rebuild_behavioral_cache() {
    behavioral_cache_.clear();
    for (const auto& rec : entities_) {
        if (!rec.alive) continue;
        for (const auto& [tid, comp] : rec.components) {
            if (auto* bc = dynamic_cast<BehavioralComponentBase*>(comp.get())) {
                behavioral_cache_.push_back(bc);
            }
        }
    }
    behavioral_cache_dirty_ = false;
}

// ============================================================================
// EntityHandle
// ============================================================================
bool EntityHandle::valid() const noexcept {
    if (!world_ || cookie_ != world_->cookie_) return false;
    return world_->alive(id_);
}

void EntityHandle::set_tag(const TagKey& key, TagValue value) {
    if (!world_) throw std::runtime_error("EntityHandle::set_tag: no world");
    auto* rec = world_->find(id_);
    if (!rec) throw std::runtime_error("EntityHandle::set_tag: invalid entity");

    // Phase D: maintain the tag index. If this entity already has a value
    // for this key, we're overwriting it — remove the entity from the old
    // bucket first, then add to the new bucket. The TagValue is moved into
    // the per-entity tag map; the index gets its own copy (it needs its own
    // key for lookup).
    auto existing = rec->tags.find(key);
    if (existing != rec->tags.end() && existing->second == value) {
        // Same value — no index change needed, just update the tag map
        // (which is a no-op assignment, but keeps the code path uniform).
        existing->second = std::move(value);
        return;
    }
    if (existing != rec->tags.end()) {
        // Overwrite: remove from old bucket, fall through to add to new.
        world_->index_tag_remove(key, existing->second, id_);
        existing->second = std::move(value);
    } else {
        rec->tags[key] = std::move(value);
    }
    // Re-read the value we just stored (it was moved from) and add to index.
    // We use the value now stored in rec->tags as the source of truth.
    world_->index_tag_add(key, rec->tags.at(key), id_);
}

std::optional<TagValue> EntityHandle::get_tag(const TagKey& key) const {
    if (!world_) return std::nullopt;
    const auto* rec = world_->find(id_);
    if (!rec) return std::nullopt;
    auto it = rec->tags.find(key);
    if (it == rec->tags.end()) return std::nullopt;
    return it->second;
}

bool EntityHandle::has_tag(const TagKey& key) const {
    return get_tag(key).has_value();
}

// ============================================================================
// Phase D: tag index maintenance helpers.
//
// These are called by set_tag() (add + remove-on-overwrite) and destroy()
// (remove for every tag on the entity). They maintain the invariant that
// tag_index_[K][V] contains exactly the live entities with tag K=V.
//
// Both are O(1) amortized for the hash lookups; the vector append/erase is
// O(1) amortized (append) or O(n) (erase by value, where n is the bucket
// size). Tag mutations are rare (set at load, destroyed basically never in
// the viewer), so the erase cost is acceptable.
// ============================================================================

void EntityWorld::index_tag_add(const TagKey& key, const TagValue& value, EntityId id) {
    // operator[] default-constructs the inner map and/or vector if absent.
    tag_index_[key][value].push_back(id);
}

void EntityWorld::index_tag_remove(const TagKey& key, const TagValue& value, EntityId id) {
    auto kit = tag_index_.find(key);
    if (kit == tag_index_.end()) return;
    auto vit = kit->second.find(value);
    if (vit == kit->second.end()) return;
    auto& bucket = vit->second;
    // Linear scan for the EntityId. EntityId is a 64-bit value (index |
    // generation), so == is a single comparison. The bucket is typically
    // small (e.g. 2659 objectives, ~5 unit classes, ~8 teams).
    for (auto it = bucket.begin(); it != bucket.end(); ++it) {
        if (*it == id) {
            bucket.erase(it);
            break;
        }
    }
    // Optional cleanup: if the bucket is now empty, erase it from the inner
    // map to keep the index compact. This also lets with_tag_ref() return
    // the shared empty_buckets_ vector for this (key, value) pair.
    if (bucket.empty()) {
        kit->second.erase(vit);
        // And if the inner map is now empty, erase it from the outer map.
        if (kit->second.empty()) {
            tag_index_.erase(kit);
        }
    }
}

// ============================================================================
// Component-type index maintenance (see the index notes in entity.hpp).
// Both are no-ops for types never queried (lazy build covers them).
// ============================================================================
void EntityWorld::component_index_on_add(std::type_index tid, EntityId id,
                                         bool replacing) {
    auto it = component_index_.find(tid);
    if (it == component_index_.end()) return;  // type never queried yet
    if (replacing) return;  // component overwritten in place — the entity's
                            // id is already in the bucket
    auto& bucket = it->second;
    if (bucket.empty() || id.index() > bucket.back().index()) {
        // Tail append preserves entity-index order (the scan's order).
        // Weapons spawn at the world's tail during a run, so this is the
        // common case; O(1) amortized.
        bucket.push_back(id);
    } else {
        // Out-of-order insert (component added to a reused slot below the
        // tail): drop the bucket, rebuild lazily on the next query. Rare.
        component_index_.erase(it);
    }
}

void EntityWorld::component_index_on_remove(std::type_index tid, EntityId id) {
    auto it = component_index_.find(tid);
    if (it == component_index_.end()) return;
    auto& bucket = it->second;
    for (auto b = bucket.begin(); b != bucket.end(); ++b) {
        if (*b == id) {
            bucket.erase(b);
            break;
        }
    }
    // NOTE: empty buckets are KEPT, not erased. A bucket that is queried
    // every tick but currently empty (RadarSimComponent/MissileComponent/
    // BombComponent in a world with no combat traffic) would otherwise be
    // erased here and REBUILT by the next query — a full O(world) walk per
    // tick per empty type, the exact cost the index exists to kill. An
    // empty bucket is correct (invariant 1: exactly the live entities
    // carrying T — none) and on_add appends into it fine. The map holds at
    // most one bucket per distinct queried type, so keeping empties is
    // bounded by the number of types the host actually queries (~15).
}

// ============================================================================
// SpatialIndex
// ============================================================================
int64_t SpatialIndex::to_key(double x, double y, double z) const noexcept {
    const int cx = static_cast<int>(std::floor(x * inv_cell_size_));
    const int cy = static_cast<int>(std::floor(y * inv_cell_size_));
    const int cz = static_cast<int>(std::floor(z * inv_cell_size_));
    return to_key(cx, cy, cz);
}

int64_t SpatialIndex::to_key(int cx, int cy, int cz) const noexcept {
    // Interleave into a single 64-bit key. Cell coordinates are signed ints;
    // we offset by 2^20 (a ~±30M cell range at 30kft cells = ±900M ft) to
    // keep the packing non-negative and collision-free.
    constexpr int64_t OFFSET = 1ll << 20;
    const int64_t kx = static_cast<int64_t>(cx) + OFFSET;
    const int64_t ky = static_cast<int64_t>(cy) + OFFSET;
    const int64_t kz = static_cast<int64_t>(cz) + OFFSET;
    return (kx << 42) | (ky << 21) | kz;
}

void SpatialIndex::insert(EntityId id, double x, double y, double z) noexcept {
    const int64_t key = to_key(x, y, z);
    grid_[key].push_back(CellEntry{id, x, y, z});
    id_to_key_[id] = key;  // reverse index for O(1) removal
}

void SpatialIndex::remove(EntityId id) noexcept {
    // O(1) removal using the reverse index. Previously this scanned ALL
    // cells, which was O(cells * entries_per_cell) — very expensive when
    // many entities are moving each frame (every update = remove + insert).
    auto rit = id_to_key_.find(id);
    if (rit == id_to_key_.end()) return;
    const int64_t key = rit->second;
    id_to_key_.erase(rit);

    auto git = grid_.find(key);
    if (git == grid_.end()) return;
    auto& entries = git->second;
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        if (it->id == id) { entries.erase(it); break; }
    }
    if (entries.empty()) grid_.erase(git);
}

std::vector<EntityId> SpatialIndex::query_radius(double cx, double cy, double cz,
                                                 double radius) const noexcept {
    std::vector<EntityId> out;
    if (radius < 0.0) return out;

    const double r2 = radius * radius;
    // Determine the cell range to scan.
    const int min_cx = static_cast<int>(std::floor((cx - radius) * inv_cell_size_));
    const int max_cx = static_cast<int>(std::floor((cx + radius) * inv_cell_size_));
    const int min_cy = static_cast<int>(std::floor((cy - radius) * inv_cell_size_));
    const int max_cy = static_cast<int>(std::floor((cy + radius) * inv_cell_size_));
    const int min_cz = static_cast<int>(std::floor((cz - radius) * inv_cell_size_));
    const int max_cz = static_cast<int>(std::floor((cz + radius) * inv_cell_size_));

    for (int cx_i = min_cx; cx_i <= max_cx; ++cx_i)
    for (int cy_i = min_cy; cy_i <= max_cy; ++cy_i)
    for (int cz_i = min_cz; cz_i <= max_cz; ++cz_i) {
        auto it = grid_.find(to_key(cx_i, cy_i, cz_i));
        if (it == grid_.end()) continue;
        for (const auto& e : it->second) {
            const double dx = e.x - cx;
            const double dy = e.y - cy;
            const double dz = e.z - cz;
            if (dx*dx + dy*dy + dz*dz <= r2) {
                out.push_back(e.id);
            }
        }
    }
    return out;
}

} // namespace f4::entities
