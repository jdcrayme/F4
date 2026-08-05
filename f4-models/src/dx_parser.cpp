// f4-models/src/dx_parser.cpp
//
// DX format parser implementation.
//
// The DX engine format stores models as vertex buffer objects with a
// DxDbHeader prefix. The header contains vertex counts, pool offsets,
// and texture IDs. After the header comes a stream of DX nodes
// (DX_ROOT, DX_SURFACE, DX_MATERIAL, etc.).
//
// References:
//   FreeFalcon: src/graphics/dxengine/dxdefines.h (DxDbHeader, DX node types)
//   FreeFalcon: src/graphics/bsplib/objectlod.cpp (DXL file reading)

#include "dx_parser.hpp"
#include "bin_reader.hpp"

#include <cstring>

namespace f4::models::detail {

bool is_dx_format(uint32_t first4) noexcept {
    return (first4 & 0xFFFF) == ((~first4 >> 16) & 0xFFFF);
}

bool parse_dx_lod(
    const uint8_t* data, std::size_t size,
    DxLodData& result,
    std::string& err)
{
    BinReader r{data, size};

    // Read DxDbHeader
    if (!r.read(result.header.version)) {
        err = "DX data too small for header";
        return false;
    }

    if (!is_dx_format(result.header.version)) {
        err = "DX header checksum mismatch";
        return false;
    }

    if (!r.read(result.header.id) ||
        !r.read(result.header.vb_class) ||
        !r.read(result.header.model_size) ||
        !r.read(result.header.n_vertices) ||
        !r.read(result.header.pool_size) ||
        !r.read(result.header.vertex_pool_offset) ||
        !r.read(result.header.n_nodes))
    {
        err = "DX header truncated";
        return false;
    }

    // Scripts[2] — each is 8 bytes (2 DWORDs)
    if (!r.skip(16)) {
        err = "DX header truncated at scripts";
        return false;
    }

    if (!r.read(result.header.n_lights) ||
        !r.read(result.header.lights_pool_offset) ||
        !r.read(result.header.n_textures))
    {
        err = "DX header truncated at lights/textures";
        return false;
    }

    // Texture ID table immediately follows the header
    if (result.header.n_textures > 0) {
        result.header.texture_ids.resize(result.header.n_textures);
        for (uint32_t i = 0; i < result.header.n_textures; ++i) {
            if (!r.read(result.header.texture_ids[i])) {
                err = "DX texture table truncated";
                return false;
            }
        }
    }

    // Extract vertex positions from the vertex pool
    if (result.header.n_vertices > 0 && result.header.vertex_pool_offset > 0) {
        auto voff = static_cast<std::size_t>(result.header.vertex_pool_offset);
        auto nv = static_cast<std::size_t>(result.header.n_vertices);
        if (voff + nv * sizeof(Vec3) <= size) {
            result.vertices.resize(nv);
            std::memcpy(result.vertices.data(), data + voff,
                        nv * sizeof(Vec3));
        }
    }

    // Store remaining data as the node stream (for deferred processing)
    if (r.pos < size) {
        result.node_stream.assign(data + r.pos, data + size);
    }

    return true;
}

} // namespace f4::models::detail
