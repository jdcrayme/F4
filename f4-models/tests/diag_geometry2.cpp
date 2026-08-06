// Diagnostic: check geometry extraction in detail
#include <f4/models/f4_models.hpp>
#include "bsp_parser.hpp"
#include "hdr_parser.hpp"
#include "poly_parser.hpp"
#include "geometry_extractor.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

int main() {
    fs::path hdr = "/home/z/my-project/f4-repo/f4-models/tests/fixtures/KoreaObj.HDR";
    fs::path lod = "/home/z/my-project/f4-repo/f4-models/tests/fixtures/KoreaObj.LOD";

    f4::models::ModelDatabase db;
    auto err = db.load(hdr, lod);
    if (!err.empty()) { printf("Load error: %0s\n", err.c_str()); return 1; }

    printf("Loaded %d models\n", db.n_models());

    // Check model 1
    auto* m1 = db.model(1);
    if (!m1) { printf("No model 1\n"); return 1; }
    printf("Model 1: radius=%f n_slots=%d n_dof=%d n_lods=%zu\n",
           m1->radius, m1->n_slots, m1->effective_dofs(), m1->lods.size());

    // Parse LOD 0
    auto parse_err = db.parse_lod(1, 0);
    if (!parse_err.empty()) { printf("Parse error: %s\n", parse_err.c_str()); return 1; }
    printf("Parsed LOD 0 successfully\n");

    // Extract geometry
    auto geom = db.extract_model_geometry(1, 0);
    printf("Geometry: %zu meshes, %zu total tris, %zu total verts\n",
           geom.meshes.size(), geom.total_triangles(), geom.total_vertices());

    // Let's manually inspect the BSP tree
    // We need access to the parsed LOD... but it's private.
    // Let's parse directly.
    printf("\n--- Direct BSP parse for model 1 LOD 0 ---\n");

    // Read LOD file
    std::ifstream lf(lod, std::ios::binary | std::ios::ate);
    auto lod_sz = lf.tellg();
    lf.seekg(0);
    std::vector<uint8_t> lod_data(lod_sz);
    lf.read(reinterpret_cast<char*>(lod_data.data()), lod_sz);

    // Get LOD table entry
    auto& parent = db.models()[1];
    auto& lod_ref = parent.lods[0];
    int lod_idx = lod_ref.lod_table_idx;
    printf("LOD table index: %d\n", lod_idx);

    // Manually get the offset/size from the HDR
    // We can't access lod_table() easily... let's try model 1 parse differently

    // Let's just parse a model with known BSP data directly
    // Read the LOD file and manually extract a record

    // Actually, let's look at what the parsed BSP tree looks like
    // by re-parsing with the internal API

    // Read HDR to get LOD table
    std::ifstream hf(hdr, std::ios::binary | std::ios::ate);
    auto hdr_sz = hf.tellg();
    hf.seekg(0);
    std::vector<uint8_t> hdr_data(hdr_sz);
    hf.read(reinterpret_cast<char*>(hdr_data.data()), hdr_sz);

    // Parse HDR to get the LOD table
    f4::models::detail::HdrParseResult result;
    std::string hdr_err;
    if (!f4::models::detail::parse_hdr(hdr_data.data(), hdr_data.size(), result, hdr_err)) {
        printf("HDR parse error: %s\n", hdr_err.c_str());
        return 1;
    }

    // Get LOD entry for model 1, LOD 0
    auto& p1 = result.parents[1];
    auto& lr = p1.lods[0];
    int lti = lr.lod_table_idx;
    printf("Model 1 LOD 0: lod_table_idx=%d, max_range=%f, name='%s'\n",
           lti, lr.max_range, lr.name.c_str());

    auto& entry = result.lod_entries[lti];
    printf("LOD entry: offset=%u, size=%u\n", entry.offset, entry.size);

    if (entry.offset + entry.size > lod_data.size()) {
        printf("LOD entry out of bounds!\n");
        return 1;
    }

    // Parse the BSP tree directly
    f4::models::BspTree tree;
    std::string bsp_err;
    const uint8_t* lod_ptr = lod_data.data() + entry.offset;
    std::size_t lod_size = entry.size;

    if (!f4::models::detail::parse_bsp_tree(lod_ptr, lod_size, tree, bsp_err)) {
        printf("BSP parse error: %s\n", bsp_err.c_str());
        return 1;
    }

    printf("BSP tree: %zu nodes, %zu tags, %zu coords, %zu normals, %zu tex_ids\n",
           tree.nodes.size(), tree.tags.size(),
           tree.coords.size(), tree.normals.size(), tree.tex_ids.size());
    printf("  tag_count=%d, data_start=%d, data_size=%d\n",
           tree.tag_count, tree.data_start, tree.data_size);
    printf("  switch_children=%zu, lod_buffer=%zu\n",
           tree.switch_children.size(), tree.lod_buffer.size());

    // Count node types
    int type_counts[17] = {};
    for (const auto& n : tree.nodes) {
        int t = static_cast<int>(n.type);
        if (t >= 0 && t < 17) type_counts[t]++;
    }
    printf("\nNode type counts:\n");
    for (int i = 0; i < 17; i++) {
        if (type_counts[i] > 0) {
            printf("  %s: %d\n", f4::models::bsp_node_type_name(static_cast<f4::models::BspNodeType>(i)), type_counts[i]);
        }
    }

    // Show primitive nodes
    int prim_count = 0;
    for (std::size_t i = 0; i < tree.nodes.size(); i++) {
        const auto& n = tree.nodes[i];
        if (n.type == f4::models::BspNodeType::BPrimitiveNode ||
            n.type == f4::models::BspNodeType::BLitPrimitiveNode ||
            n.type == f4::models::BspNodeType::BCulledPrimitiveNode)
        {
            prim_count++;
            if (prim_count <= 10) {
                printf("\n  Prim node %zu: type=%s, prim_offset=%d",
                       i, f4::models::bsp_node_type_name(n.type), n.prim_offset);
                if (n.type == f4::models::BspNodeType::BLitPrimitiveNode) {
                    printf(", back_poly_offset=%d", n.back_poly_offset);
                }
                printf("\n");

                // Try to decode the prim
                if (n.prim_offset >= 0 && tree.lod_buffer.size() > 0) {
                    f4::models::detail::DecodedPrim prim;
                    std::string prim_err;
                    bool ok = f4::models::detail::decode_prim(
                        tree.lod_buffer.data(), tree.lod_buffer.size(),
                        n.prim_offset, prim, prim_err);
                    if (ok) {
                        printf("    Decoded: type=%s nVerts=%d xyz_indices=%zu\n",
                               f4::models::poly_type_name(prim.type),
                               prim.n_verts, prim.xyz_indices.size());
                        if (prim.n_verts > 0 && !prim.xyz_indices.empty()) {
                            printf("    xyz[0]=%d", prim.xyz_indices[0]);
                            if (prim.xyz_indices[0] >= 0 && prim.xyz_indices[0] < static_cast<int>(tree.coords.size())) {
                                auto& c = tree.coords[prim.xyz_indices[0]];
                                printf(" -> (%f, %f, %f0)", c.x, c.y, c.z);
                            }
                            printf("\n");
                        }
                    } else {
                        printf("    Decode FAILED: %s\n", prim_err.c_str());
                    }
                } else {
                    printf("    prim_offset out of bounds or no lod_buffer\n");
                }
            }
        }
    }
    printf("\nTotal prim nodes: %d\n", prim_count);

    // Show switch nodes
    for (std::size_t i = 0; i < tree.nodes.size(); i++) {
        const auto& n = tree.nodes[i];
        if (n.type == f4::models::BspNodeType::BSwitchNode ||
            n.type == f4::models::BspNodeType::BXSwitchNode) {
            printf("\n  Switch node %zu: switch_number=%d, n_children=%d, children_offset=%d, subtree=%d\n",
                   i, n.switch_number, n.n_children, n.switch_children_offset, n.subtree);
            // Show the children
            if (n.n_children > 0 && n.switch_children_offset >= 0) {
                auto base = static_cast<std::size_t>(n.switch_children_offset);
                for (int k = 0; k < n.n_children; k++) {
                    if (base + k < tree.switch_children.size()) {
                        printf("    child[%d] = %d\n", k, tree.switch_children[base + k]);
                    }
                }
            }
        }
    }

    // Now extract geometry and see what happens
    f4::models::ModelState state;
    std::string ext_err;
    auto geom2 = f4::models::detail::extract_geometry(tree, state, 0, ext_err);
    printf("\n--- Geometry Extraction Result ---\n");
    printf("Meshes: %zu, total triangles: %zu, total vertices: %zu\n",
           geom2.meshes.size(), geom2.total_triangles(), geom2.total_vertices());

    return 0;
}
