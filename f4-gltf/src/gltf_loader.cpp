// f4-gltf/src/gltf_loader.cpp
//
// Minimal glTF 2.0 reader. Uses f4::json::Reader for the JSON parsing.
// Reads the subset of glTF that f4import models emits: buffers, buffer
// views, accessors, meshes, primitives, nodes, scenes, and the f4
// extras schema for tagged nodes.

#include <f4/gltf/gltf_loader.hpp>
#include <f4/json/reader.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace f4::gltf {

namespace {

// ── JSON helpers ───────────────────────────────────────────────────────────
//
// f4::json::Reader walks the JSON structurally. The helpers below wrap
// the "read an array of objects" and "read a fixed-size array" patterns.

// Read an array of numbers into a vector<double>.
std::vector<double> read_number_array(f4::json::Reader& r) {
    std::vector<double> out;
    r.skip_ws();
    r.expect('[');
    while (!r.consume(']')) {
        out.push_back(r.read_number());
        r.skip_ws();
        (void)r.consume(',');
    }
    return out;
}

// Read a 4x4 or 3 or 4-element array into std::array<double, N>.
template <std::size_t N>
std::optional<std::array<double, N>> read_fixed_array(f4::json::Reader& r) {
    r.skip_ws();
    r.expect('[');
    std::array<double, N> arr{};
    std::size_t i = 0;
    while (!r.consume(']')) {
        if (i >= N) {
            r.skip_value();
        } else {
            arr[i++] = r.read_number();
        }
        r.skip_ws();
        (void)r.consume(',');
    }
    if (i < N) return std::nullopt;
    return arr;
}

// Read the f4 extras block from a node. The reader must be positioned
// at the opening '{' of the extras object.
F4Extras read_f4_extras(f4::json::Reader& r) {
    F4Extras f;
    r.skip_ws();
    r.expect('{');
    while (!r.consume('}')) {
        r.skip_ws();
        std::string key = r.read_string();
        r.expect(':');
        if (key == "v") {
            f.version = static_cast<int>(r.read_int());
        } else if (key == "kind") {
            f.kind = r.read_string();
        } else if (key == "id") {
            f.id = r.read_string();
        } else if (key == "index") {
            // Used by dof, sw, slot. The meaning depends on kind —
            // stored generically, the caller interprets.
            int idx = static_cast<int>(r.read_int());
            f.dof_index = idx;
            f.sw_index = idx;
            f.slot_index = idx;
        } else if (key == "min") {
            f.dof_min = static_cast<float>(r.read_number());
        } else if (key == "max") {
            f.dof_max = static_cast<float>(r.read_number());
        } else if (key == "mult") {
            f.dof_mult = static_cast<float>(r.read_number());
        } else if (key == "flags") {
            f.dof_flags = static_cast<int>(r.read_int());
        } else if (key == "child") {
            f.sw_child = static_cast<int>(r.read_int());
        } else if (key == "state") {
            f.sw_state = r.read_string();
        } else if (key == "station") {
            f.slot_station = static_cast<int>(r.read_int());
        } else if (key == "level") {
            f.lod_level = static_cast<int>(r.read_int());
        } else if (key == "billboard") {
            f.lod_billboard = r.read_bool();
        } else if (key == "shape") {
            f.col_shape = r.read_string();
        } else if (key == "material") {
            f.col_material = r.read_string();
        } else {
            r.skip_value();
        }
        r.skip_ws();
        (void)r.consume(',');
    }
    return f;
}

// Read a primitive object. Reader positioned at '{'.
Primitive read_primitive(f4::json::Reader& r) {
    Primitive p;
    r.skip_ws();
    r.expect('{');
    while (!r.consume('}')) {
        r.skip_ws();
        std::string key = r.read_string();
        r.expect(':');
        if (key == "attributes") {
            // Spec-compliant form: "attributes": { "POSITION": n, ... }
            r.skip_ws();
            r.expect('{');
            while (!r.consume('}')) {
                r.skip_ws();
                std::string attr = r.read_string();
                r.expect(':');
                if (attr == "POSITION") {
                    p.positions = static_cast<std::size_t>(r.read_int());
                } else if (attr == "NORMAL") {
                    p.normals = static_cast<std::size_t>(r.read_int());
                } else if (attr == "TEXCOORD_0") {
                    p.texcoords0 = static_cast<std::size_t>(r.read_int());
                } else if (attr == "COLOR_0") {
                    p.colors0 = static_cast<std::size_t>(r.read_int());
                } else {
                    r.skip_value();
                }
                r.skip_ws();
                (void)r.consume(',');
            }
        } else if (key == "POSITION") {
            // Legacy flat form emitted by f4import < 0.5.0.
            p.positions = static_cast<std::size_t>(r.read_int());
        } else if (key == "NORMAL") {
            p.normals = static_cast<std::size_t>(r.read_int());
        } else if (key == "TEXCOORD_0") {
            p.texcoords0 = static_cast<std::size_t>(r.read_int());
        } else if (key == "COLOR_0") {
            p.colors0 = static_cast<std::size_t>(r.read_int());
        } else if (key == "indices") {
            p.indices = static_cast<std::size_t>(r.read_int());
        } else if (key == "mode") {
            p.mode = static_cast<int>(r.read_int());
        } else {
            r.skip_value();
        }
        r.skip_ws();
        (void)r.consume(',');
    }
    return p;
}

// Read a mesh object. Reader positioned at '{'.
Mesh read_mesh(f4::json::Reader& r) {
    Mesh m;
    r.skip_ws();
    r.expect('{');
    while (!r.consume('}')) {
        r.skip_ws();
        std::string key = r.read_string();
        r.expect(':');
        if (key == "name") {
            m.name = r.read_string();
        } else if (key == "primitives") {
            r.skip_ws();
            r.expect('[');
            while (!r.consume(']')) {
                m.primitives.push_back(read_primitive(r));
                r.skip_ws();
                (void)r.consume(',');
            }
        } else {
            r.skip_value();
        }
        r.skip_ws();
        (void)r.consume(',');
    }
    return m;
}

// Read a node object. Reader positioned at '{'.
Node read_node(f4::json::Reader& r) {
    Node n;
    r.skip_ws();
    r.expect('{');
    while (!r.consume('}')) {
        r.skip_ws();
        std::string key = r.read_string();
        r.expect(':');
        if (key == "name") {
            n.name = r.read_string();
        } else if (key == "mesh") {
            n.mesh = static_cast<std::size_t>(r.read_int());
        } else if (key == "children") {
            r.skip_ws();
            r.expect('[');
            while (!r.consume(']')) {
                n.children.push_back(static_cast<std::size_t>(r.read_int()));
                r.skip_ws();
                (void)r.consume(',');
            }
        } else if (key == "matrix") {
            n.matrix = read_fixed_array<16>(r);
        } else if (key == "translation") {
            n.translation = read_fixed_array<3>(r);
        } else if (key == "rotation") {
            n.rotation = read_fixed_array<4>(r);
        } else if (key == "scale") {
            n.scale = read_fixed_array<3>(r);
        } else if (key == "extras") {
            // The extras object may contain an "f4" key.
            r.skip_ws();
            r.expect('{');
            while (!r.consume('}')) {
                r.skip_ws();
                std::string ek = r.read_string();
                r.expect(':');
                if (ek == "f4") {
                    n.f4 = read_f4_extras(r);
                    n.has_f4 = true;
                } else {
                    r.skip_value();
                }
                r.skip_ws();
                (void)r.consume(',');
            }
        } else {
            r.skip_value();
        }
        r.skip_ws();
        (void)r.consume(',');
    }
    return n;
}

} // namespace

