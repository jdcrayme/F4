// f4-models/include/f4/models/model_record.hpp
//
// Model directory entry — one record per parent object in KoreaObj.HDR.
// Contains bounding volume, signatures, LOD references, slot positions,
// and DOF/switch counts.
//
// References:
//   FreeFalcon: src/graphics/include/objectparent.h (ParentFileRecord)

#pragma once

#include <f4/models/types.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace f4::models {

/// Axis-aligned bounding box.
struct BoundingBox {
    float min_x = 0, max_x = 0;
    float min_y = 0, max_y = 0;
    float min_z = 0, max_z = 0;

    [[nodiscard]] float center_x() const noexcept { return (min_x + max_x) * 0.5f; }
    [[nodiscard]] float center_y() const noexcept { return (min_y + max_y) * 0.5f; }
    [[nodiscard]] float center_z() const noexcept { return (min_z + max_z) * 0.5f; }
    [[nodiscard]] float extent_x() const noexcept { return (max_x - min_x) * 0.5f; }
    [[nodiscard]] float extent_y() const noexcept { return (max_y - min_y) * 0.5f; }
    [[nodiscard]] float extent_z() const noexcept { return (max_z - min_z) * 0.5f; }
};

/// Reference to one LOD level within a parent model.
struct LodRef {
    std::string name;         ///< LOD name (non-empty only for DX engine)
    int lod_table_idx = -1;   ///< index into the HDR LOD table
    float max_range = 0;      ///< max display distance for this LOD
};

/// Slot attachment point — position + orientation for child models.
struct SlotInfo {
    Vec3 position = {};       ///< slot origin in parent coordinates
    Mat3x3 rotation = {};     ///< slot orientation matrix
};

/// One parent model record — the top-level descriptor from KoreaObj.HDR.
/// This corresponds to one entry in the HDR parent list.
struct ModelRecord {
    int index = -1;              ///< parent index in HDR (0-based)
    float radius = 0;            ///< bounding sphere radius
    BoundingBox bbox;            ///< axis-aligned bounding box
    float radar_signature = 0;   ///< radar cross-section
    float ir_signature = 0;      ///< infrared signature

    int16_t n_texture_sets = 0;    ///< number of texture sets
    int16_t n_dynamic_coords = 0;  ///< number of dynamic vertices
    uint8_t n_lods = 0;            ///< number of LOD levels
    uint8_t n_switch = 0;          ///< (legacy) number of switches
    uint8_t n_dof = 0;             ///< (legacy) number of DOFs
    uint8_t n_slots = 0;           ///< number of attachment slots
    int16_t n_switches = 0;        ///< (extended) number of switches
    int16_t n_dofs = 0;            ///< (extended) number of DOFs

    std::vector<LodRef> lods;              ///< LOD references (low to high detail)
    std::vector<SlotInfo> slots;           ///< parsed slot positions + rotations
    std::vector<Vec3> dynamic_coord_defaults; ///< default positions for dynamic coords

    /// Visual class heuristic based on slot/switch/DOF counts.
    /// Air: many slots + DOFs. Ground: some slots. Feature: few or none.
    [[nodiscard]] std::string_view visual_class() const noexcept;

    /// Effective number of switches (max of legacy and extended counts).
    [[nodiscard]] int effective_switches() const noexcept {
        return std::max(static_cast<int>(n_switch), static_cast<int>(n_switches));
    }

    /// Effective number of DOFs (max of legacy and extended counts).
    [[nodiscard]] int effective_dofs() const noexcept {
        return std::max(static_cast<int>(n_dof), static_cast<int>(n_dofs));
    }
};

} // namespace f4::models
