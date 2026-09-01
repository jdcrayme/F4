// f4-gltf/include/f4/gltf/gltf_loader.hpp
//
// Minimal glTF 2.0 loader — the runtime-side model loader for the F4
// asset pipeline (Stage 3, ASSET_PIPELINE_SPEC.md §11).
//
// This is NOT a general-purpose glTF implementation. It reads exactly
// what f4import models emits: nodes, meshes (POSITION/NORMAL/indices),
// buffers, buffer views, accessors, and the f4 extras schema for
// DOF/switch/slot/anchor/lod/col tags. External textures are
// referenced by URI (not loaded here — the renderer handles image
// loading).
//
// Why not vendor tinygltf or fastgltf? The project's stance (per
// f4-json's design rationale) is: we control both ends of the wire
// format. A 300-line reader that uses f4-json for the JSON parsing is
// sufficient, simpler to audit, and faster to compile than a 15k-line
// header. When a richer API is needed (skinning, morph targets,
// animation), this library can be replaced or extended — the contract
// is the glTF 2.0 spec, not this header.
//
// Dependencies: f4-json (for JSON parsing), standard library only.
// C++20. Header-only types + a static .cpp for the loader.

#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace f4::gltf {

// ── glTF 2.0 core types (subset) ──────────────────────────────────────────

/// A buffer — raw binary data. May be embedded (base64) or external (.bin).
/// For f4import models output, it's always external (.bin next to .gltf).
struct Buffer {
    std::size_t byte_length = 0;
    std::string uri;             // empty = .bin next to .gltf (resolved by loader)
    std::vector<uint8_t> data;    // loaded on demand
};

/// A buffer view — a contiguous slice of a buffer.
struct BufferView {
    std::size_t buffer = 0;       // index into GltfDocument::buffers
    std::size_t byte_offset = 0;
    std::size_t byte_length = 0;
    std::size_t byte_stride = 0;  // 0 = tightly packed
    int target = 0;               // 34962 = ARRAY_BUFFER, 34963 = ELEMENT_ARRAY_BUFFER
};

/// An accessor — typed view into a buffer view.
struct Accessor {
    std::size_t buffer_view = 0;
    std::size_t byte_offset = 0;  // within the buffer view
    int component_type = 0;       // 5120=BYTE, 5121=UBYTE, 5122=SHORT, 5123=USHORT,
                                  // 5125=UINT, 5126=FLOAT
    bool normalized = false;
    std::size_t count = 0;
    std::string type;              // "SCALAR", "VEC2", "VEC3", "VEC4", "MAT4", ...
    std::optional<std::vector<double>> min;
    std::optional<std::vector<double>> max;
};

/// A mesh primitive — one draw call.
struct Primitive {
    std::optional<std::size_t> positions;   // accessor index for POSITION
    std::optional<std::size_t> normals;     // accessor index for NORMAL
    std::optional<std::size_t> texcoords0;  // accessor index for TEXCOORD_0
    std::optional<std::size_t> colors0;     // accessor index for COLOR_0
    std::optional<std::size_t> indices;    // accessor index for indices
    int mode = 4;                           // 4 = TRIANGLES, see glTF spec
};

/// A mesh — one or more primitives.
struct Mesh {
    std::string name;
    std::vector<Primitive> primitives;
};

/// The f4 extras for a tagged node (ASSET_PIPELINE_SPEC §6.2).
/// All fields are optional — a node may have zero or more of these.
struct F4Extras {
    int version = 0;             // schema version (f4.v)
    std::string kind;            // "dof", "sw", "slot", "anchor", "lod", "col"
    std::string id;              // the tag id (e.g. "gear", "hook", "parking.3")
    // DOF-specific:
    std::optional<int> dof_index;
    std::optional<float> dof_min;
    std::optional<float> dof_max;
    std::optional<float> dof_mult;
    std::optional<int> dof_flags;
    // Switch-specific:
    std::optional<int> sw_index;
    std::optional<int> sw_child;     // child number within the switch
    std::optional<std::string> sw_state; // "up", "down", etc.
    // Slot-specific:
    std::optional<int> slot_index;
    std::optional<int> slot_station;
    // Anchor-specific:
    // (anchor-specific fields live in the `detail` string — free-form)
    // LOD-specific:
    std::optional<int> lod_level;
    std::optional<bool> lod_billboard;
    // Col-specific:
    std::optional<std::string> col_shape;
    std::optional<std::string> col_material;
};

