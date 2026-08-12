// f4-models-viewer/src/scene.hpp
//
// ModelGeometry → Raylib ::Mesh conversion.
// Now delegates to f4::renderer::build_raylib_meshes() and
// f4::renderer::build_mesh_entries() from f4-renderer.
// This header is kept for backward compatibility; the free
// functions are no longer defined locally.

#pragma once

namespace f4::models_viewer {

// The local build_raylib_meshes() and unload_meshes() have been
// removed. Callers should use:
//   f4::renderer::build_raylib_meshes(geom, color_bank)
//   f4::renderer::build_mesh_entries(geom, raylib_meshes)
//   f4::renderer::unload_meshes(raylib_meshes)

} // namespace f4::models_viewer
