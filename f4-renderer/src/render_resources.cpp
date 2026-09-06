// f4-renderer/src/render_resources.cpp
//
// RenderResources implementation. The model-build path delegates to
// RuntimeModelCache (glTF + PNG — Tranche 0d); this file owns the
// default material, lighting state, and the airfield geometry cache.

#include <f4/renderer/render_resources.hpp>

#include <raylib.h>
#include <rlgl.h>   // GetShaderDefault

namespace f4::renderer {

// ── Destructor ───────────────────────────────────────────────────────────

RenderResources::~RenderResources() {
    unload_all();
}

// ── set_model_data_dir ───────────────────────────────────────────────────

void RenderResources::set_model_data_dir(const std::filesystem::path& data_dir) {
    model_cache.set_data_dir(data_dir);
}

// ── ensure_default_material ──────────────────────────────────────────────

bool RenderResources::ensure_default_material() {
    if (default_mat_valid_) return true;

    // Compile the lit shader (idempotent; may fail headless, in which
    // case we still build the material with Raylib's unlit default).
    lit_shader.ensure();

    // 1) 1×1 opaque-white fallback texture.
    if (!fallback_white_tex_valid_) {
        Image img = {};
        img.data = RL_MALLOC(4);  // one RGBA8 pixel
        if (!img.data) return false;
        auto* px = static_cast<unsigned char*>(img.data);
        px[0] = 255; px[1] = 255; px[2] = 255; px[3] = 255;
        img.width = 1;
        img.height = 1;
        img.mipmaps = 1;
        img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
        fallback_white_tex_ = LoadTextureFromImage(img);
        UnloadImage(img);
        fallback_white_tex_valid_ = (fallback_white_tex_.id != 0);
        if (!fallback_white_tex_valid_) return false;
    }

    // 2) Default material with the white texture bound (so untextured
    // meshes sample (1,1,1,1) — the lit shader's chroma-key discard
    // would hide them otherwise) and the lit shader assigned if it
    // compiled.
    default_mat_ = LoadMaterialDefault();
    default_mat_.maps[MATERIAL_MAP_DIFFUSE].texture = fallback_white_tex_;
    default_mat_.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
    if (lit_shader.is_loaded()) {
        default_mat_.shader = lit_shader.shader();
    }
    default_mat_valid_ = true;
    return true;
}

// ── build_mesh_for_model ─────────────────────────────────────────────────

void RenderResources::build_mesh_for_model(int vis_type) {
    // RuntimeModelCache handles the lazy build + texture upload (glTF
    // load → mesh extraction → UploadMesh → upload_png). It marks the
    // entry built even on failure, so this stays idempotent.
    model_cache.build_model(vis_type, texture_cache);
}

// ── unload_all ───────────────────────────────────────────────────────────

void RenderResources::unload_all() {
    texture_cache.unload_all();

    // glTF model meshes (GPU handles owned by the cache).
    model_cache.unload_all();

    // Pure data — no GPU resources, but free the memory.
    airfield_cache.clear();

    // Unset the texture reference on the material BEFORE unloading it,
    // otherwise UnloadMaterial may try to free a texture that's about to
    // be freed separately. Same for the shader: raylib's UnloadMaterial
    // frees any non-default shader assigned to the material — the
    // LitShader member owns that shader and unloads it itself, so detach
    // it first to avoid a double free.
    if (default_mat_valid_) {
        default_mat_.maps[MATERIAL_MAP_DIFFUSE].texture = {};
        default_mat_.shader.id = rlGetShaderIdDefault();
        UnloadMaterial(default_mat_);
        default_mat_ = {};
        default_mat_valid_ = false;
    }
    if (fallback_white_tex_valid_) {
        UnloadTexture(fallback_white_tex_);
        fallback_white_tex_ = {};
        fallback_white_tex_valid_ = false;
    }
}

} // namespace f4::renderer
