// f4-models/include/f4/models/model_lod.hpp
//
// One parsed LOD record — either a classic BSP tree or a DX model.
// This is the output of parsing one LOD entry from KoreaObj.LOD/DXL.
//
// References:
//   FreeFalcon: src/graphics/include/objectlod.h (ObjectLOD)

#pragma once

#include <f4/models/bsp_node.hpp>
#include <f4/models/types.hpp>

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace f4::models {

/// Parsed classic BSP LOD data.
struct BspLodData {
    BspTree tree;
};

/// Parsed DX LOD data — vertex buffer format.
struct DxLodData {
    DxHeader header;
    /// Vertex positions (x,y,z triples).
    std::vector<Vec3> vertices;
    /// Raw DX node stream bytes (for deferred processing).
    std::vector<uint8_t> node_stream;
};

/// One parsed LOD — either BSP or DX format.
struct ModelLod {
    LodFormat format = LodFormat::Bsp;
    int lod_table_idx = -1;       ///< index into HDR LOD table
    std::string name;             ///< LOD name (empty for classic BSP)
    float max_range = 0;          ///< max display distance
    uint32_t offset = 0;          ///< byte offset in LOD file
    uint32_t size = 0;            ///< byte size in LOD file

    /// The parsed geometry data (BSP or DX).
    std::variant<BspLodData, DxLodData> data;

    /// Convenience: get the BSP tree if this is a BSP LOD, else nullptr.
    [[nodiscard]] const BspTree* bsp_tree() const noexcept;
    [[nodiscard]] BspTree* bsp_tree() noexcept;

    /// Convenience: get the DX data if this is a DX LOD, else nullptr.
    [[nodiscard]] const DxLodData* dx_data() const noexcept;
    [[nodiscard]] DxLodData* dx_data() noexcept;
};

} // namespace f4::models