/// A node in the scene graph.
struct Node {
    std::string name;
    std::optional<std::size_t> mesh;        // index into GltfDocument::meshes
    std::vector<std::size_t> children;      // child node indices
    std::optional<std::array<double, 16>> matrix; // 4x4 column-major
    std::optional<std::array<double, 3>> translation;
    std::optional<std::array<double, 4>> rotation; // quaternion [x,y,z,w]
    std::optional<std::array<double, 3>> scale;
    F4Extras f4;                             // parsed from node extras (if present)
    bool has_f4 = false;                    // true when the node had an f4 extras block
};

/// A scene — the root nodes of the scene graph.
struct Scene {
    std::vector<std::size_t> nodes;  // root node indices
};

/// A complete glTF document — the parsed .gltf file.
struct GltfDocument {
    std::vector<Buffer> buffers;
    std::vector<BufferView> buffer_views;
    std::vector<Accessor> accessors;
    std::vector<Mesh> meshes;
    std::vector<Node> nodes;
    std::vector<Scene> scenes;
    int scene = -1;               // default scene index

    /// Load a .gltf file from disk. Resolves external .bin buffers relative
    /// to the .gltf file's directory. Throws std::runtime_error on parse
    /// failure or I/O error.
    void load(const std::filesystem::path& gltf_path);

    /// Load from an in-memory JSON string. Buffer URIs are NOT resolved
    /// (callers must call load_buffer_data() manually). Used for testing.
    void load_from_string(const std::string& json);

    /// Load the binary data for a buffer from its URI (resolved relative
    /// to the .gltf file's directory). Throws on I/O error. No-op if the
    /// buffer is already loaded or has no URI.
    void load_buffer_data(std::size_t buffer_index,
                          const std::filesystem::path& gltf_dir);

    /// Get a typed view into an accessor's data. Returns a pointer to the
    /// first element; the caller interprets it based on component_type +
    /// type + count. Returns nullptr if the accessor or its buffer view
    /// is invalid, or if the buffer data isn't loaded.
    [[nodiscard]] const uint8_t* accessor_data(std::size_t accessor_index) const;

    /// Convenience: read a single VEC3 float from an accessor at a given
    /// index. Returns nullopt if the accessor is invalid or the data isn't
    /// loaded. Used by the renderer to read POSITION/NORMAL attributes.
    [[nodiscard]] std::optional<std::array<float, 3>>
    read_vec3_float(std::size_t accessor_index, std::size_t element_index) const;

    /// Convenience: read a single uint32 index from an accessor. Returns
    /// nullopt if invalid. Used by the renderer to read triangle indices.
    [[nodiscard]] std::optional<uint32_t>
    read_index_u32(std::size_t accessor_index, std::size_t element_index) const;

    /// Count of nodes with f4 extras of a given kind.
    [[nodiscard]] std::size_t count_f4_nodes(const std::string& kind) const noexcept;

    /// Find a node by f4 kind + id. Returns nullptr if not found.
    [[nodiscard]] const Node* find_f4_node(const std::string& kind,
                                             const std::string& id) const noexcept;
};

// ── f4 tag grammar helpers (ASSET_PIPELINE_SPEC §6.1) ─────────────────────

/// Parse a node name into its (kind, id) pair. Returns false if the name
/// doesn't conform to the `<kind>:<id>[.<instance>]` grammar.
[[nodiscard]] bool parse_f4_node_name(const std::string& name,
                                        std::string& kind,
                                        std::string& id);

/// True if `kind` is one of the reserved kinds: dof, sw, slot, anchor, lod, col.
[[nodiscard]] bool is_reserved_kind(const std::string& kind) noexcept;

} // namespace f4::gltf