// ── GltfDocument ───────────────────────────────────────────────────────────

void GltfDocument::load_from_string(const std::string& json) {
    f4::json::Reader r(json);

    r.skip_ws();
    r.expect('{');
    while (!r.consume('}')) {
        r.skip_ws();
        std::string key = r.read_string();
        r.expect(':');

        if (key == "scene") {
            scene = static_cast<int>(r.read_int());
        } else if (key == "scenes") {
            r.skip_ws();
            r.expect('[');
            while (!r.consume(']')) {
                Scene s;
                r.skip_ws();
                r.expect('{');
                while (!r.consume('}')) {
                    r.skip_ws();
                    std::string sk = r.read_string();
                    r.expect(':');
                    if (sk == "nodes") {
                        r.skip_ws();
                        r.expect('[');
                        while (!r.consume(']')) {
                            s.nodes.push_back(static_cast<std::size_t>(r.read_int()));
                            r.skip_ws();
                            (void)r.consume(',');
                        }
                    } else {
                        r.skip_value();
                    }
                    r.skip_ws();
                    (void)r.consume(',');
                }
                scenes.push_back(std::move(s));
                r.skip_ws();
                (void)r.consume(',');
            }
        } else if (key == "nodes") {
            r.skip_ws();
            r.expect('[');
            while (!r.consume(']')) {
                nodes.push_back(read_node(r));
                r.skip_ws();
                (void)r.consume(',');
            }
        } else if (key == "meshes") {
            r.skip_ws();
            r.expect('[');
            while (!r.consume(']')) {
                meshes.push_back(read_mesh(r));
                r.skip_ws();
                (void)r.consume(',');
            }
        } else if (key == "accessors") {
            r.skip_ws();
            r.expect('[');
            while (!r.consume(']')) {
                Accessor a;
                r.skip_ws();
                r.expect('{');
                while (!r.consume('}')) {
                    r.skip_ws();
                    std::string ak = r.read_string();
                    r.expect(':');
                    if (ak == "bufferView") {
                        a.buffer_view = static_cast<std::size_t>(r.read_int());
                    } else if (ak == "byteOffset") {
                        a.byte_offset = static_cast<std::size_t>(r.read_int());
                    } else if (ak == "componentType") {
                        a.component_type = static_cast<int>(r.read_int());
                    } else if (ak == "normalized") {
                        a.normalized = r.read_bool();
                    } else if (ak == "count") {
                        a.count = static_cast<std::size_t>(r.read_int());
                    } else if (ak == "type") {
                        a.type = r.read_string();
                    } else if (ak == "min") {
                        a.min = read_number_array(r);
                    } else if (ak == "max") {
                        a.max = read_number_array(r);
                    } else {
                        r.skip_value();
                    }
                    r.skip_ws();
                    (void)r.consume(',');
                }
                accessors.push_back(std::move(a));
                r.skip_ws();
                (void)r.consume(',');
            }
        } else if (key == "bufferViews") {
            r.skip_ws();
            r.expect('[');
            while (!r.consume(']')) {
                BufferView bv;
                r.skip_ws();
                r.expect('{');
                while (!r.consume('}')) {
                    r.skip_ws();
                    std::string bk = r.read_string();
                    r.expect(':');
                    if (bk == "buffer") {
                        bv.buffer = static_cast<std::size_t>(r.read_int());
                    } else if (bk == "byteOffset") {
                        bv.byte_offset = static_cast<std::size_t>(r.read_int());
                    } else if (bk == "byteLength") {
                        bv.byte_length = static_cast<std::size_t>(r.read_int());
                    } else if (bk == "byteStride") {
                        bv.byte_stride = static_cast<std::size_t>(r.read_int());
                    } else if (bk == "target") {
                        bv.target = static_cast<int>(r.read_int());
                    } else {
                        r.skip_value();
                    }
                    r.skip_ws();
                    (void)r.consume(',');
                }
                buffer_views.push_back(std::move(bv));
                r.skip_ws();
                (void)r.consume(',');
            }
        } else if (key == "buffers") {
            r.skip_ws();
            r.expect('[');
            while (!r.consume(']')) {
                Buffer b;
                r.skip_ws();
                r.expect('{');
                while (!r.consume('}')) {
                    r.skip_ws();
                    std::string bk = r.read_string();
                    r.expect(':');
                    if (bk == "byteLength") {
                        b.byte_length = static_cast<std::size_t>(r.read_int());
                    } else if (bk == "uri") {
                        b.uri = r.read_string();
                    } else {
                        r.skip_value();
                    }
                    r.skip_ws();
                    (void)r.consume(',');
                }
                buffers.push_back(std::move(b));
                r.skip_ws();
                (void)r.consume(',');
            }
        } else {
            // Skip unknown top-level fields (asset, materials, images,
            // textures, samplers, etc. — not needed by the runtime loader
            // for the f4import models output).
            r.skip_value();
        }
        r.skip_ws();
        (void)r.consume(',');
    }
}

