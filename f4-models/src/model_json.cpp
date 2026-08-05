// f4-models/src/model_json.cpp
//
// JSON export for model data. Uses f4::json::Writer for compact output.
//
// This provides:
//   model_list_json()    — all models from HDR
//   model_record_json()  — one model's HDR data
//   bsp_tree_json()      — one BSP tree's structure (for debugging)

#include <f4/models/f4_models.hpp>
#include <f4/json/f4_json.hpp>

#include <cstdio>
#include <sstream>

namespace f4::models {

namespace {

void write_bounding_box(json::Writer& w, const BoundingBox& bb) {
    w.raw("[");
    w.number(bb.min_x); w.raw(",");
    w.number(bb.max_x); w.raw(",");
    w.number(bb.min_y); w.raw(",");
    w.number(bb.max_y); w.raw(",");
    w.number(bb.min_z); w.raw(",");
    w.number(bb.max_z);
    w.raw("]");
}

void write_vec3(json::Writer& w, const Vec3& v) {
    w.raw("[");
    w.number(v.x); w.raw(",");
    w.number(v.y); w.raw(",");
    w.number(v.z);
    w.raw("]");
}

void write_lod_ref(json::Writer& w, const LodRef& lr) {
    w.raw("{");
    w.string_key("name", lr.name); w.raw(",");
    w.number_key("lod_table_idx", lr.lod_table_idx); w.raw(",");
    w.number_key("max_range", static_cast<double>(lr.max_range));
    w.raw("}");
}

void write_model_record(json::Writer& w, const ModelRecord& m) {
    w.raw("{");
    w.number_key("index", m.index); w.raw(",");

    w.string("radius"); w.raw(":"); w.number(static_cast<double>(m.radius)); w.raw(",");

    w.string("bounding_box"); w.raw(":"); write_bounding_box(w, m.bbox); w.raw(",");

    w.string("radar_signature"); w.raw(":"); w.number(static_cast<double>(m.radar_signature)); w.raw(",");
    w.string("ir_signature"); w.raw(":"); w.number(static_cast<double>(m.ir_signature)); w.raw(",");

    w.number_key("nlod", static_cast<int>(m.n_lods)); w.raw(",");
    w.number_key("nslots", static_cast<int>(m.n_slots)); w.raw(",");
    w.number_key("nswitches", static_cast<int>(m.n_switches)); w.raw(",");
    w.number_key("ndofs", static_cast<int>(m.n_dofs)); w.raw(",");
    w.number_key("ntexture_sets", static_cast<int>(m.n_texture_sets)); w.raw(",");
    w.number_key("ndynamic_coords", static_cast<int>(m.n_dynamic_coords)); w.raw(",");

    // Slot positions
    w.string("slot_positions"); w.raw(":[");
    for (int i = 0; i < static_cast<int>(m.slots.size()); ++i) {
        if (i > 0) w.raw(",");
        write_vec3(w, m.slots[i].position);
    }
    w.raw("],");

    // LODs
    w.string("lods"); w.raw(":[");
    for (int i = 0; i < static_cast<int>(m.lods.size()); ++i) {
        if (i > 0) w.raw(",");
        write_lod_ref(w, m.lods[i]);
    }
    w.raw("]");

    w.raw("}");
}

} // anonymous namespace

// ── Public JSON functions ─────────────────────────────────────────────────

/// Export the full model list as JSON.
std::string model_list_json(const ModelDatabase& db) {
    json::Writer w;
    w.raw("{\n");

    w.string("command"); w.raw(":"); w.string("list-models"); w.raw(",\n");

    w.string("hdr_file"); w.raw(":"); w.string(db.hdr_path().string()); w.raw(",\n");
    w.string("lod_file"); w.raw(":"); w.string(db.lod_path().string()); w.raw(",\n");

    // Version as hex
    {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "0x%08X", db.version());
        w.string("version"); w.raw(":"); w.string(buf); w.raw(",\n");
    }

    w.number_key("n_models", db.n_models()); w.raw(",\n");
    w.number_key("n_lod_entries", db.n_lod_entries()); w.raw(",\n");
    w.number_key("n_textures", db.n_textures()); w.raw(",\n");
    w.string("has_lod_names"); w.raw(":"); w.raw(db.has_lod_names() ? "true" : "false"); w.raw(",\n");

