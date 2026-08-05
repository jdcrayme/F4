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
        rec.tags.clear();
        rec.components.clear();
    } else {
        index = static_cast<uint32_t>(entities_.size());
        EntityRecord rec;
        rec.generation = 1;         // generation 0 is reserved (null id)
        rec.alive = true;
        entities_.push_back(std::move(rec));
    }
    return EntityHandle(EntityId::make(index, entities_[index].generation), this);
}

void EntityWorld::destroy(EntityId id) {
    auto* rec = find(id);
    if (!rec || !rec->alive) return;
    rec->alive = false;
    rec->tags.clear();
    rec->components.clear();
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
    std::vector<EntityId> out;
    for (uint32_t i = 0; i < entities_.size(); ++i) {
        const auto& rec = entities_[i];
        if (!rec.alive) continue;
        auto it = rec.tags.find(key);
        if (it != rec.tags.end() && it->second == value) {
            out.push_back(EntityId::make(i, rec.generation));
        }
    }
    return out;
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
    rec->tags[key] = std::move(value);
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
