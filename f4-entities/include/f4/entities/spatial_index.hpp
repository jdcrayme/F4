// f4-entities/spatial_index.hpp
//
// 3D spatial hash grid for fast radius queries over entity positions.
//
// Cell size should be on the order of the typical query radius for best
// performance (a query then touches only the 3x3x3 neighborhood of cells
// around the query point). For theater-scale AI queries (radar/SAM rings,
// formation spacing), a cell size of a few NM (~30k ft) is typical.
//
// The index is populated from entities that have a TransformComponent; the
// EntityWorld is the source of truth, the index is a derived acceleration
// structure and must be resynced when entities move (call update()).

#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "entity.hpp"

namespace std {
    template<>
    struct hash<f4::entities::EntityId> {
        std::size_t operator()(const f4::entities::EntityId& id) const noexcept {
            return std::hash<uint64_t>{}(id.value);
        }
    };
} // namespace std

namespace f4::entities {

class SpatialIndex {
public:
    explicit SpatialIndex(double cell_size = 30000.0) noexcept
        : cell_size_(cell_size), inv_cell_size_(1.0 / cell_size) {}

    void clear() noexcept { grid_.clear(); id_to_key_.clear(); }

    /// Insert or update an entity's position in the grid.
    void insert(EntityId id, double x, double y, double z) noexcept;
    void remove(EntityId id) noexcept;
    void update(EntityId id, double x, double y, double z) noexcept {
        remove(id);
        insert(id, x, y, z);
    }

    /// All entity ids within `radius` (slant) of (cx,cy,cz).
    /// Positions are returned via a caller-supplied callback so the grid
    /// never owns a copy of world state — it only maps id -> cell.
    /// Use EntityWorld::within_radius for the convenience wrapper that reads
    /// positions back from TransformComponents.
    [[nodiscard]] std::vector<EntityId> query_radius(double cx, double cy, double cz,
                                                      double radius) const noexcept;

    [[nodiscard]] std::size_t cell_count() const noexcept { return grid_.size(); }
    [[nodiscard]] double cell_size() const noexcept { return cell_size_; }

private:
    struct CellEntry { EntityId id; double x, y, z; };

    [[nodiscard]] int64_t to_key(double x, double y, double z) const noexcept;
    [[nodiscard]] int64_t to_key(int cx, int cy, int cz) const noexcept;

    double cell_size_;
    double inv_cell_size_;
    // cell key -> entries in that cell. Linear scan within a cell is fine
    // because cells are sized to the query radius (few entries per cell).
    std::unordered_map<int64_t, std::vector<CellEntry>> grid_;
    // Reverse index: entity id -> cell key, for O(1) removal instead of
    // the previous O(cells * entries) brute-force scan. The update() method
    // is the hot path (called every frame for every moving entity), and the
    // old remove() implementation scanned ALL cells to find the entity.
    std::unordered_map<EntityId, int64_t> id_to_key_;
};

} // namespace f4::entities
