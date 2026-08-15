// f4-renderer/src/lit_shader.cpp
//
// Lit shader implementation: GLSL 330 source + RAII lifecycle + uniform setters.

#include <f4/renderer/lit_shader.hpp>

#include <f4/math/vec3.hpp>

#include <raylib.h>

#include <cmath>
#include <utility>

namespace f4::renderer {

// ── GLSL 330 Vertex Shader ────────────────────────────────────────────────────
// Standard Raylib vertex pipeline + pass fragNormal in world space.
//
// Uses `mat3(matModel) * vertexNormal` instead of bare `vertexNormal`
// because some callers (scenario-player) draw meshes with a non-identity
// model matrix (entity transform). For identity-model-matrix callers
// (models-viewer), matModel is identity so this is equivalent.
static const char* kLitShaderVS =
    "#version 330\n"
    "in vec3 vertexPosition;\n"
    "in vec2 vertexTexCoord;\n"
    "in vec4 vertexColor;\n"
    "in vec3 vertexNormal;\n"
    "uniform mat4 mvp;\n"
    "uniform mat4 matModel;\n"
    "out vec2 fragTexCoord;\n"
    "out vec4 fragColor;\n"
    "out vec3 fragNormal;\n"
    "void main() {\n"
    "    fragTexCoord = vertexTexCoord;\n"
    "    fragColor = vertexColor;\n"
    "    fragNormal = normalize(mat3(matModel) * vertexNormal);\n"
    "    gl_Position = mvp * vec4(vertexPosition, 1.0);\n"
    "}\n";

// ── GLSL 330 Fragment Shader ──────────────────────────────────────────────────
// tex * colDiffuse * fragColor * (ambient + lightColor * NdotL)
// Chroma-key discard: FreeFalcon's .TEX textures mark transparent pixels
// with alpha=0 (set in tex_reader.cpp when the palette-resolved color
// matches the TexBankEntry chroma key). discard prevents both color AND
// depth writes, so transparent pixels don't occlude geometry behind them.
static const char* kLitShaderFS =
    "#version 330\n"
    "in vec2 fragTexCoord;\n"
    "in vec4 fragColor;\n"
    "in vec3 fragNormal;\n"
    "uniform sampler2D texture0;\n"
    "uniform vec4 colDiffuse;\n"
    "uniform vec3 lightDir;\n"
    "uniform vec4 lightColor;\n"
    "uniform vec4 ambient;\n"
    "out vec4 finalColor;\n"
    "void main() {\n"
    "    vec4 tex = texture(texture0, fragTexCoord);\n"
    "    if (tex.a < 0.5) discard;\n"
    "    vec3 N = normalize(fragNormal);\n"
    "    vec3 L = normalize(lightDir);\n"
    "    float NdotL = max(dot(N, L), 0.0);\n"
    "    vec4 light = ambient + lightColor * NdotL;\n"
    "    finalColor = tex * colDiffuse * fragColor * light;\n"
    "}\n";

// ── Destructor ────────────────────────────────────────────────────────────────

LitShader::~LitShader() {
    if (loaded_ && shader_.id != 0) {
        UnloadShader(shader_);
    }
}

// ── Move operations ───────────────────────────────────────────────────────────

LitShader::LitShader(LitShader&& other) noexcept
    : shader_(other.shader_)
    , loaded_(other.loaded_)
    , dir_loc_(other.dir_loc_)
    , color_loc_(other.color_loc_)
    , ambient_loc_(other.ambient_loc_)
{
    other.shader_ = {};
    other.loaded_ = false;
    other.dir_loc_ = -1;
    other.color_loc_ = -1;
    other.ambient_loc_ = -1;
}

LitShader& LitShader::operator=(LitShader&& other) noexcept {
    if (this != &other) {
        if (loaded_ && shader_.id != 0) {
            UnloadShader(shader_);
        }
        shader_ = other.shader_;
        loaded_ = other.loaded_;
        dir_loc_ = other.dir_loc_;
        color_loc_ = other.color_loc_;
        ambient_loc_ = other.ambient_loc_;
        other.shader_ = {};
        other.loaded_ = false;
        other.dir_loc_ = -1;
        other.color_loc_ = -1;
        other.ambient_loc_ = -1;
    }
    return *this;
}

// ── ensure ────────────────────────────────────────────────────────────────────

bool LitShader::ensure(std::string* status_msg) {
    if (loaded_) {
        return shader_.id != 0;
    }
    shader_ = LoadShaderFromMemory(kLitShaderVS, kLitShaderFS);
    loaded_ = true;
    if (shader_.id == 0) {
        if (status_msg) {
            *status_msg = "warning: lit shader failed to compile (falling back to unlit)";
        }
        return false;
    }
    dir_loc_     = GetShaderLocation(shader_, "lightDir");
    color_loc_   = GetShaderLocation(shader_, "lightColor");
    ambient_loc_ = GetShaderLocation(shader_, "ambient");
    return true;
}

// ── set_lighting ──────────────────────────────────────────────────────────────

void LitShader::set_lighting(Vector3 light_dir, Color light_color, float intensity,
                             Color ambient_color) const {
    // Normalize light direction
    {
        const f4::math::Vec3f dir{light_dir.x, light_dir.y, light_dir.z};
        const f4::math::Vec3f n = (dir.length() > 0.0001f) ? dir.normalized() : f4::math::Vec3f{0.5f, -1.0f, 0.3f};
        light_dir = {n.x, n.y, n.z};
    }

    if (dir_loc_ >= 0) {
        const float dir[3] = { light_dir.x, light_dir.y, light_dir.z };
        SetShaderValue(shader_, dir_loc_, dir, SHADER_UNIFORM_VEC3);
    }
    if (color_loc_ >= 0) {
        const float col[4] = {
            light_color.r / 255.0f * intensity,
            light_color.g / 255.0f * intensity,
            light_color.b / 255.0f * intensity,
            light_color.a / 255.0f
        };
        SetShaderValue(shader_, color_loc_, col, SHADER_UNIFORM_VEC4);
    }
    if (ambient_loc_ >= 0) {
        const float amb[4] = {
            ambient_color.r / 255.0f,
            ambient_color.g / 255.0f,
            ambient_color.b / 255.0f,
            ambient_color.a / 255.0f
        };
        SetShaderValue(shader_, ambient_loc_, amb, SHADER_UNIFORM_VEC4);
    }
}

} // namespace f4::renderer
