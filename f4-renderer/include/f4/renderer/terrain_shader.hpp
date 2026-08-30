// f4-renderer/include/f4/renderer/terrain_shader.hpp
//
// TerrainShader — the textured-terrain shader (RAII wrapper, same shape
// as LitShader). GLSL 330 with four sampler2DArray bindings (far 32px,
// near 32/64/128px) + Lambertian lighting + distance fog.
//
// Per-vertex inputs beyond the standard raylib attributes:
//   vertexTexCoord2 = (tile layer index, tile size family flag)
//     flag 0   → far array (32px)
//     flag 32  → near 32px array
//     flag 64  → near 64px array
//     flag 128 → near 128px array
// All four vertices of a terrain quad carry identical values, so plain
// interpolation is exact — no flat qualifiers needed.
//
// Raylib binds "vertexTexCoord2" to attribute location 5 automatically
// (rlgl rlLoadShaderProgram) and DrawMesh uploads mesh->texcoords2 —
// verified against the vendored raylib 5.0 sources.
//
// C++20.

#pragma once

#include <f4/renderer/terrain_tile_cache.hpp>

#include <raylib.h>
// Undef raylib macros that pollute the namespace
#undef PI
#undef DEG2RAD
#undef RAD2DEG

#include <string>

namespace f4::renderer {

class TerrainShader {
public:
    TerrainShader() = default;
    ~TerrainShader();

    TerrainShader(const TerrainShader&) = delete;
    TerrainShader& operator=(const TerrainShader&) = delete;
    TerrainShader(TerrainShader&& other) noexcept;
    TerrainShader& operator=(TerrainShader&& other) noexcept;

    /// Lazily compile. Returns true when usable; on failure sets
    /// `status_msg` (callers fall back to the vertex-color path).
    bool ensure(std::string* status_msg = nullptr);

    /// Lighting uniforms — same convention as LitShader::set_lighting().
    void set_lighting(Vector3 light_dir, Color light_color, float intensity,
                      Color ambient_color) const;

    /// Fog uniforms. Fog ramps linearly on view distance from
    /// start_ft to end_ft toward fog_color. end_ft <= start_ft disables.
    void set_fog(Color fog_color, float start_ft, float end_ft) const;

    /// Point the four samplers at the cache's arrays and assign texture
    /// units (1..4 — unit 0 stays raylib's diffuse). Call once per frame
    /// before drawing textured terrain. Requires ensure() + the cache's
    /// ensure_arrays().
    void bind_tile_samplers(TerrainTileCache& cache) const;

    [[nodiscard]] const Shader& shader() const noexcept { return shader_; }
    [[nodiscard]] bool is_loaded() const noexcept { return loaded_ && shader_.id != 0; }

private:
    Shader shader_ = {};
    bool loaded_ = false;
    int dir_loc_ = -1;
    int color_loc_ = -1;
    int ambient_loc_ = -1;
    int fog_color_loc_ = -1;
    int fog_start_loc_ = -1;
    int fog_end_loc_ = -1;
    int sampler_loc_[4] = {-1, -1, -1, -1};  // far, near32, near64, near128
};

} // namespace f4::renderer
