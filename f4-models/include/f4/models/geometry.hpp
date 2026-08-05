// f4-models/include/f4/models/geometry.hpp
//
// Renderable geometry types — the output of BSP tree traversal.
// These are engine-agnostic: flat triangle lists with vertex attributes.
// Any renderer (Raylib, OpenGL, Vulkan) or exporter (OBJ, glTF) can
// consume these without knowing about BSP trees.

#pragma once

#include <f4/models/types.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace f4::models {

// ── Vertex ────────────────────────────────────────────────────────────────

struct Vertex {
    Vec3    position  = {};     ///< world-space position
    Vec3    normal    = {};     ///< surface normal (0,0,0 if not present)
    TexCoord uv       = {};     ///< texture coordinates (-1,-1 if not present)
    uint32_t color    = 0;      ///< packed RGBA color (0 if not present)
    int32_t  tex_id   = -1;     ///< texture ID index into HDR texture bank
};

// ── Triangle ──────────────────────────────────────────────────────────────

struct Triangle {
    uint32_t v0 = 0, v1 = 0, v2 = 0;  ///< indices into vertex array
    int32_t  tex_id = -1;               ///< texture ID for this face
};

// ── Mesh ──────────────────────────────────────────────────────────────────
/// One draw-call worth of geometry — all triangles share the same
/// material/texture. A complete model may have multiple meshes.

struct Mesh {
    std::vector<Vertex>   vertices;
    std::vector<Triangle> triangles;
    int32_t               tex_id = -1;     ///< common texture ID (-1 = mixed)
    std::string           name;             ///< optional debug name

    /// Merge another mesh into this one (remaps vertex indices).
    void merge(const Mesh& other);

    [[nodiscard]] bool empty() const noexcept {
        return vertices.empty() && triangles.empty();
    }
    [[nodiscard]] std::size_t triangle_count() const noexcept {
        return triangles.size();
    }
};

// ── Extracted Model Geometry ──────────────────────────────────────────────
/// Complete geometry for one model, potentially multiple meshes
/// (one per texture or material).

struct ModelGeometry {
    std::vector<Mesh> meshes;

    /// Total triangle count across all meshes.
    [[nodiscard]] std::size_t total_triangles() const noexcept;

    /// Total vertex count across all meshes.
    [[nodiscard]] std::size_t total_vertices() const noexcept;

    /// Merge all meshes into a single mesh.
    [[nodiscard]] Mesh merged() const;
};

// ── DOF/Switch State ──────────────────────────────────────────────────────
/// Controls for interactive model viewing. Maps DOF/switch indices
/// to their current values.

struct DofState {
    int     dof_number = -1;    ///< which DOF this controls
    float   value      = 0;     ///< current DOF value (range: [min, max])
    float   min        = 0;     ///< DOF minimum
    float   max        = 0;     ///< DOF maximum
    Mat3x3  rotation   = {};    ///< rotation matrix at DOF=0
    Vec3    translation = {};   ///< translation at DOF=0
};

struct SwitchState {
    int  switch_number = -1;    ///< which switch this controls
    int  active_child  = 0;     ///< currently visible child index (0-based)
    int  n_children    = 0;     ///< total number of switch children
};

struct ModelState {
    std::vector<DofState>    dofs;
    std::vector<SwitchState> switches;
    int                      lod_level = 0;  ///< active LOD level
};

} // namespace f4::models