void GltfDocument::load(const std::filesystem::path& gltf_path) {
    std::ifstream f(gltf_path);
    if (!f) throw std::runtime_error("GltfDocument: cannot open " + gltf_path.string());
    std::ostringstream ss;
    ss << f.rdbuf();
    load_from_string(ss.str());

    // Auto-load external .bin buffers relative to the .gltf file's dir.
    auto dir = gltf_path.parent_path();
    for (std::size_t i = 0; i < buffers.size(); ++i) {
        if (!buffers[i].data.empty()) continue;
        if (buffers[i].uri.empty()) continue;
        // Skip base64 data URIs (not used by f4import models, but
        // handle gracefully for test fixtures).
        if (buffers[i].uri.substr(0, 5) == "data:") continue;
        load_buffer_data(i, dir);
    }
}

void GltfDocument::load_buffer_data(std::size_t buffer_index,
                                     const std::filesystem::path& gltf_dir) {
    if (buffer_index >= buffers.size()) return;
    Buffer& b = buffers[buffer_index];
    if (b.data.empty() && !b.uri.empty()) {
        auto path = gltf_dir / b.uri;
        std::ifstream f(path, std::ios::binary);
        if (!f) throw std::runtime_error("GltfDocument: cannot open buffer " + path.string());
        b.data.assign(std::istreambuf_iterator<char>(f),
                       std::istreambuf_iterator<char>());
    }
}

