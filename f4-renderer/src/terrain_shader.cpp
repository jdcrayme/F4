// f4-renderer/src/terrain_shader.cpp
//
// TerrainShader implementation: GLSL 330 sources + lifecycle + uniforms.

#include <f4/renderer/terrain_shader.hpp>

#include <f4/math/vec3.hpp>

#include <raylib.h>
#include <rlgl.h>

#include "external/glad.h"   // glBindTexture / GL_TEXTURE_2D_ARRAY (see terrain_tile_cache.cpp)

#include <utility>

namespace f4::renderer {

// ── GLSL 330 Vertex Shader ────────────────────────────────────────────────────
// Standard raylib attribute set (mvp/matModel uniforms provided by DrawMesh)
// plus vertexTexCoord2 carrying (tile layer, tile size family). View
// distance for fog is taken from gl_Position.w (== eye-space -z under a
// perspective projection).
static const char* kTerrainShaderVS =
    "#version 330\n"
    "in vec3 vertexPosition;\n"
    "in vec2 vertexTexCoord;\n"
    "in vec4 vertexColor;\n"
    "in vec3 vertexNormal;\n"
    "in vec2 vertexTexCoord2;\n"
    "uniform mat4 mvp;\n"
    "uniform mat4 matModel;\n"
    "out vec2 fragTexCoord;\n"
    "out vec4 fragColor;\n"
    "out vec3 fragNormal;\n"
    "out vec2 fragTile;\n"
    "out float fragViewDist;\n"
    "void main() {\n"
    "    fragTexCoord = vertexTexCoord;\n"
    "    fragColor = vertexColor;\n"
    "    fragNormal = normalize(mat3(matModel) * vertexNormal);\n"
    "    fragTile = vertexTexCoord2;\n"
    "    gl_Position = mvp * vec4(vertexPosition, 1.0);\n"
    "    fragViewDist = gl_Position.w;\n"
    "}\n";

// ── GLSL 330 Fragment Shader ──────────────────────────────────────────────────
// Selects one of the four tile arrays by the family flag, then applies
// ambient + N*L lighting and linear distance fog. No chroma discard —
// terrain tiles are opaque.
static const char* kTerrainShaderFS =
    "#version 330\n"
    "in vec2 fragTexCoord;\n"
    "in vec4 fragColor;\n"
    "in vec3 fragNormal;\n"
    "in vec2 fragTile;\n"
    "in float fragViewDist;\n"
    "uniform sampler2DArray texFar;\n"
    "uniform sampler2DArray texNear32;\n"
    "uniform sampler2DArray texNear64;\n"
    "uniform sampler2DArray texNear128;\n"
    "uniform vec4 colDiffuse;\n"
    "uniform vec3 lightDir;\n"
    "uniform vec4 lightColor;\n"
    "uniform vec4 ambient;\n"
    "uniform vec4 fogColor;\n"
    "uniform float fogStart;\n"
    "uniform float fogEnd;\n"
    "out vec4 finalColor;\n"
    "void main() {\n"
    "    vec4 tex;\n"
    "    if (fragTile.y < -0.5)      tex = vec4(1.0);   // untextured quad (vertex color)\n"
    "    else if (fragTile.y < 0.5)  tex = texture(texFar,    vec3(fragTexCoord, fragTile.x));\n"
    "    else if (fragTile.y < 48.0) tex = texture(texNear32, vec3(fragTexCoord, fragTile.x));\n"
    "    else if (fragTile.y < 96.0) tex = texture(texNear64, vec3(fragTexCoord, fragTile.x));\n"
    "    else                        tex = texture(texNear128,vec3(fragTexCoord, fragTile.x));\n"
    "    vec3 N = normalize(fragNormal);\n"
    "    vec3 L = normalize(lightDir);\n"
    "    float NdotL = max(dot(N, L), 0.0);\n"
    "    vec4 light = ambient + lightColor * NdotL;\n"
    "    vec4 c = tex * colDiffuse * fragColor * light;\n"
    "    if (fogEnd > fogStart) {\n"
    "        float f = clamp((fragViewDist - fogStart) / (fogEnd - fogStart), 0.0, 1.0);\n"
    "        c = mix(c, fogColor, f);\n"
    "    }\n"
    "    finalColor = c;\n"
    "}\n";

// ── Lifecycle ────────────────────────────────────────────────────────────────

TerrainShader::~TerrainShader() {
    if (loaded_ && shader_.id != 0) UnloadShader(shader_);
}

TerrainShader::TerrainShader(TerrainShader&& other) noexcept
    : shader_(other.shader_)
    , loaded_(other.loaded_)
    , dir_loc_(other.dir_loc_)
    , color_loc_(other.color_loc_)
    , ambient_loc_(other.ambient_loc_)
    , fog_color_loc_(other.fog_color_loc_)
    , fog_start_loc_(other.fog_start_loc_)
    , fog_end_loc_(other.fog_end_loc_)
{
    for (int i = 0; i < 4; ++i) sampler_loc_[i] = other.sampler_loc_[i];
    other = TerrainShader{};
}

TerrainShader& TerrainShader::operator=(TerrainShader&& other) noexcept {
    if (this != &other) {
        if (loaded_ && shader_.id != 0) UnloadShader(shader_);
        shader_ = other.shader_;
        loaded_ = other.loaded_;
        dir_loc_ = other.dir_loc_;
        color_loc_ = other.color_loc_;
        ambient_loc_ = other.ambient_loc_;
        fog_color_loc_ = other.fog_color_loc_;
        fog_start_loc_ = other.fog_start_loc_;
        fog_end_loc_ = other.fog_end_loc_;
        for (int i = 0; i < 4; ++i) sampler_loc_[i] = other.sampler_loc_[i];
        other = TerrainShader{};
    }
    return *this;
}

bool TerrainShader::ensure(std::string* status_msg) {
    if (loaded_) return shader_.id != 0;
    shader_ = LoadShaderFromMemory(kTerrainShaderVS, kTerrainShaderFS);
    loaded_ = true;
    if (shader_.id == 0) {
        if (status_msg) *status_msg = "warning: terrain shader failed to compile";
        return false;
    }
    dir_loc_       = GetShaderLocation(shader_, "lightDir");
    color_loc_     = GetShaderLocation(shader_, "lightColor");
    ambient_loc_   = GetShaderLocation(shader_, "ambient");
    fog_color_loc_ = GetShaderLocation(shader_, "fogColor");
    fog_start_loc_ = GetShaderLocation(shader_, "fogStart");
    fog_end_loc_   = GetShaderLocation(shader_, "fogEnd");
    sampler_loc_[0] = GetShaderLocation(shader_, "texFar");
    sampler_loc_[1] = GetShaderLocation(shader_, "texNear32");
    sampler_loc_[2] = GetShaderLocation(shader_, "texNear64");
    sampler_loc_[3] = GetShaderLocation(shader_, "texNear128");
    return true;
}

// ── Uniforms ─────────────────────────────────────────────────────────────────

void TerrainShader::set_lighting(Vector3 light_dir, Color light_color,
                                 float intensity, Color ambient_color) const {
    {
        const f4::math::Vec3f dir{light_dir.x, light_dir.y, light_dir.z};
        const f4::math::Vec3f n =
            (dir.length() > 0.0001f) ? dir.normalized()
                                     : f4::math::Vec3f{0.5f, -1.0f, 0.3f};
        light_dir = {n.x, n.y, n.z};
    }
    if (dir_loc_ >= 0) {
        const float v[3] = {light_dir.x, light_dir.y, light_dir.z};
        SetShaderValue(shader_, dir_loc_, v, SHADER_UNIFORM_VEC3);
    }
    if (color_loc_ >= 0) {
        const float v[4] = {
            light_color.r / 255.0f * intensity,
            light_color.g / 255.0f * intensity,
            light_color.b / 255.0f * intensity,
            light_color.a / 255.0f,
        };
        SetShaderValue(shader_, color_loc_, v, SHADER_UNIFORM_VEC4);
    }
    if (ambient_loc_ >= 0) {
        const float v[4] = {
            ambient_color.r / 255.0f,
            ambient_color.g / 255.0f,
            ambient_color.b / 255.0f,
            ambient_color.a / 255.0f,
        };
        SetShaderValue(shader_, ambient_loc_, v, SHADER_UNIFORM_VEC4);
    }
}

void TerrainShader::set_fog(Color fog_color, float start_ft, float end_ft) const {
    if (fog_color_loc_ >= 0) {
        const float v[4] = {
            fog_color.r / 255.0f, fog_color.g / 255.0f,
            fog_color.b / 255.0f, fog_color.a / 255.0f,
        };
        SetShaderValue(shader_, fog_color_loc_, v, SHADER_UNIFORM_VEC4);
    }
    if (fog_start_loc_ >= 0)
        SetShaderValue(shader_, fog_start_loc_, &start_ft, SHADER_UNIFORM_FLOAT);
    if (fog_end_loc_ >= 0)
        SetShaderValue(shader_, fog_end_loc_, &end_ft, SHADER_UNIFORM_FLOAT);
}

void TerrainShader::bind_tile_samplers(TerrainTileCache& cache) const {
    if (!is_loaded()) return;
    cache.ensure_arrays();
    TerrainTileCache::TileArray* arrays[4] = {
        &cache.far(), &cache.near32(), &cache.near64(), &cache.near128(),
    };
    for (int i = 0; i < 4; ++i) {
        const int unit = 1 + i;   // unit 0 belongs to raylib's diffuse map
        if (sampler_loc_[i] >= 0) {
            SetShaderValue(shader_, sampler_loc_[i], &unit, SHADER_UNIFORM_INT);
        }
        rlActiveTextureSlot(unit);
        glBindTexture(GL_TEXTURE_2D_ARRAY, arrays[i]->gl_id);
    }
    rlActiveTextureSlot(0);
}

} // namespace f4::renderer
