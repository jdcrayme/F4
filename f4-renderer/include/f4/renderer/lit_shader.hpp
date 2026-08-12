// f4-renderer/include/f4/renderer/lit_shader.hpp
//
// RAII wrapper for the custom Lambertian lit shader used across all
// F4 viewer apps. Single GLSL 330 source, compiled once, uniforms
// set per-frame.
//
// Consolidated from 4 duplicated implementations.

#pragma once

#include <raylib.h>
// Undef raylib macros that pollute the namespace
#undef PI
#undef DEG2RAD
#undef RAD2DEG

#include <string>

namespace f4::renderer {

/// RAII wrapper for the F4 lit shader (Lambertian diffuse + ambient + chroma-key discard).
///
/// Usage:
///   LitShader shader;
///   if (shader.ensure()) {
///       shader.set_lighting(dir, color, intensity, ambient);
///       material.shader = shader.shader();
///   }
///
/// The shader is compiled lazily on first ensure() call. If compilation
/// fails (e.g. headless GL context), ensure() returns false and the
/// caller should fall back to Raylib's default unlit shader.
class LitShader {
public:
    /// Construct an uninitialized lit shader. Call ensure() to compile.
    LitShader() = default;

    /// Destructor: unloads the shader from GPU if it was loaded.
    ~LitShader();

    // Non-copyable, movable
    LitShader(const LitShader&) = delete;
    LitShader& operator=(const LitShader&) = delete;
    LitShader(LitShader&& other) noexcept;
    LitShader& operator=(LitShader&& other) noexcept;

    /// Lazily compile the lit shader if not already loaded.
    /// Returns true if the shader is available (id != 0).
    /// @param status_msg  Optional pointer to a string that will be set
    ///                    on compilation failure (caller can display it).
    bool ensure(std::string* status_msg = nullptr);

    /// Set the lighting uniforms on the shader. Call once per frame
    /// before drawing meshes with this shader.
    /// @param light_dir     World-space direction FROM surface TO light (will be normalized)
    /// @param light_color   Color of the light
    /// @param intensity     Scalar intensity multiplier
    /// @param ambient_color Ambient color
    void set_lighting(Vector3 light_dir, Color light_color, float intensity,
                      Color ambient_color) const;

    /// Get the Raylib Shader object. Only valid after ensure() returns true.
    const Shader& shader() const noexcept { return shader_; }

    /// Returns true if the shader was successfully compiled.
    bool is_loaded() const noexcept { return loaded_ && shader_.id != 0; }

private:
    Shader shader_ = {};
    bool loaded_ = false;       ///< true once we've tried compiling
    int dir_loc_ = -1;          ///< uniform location for "lightDir"
    int color_loc_ = -1;        ///< uniform location for "lightColor"
    int ambient_loc_ = -1;      ///< uniform location for "ambient"
};

} // namespace f4::renderer