const uint8_t* GltfDocument::accessor_data(std::size_t accessor_index) const {
    if (accessor_index >= accessors.size()) return nullptr;
    const Accessor& a = accessors[accessor_index];
    if (a.buffer_view >= buffer_views.size()) return nullptr;
    const BufferView& bv = buffer_views[a.buffer_view];
    if (bv.buffer >= buffers.size()) return nullptr;
    const Buffer& b = buffers[bv.buffer];
    if (b.data.empty()) return nullptr;
    std::size_t offset = bv.byte_offset + a.byte_offset;
    if (offset >= b.data.size()) return nullptr;
    return b.data.data() + offset;
}

std::optional<std::array<float, 3>>
GltfDocument::read_vec3_float(std::size_t accessor_index,
                               std::size_t element_index) const {
    if (accessor_index >= accessors.size()) return std::nullopt;
    const Accessor& a = accessors[accessor_index];
    if (a.type != "VEC3" || a.component_type != 5126) return std::nullopt;  // FLOAT
    const uint8_t* base = accessor_data(accessor_index);
    if (!base) return std::nullopt;
    if (element_index >= a.count) return std::nullopt;
    const float* p = reinterpret_cast<const float*>(base) + element_index * 3;
    return std::array<float, 3>{p[0], p[1], p[2]};
}

std::optional<uint32_t>
GltfDocument::read_index_u32(std::size_t accessor_index,
                              std::size_t element_index) const {
    if (accessor_index >= accessors.size()) return std::nullopt;
    const Accessor& a = accessors[accessor_index];
    if (a.type != "SCALAR") return std::nullopt;
    const uint8_t* base = accessor_data(accessor_index);
    if (!base) return std::nullopt;
    if (element_index >= a.count) return std::nullopt;
    // f4import models emits uint32 indices.
    if (a.component_type == 5125) {  // UNSIGNED_INT
        const uint32_t* p = reinterpret_cast<const uint32_t*>(base);
        return p[element_index];
    }
    if (a.component_type == 5123) {  // UNSIGNED_SHORT
        const uint16_t* p = reinterpret_cast<const uint16_t*>(base);
        return static_cast<uint32_t>(p[element_index]);
    }
    if (a.component_type == 5121) {  // UNSIGNED_BYTE
        return static_cast<uint32_t>(base[element_index]);
    }
    return std::nullopt;
}

std::size_t GltfDocument::count_f4_nodes(const std::string& kind) const noexcept {
    std::size_t n = 0;
    for (const auto& node : nodes) {
        if (node.has_f4 && node.f4.kind == kind) ++n;
    }
    return n;
}

const Node* GltfDocument::find_f4_node(const std::string& kind,
                                        const std::string& id) const noexcept {
    for (const auto& node : nodes) {
        if (node.has_f4 && node.f4.kind == kind && node.f4.id == id) {
            return &node;
        }
    }
    return nullptr;
}

// ── f4 tag grammar helpers ─────────────────────────────────────────────────

bool parse_f4_node_name(const std::string& name,
                          std::string& kind,
                          std::string& id) {
    auto colon = name.find(':');
    if (colon == std::string::npos) return false;
    kind = name.substr(0, colon);
    if (!is_reserved_kind(kind)) return false;
    id = name.substr(colon + 1);
    if (id.empty()) return false;
    return true;
}

bool is_reserved_kind(const std::string& kind) noexcept {
    return kind == "dof" || kind == "sw" || kind == "slot" ||
           kind == "anchor" || kind == "lod" || kind == "col";
}

} // namespace f4::gltf