    w.string("models"); w.raw(":[\n");
    const auto& models = db.models();
    for (int i = 0; i < static_cast<int>(models.size()); ++i) {
        if (i > 0) w.raw(",\n");
        w.raw("  ");
        write_model_record(w, models[i]);
    }
    w.raw("\n]\n}");

    return w.str();
}

/// Export a single model record as JSON.
std::string model_record_json(const ModelRecord& m) {
    json::Writer w;
    write_model_record(w, m);
    return w.str();
}

/// Export a BSP tree structure as JSON (for debugging/inspection).
std::string bsp_tree_json(const BspTree& tree, int max_nodes) {
    json::Writer w;
    w.raw("{\n");

    w.number_key("tag_count", tree.tag_count); w.raw(",\n");
    w.number_key("data_start", tree.data_start); w.raw(",\n");
    w.number_key("data_size", tree.data_size); w.raw(",\n");
    w.number_key("n_nodes", static_cast<int>(tree.nodes.size())); w.raw(",\n");
    w.number_key("n_coords", static_cast<int>(tree.coords.size())); w.raw(",\n");
    w.number_key("n_normals", static_cast<int>(tree.normals.size())); w.raw(",\n");
    w.number_key("n_tex_ids", static_cast<int>(tree.tex_ids.size())); w.raw(",\n");

    // Tag type counts
    int type_counts[BSP_NODE_TYPE_COUNT] = {};
    for (const auto& t : tree.tags) {
        int idx = static_cast<int>(t);
        if (idx >= 0 && idx < BSP_NODE_TYPE_COUNT) type_counts[idx]++;
    }
    w.string("type_counts"); w.raw(":{");
    bool first = true;
    for (int k = 0; k < BSP_NODE_TYPE_COUNT; ++k) {
        if (type_counts[k] > 0) {
            if (!first) w.raw(",");
            w.string(bsp_node_type_name(static_cast<BspNodeType>(k)));
            w.raw(":"); w.number(type_counts[k]);
            first = false;
        }
    }
    w.raw("},\n");

    // Node list (limited)
    int n = static_cast<int>(tree.nodes.size());
    if (max_nodes > 0 && n > max_nodes) n = max_nodes;

    w.string("nodes"); w.raw(":[");
    for (int i = 0; i < n; ++i) {
        if (i > 0) w.raw(",");
        const auto& nd = tree.nodes[i];
        w.raw("{");
        w.number_key("i", i); w.raw(",");
        w.string("type"); w.raw(":"); w.string(bsp_node_type_name(nd.type)); w.raw(",");
        w.number_key("sibling", nd.sibling);

        if (nd.type == BspNodeType::BRoot || nd.type == BspNodeType::BSubTree ||
            nd.type == BspNodeType::BDofNode || nd.type == BspNodeType::BXDofNode ||
            nd.type == BspNodeType::BTransNode || nd.type == BspNodeType::BScaleNode) {
            w.raw(","); w.number_key("subtree", nd.subtree);
            w.raw(","); w.number_key("n_coords", nd.n_coords);
            w.raw(","); w.number_key("n_normals", nd.n_normals);
        }
        if (nd.type == BspNodeType::BRoot) {
            w.raw(","); w.number_key("n_tex_ids", nd.n_tex_ids);
            w.raw(","); w.number_key("script", nd.script_number);
        }
        if (nd.type == BspNodeType::BSplitterNode) {
            w.raw(","); w.number_key("front", nd.front);
            w.raw(","); w.number_key("back", nd.back);
        }
        if (nd.type == BspNodeType::BSlotNode) {
            w.raw(","); w.number_key("slot_number", nd.slot_number);
        }
        if (nd.type == BspNodeType::BSwitchNode || nd.type == BspNodeType::BXSwitchNode) {
            w.raw(","); w.number_key("switch_number", nd.switch_number);
            w.raw(","); w.number_key("n_children", nd.n_children);
        }

        w.raw("}");
    }
    if (static_cast<int>(tree.nodes.size()) > n) {
        w.raw(",{\"truncated\":true}");
    }
    w.raw("]\n}");

    return w.str();
}

} // namespace f4::models
