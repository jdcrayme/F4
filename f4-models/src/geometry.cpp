// f4-models/src/geometry.cpp
//
// Geometry type method implementations.

#include <f4/models/geometry.hpp>

namespace f4::models {

void Mesh::merge(const Mesh& other) {
    auto offset = static_cast<uint32_t>(vertices.size());
    vertices.insert(vertices.end(), other.vertices.begin(), other.vertices.end());
    for (auto tri : other.triangles) {
        tri.v0 += offset;
        tri.v1 += offset;
        tri.v2 += offset;
        triangles.push_back(tri);
    }
}

std::size_t ModelGeometry::total_triangles() const noexcept {
    std::size_t n = 0;
    for (const auto& m : meshes) n += m.triangles.size();
    return n;
}

std::size_t ModelGeometry::total_vertices() const noexcept {
    std::size_t n = 0;
    for (const auto& m : meshes) n += m.vertices.size();
    return n;
}

Mesh ModelGeometry::merged() const {
    Mesh result;
    for (const auto& m : meshes) {
        result.merge(m);
    }
    return result;
}

} // namespace f4::models
