// dump_lod_info.cpp
// Dump detailed info about each LOD of a model, including whether
// the LOD data parses successfully and what format it is.

#include <f4/models/model_database.hpp>
#include <f4/models/model_lod.hpp>
#include <f4/models/bsp_node.hpp>

#include "bsp_parser.hpp"
#include "dx_parser.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <filesystem>

int main(int argc, char* argv[]) {
    std::filesystem::path hdr_path = "../temp/KoreaObj.HDR";
    std::filesystem::path lod_path = "../temp/KoreaObj.LOD";
    int target_parent = 829;

    if (argc >= 2) hdr_path = argv[1];
    if (argc >= 3) lod_path = argv[2];
    if (argc >= 4) target_parent = std::atoi(argv[3]);

    f4::models::ModelDatabase db;
    std::string err = db.load(hdr_path, lod_path);
    if (!err.empty()) { printf("ERROR: %s\n", err.c_str()); return 1; }

    const auto* rec = db.model(target_parent);
    if (!rec) { printf("ERROR: model %d not found\n", target_parent); return 1; }

    printf("Model %d: %s, %zu LODs\n",
            target_parent, rec->visual_class().data(), rec->lods.size());

    const auto& lod_table = db.lod_table();

    for (std::size_t li = 0; li < rec->lods.size(); ++li) {
        const auto& lod_ref = rec->lods[li];
        int idx = lod_ref.lod_table_idx;
        printf("\nLOD %zu: name='%s' lod_table_idx=%d max_range=%.1f\n",
                li, lod_ref.name.c_str(), idx, lod_ref.max_range);

        if (idx < 0 || idx >= static_cast<int>(lod_table.size())) {
            printf("  ERROR: lod_table_idx out of range (table has %zu entries)\n",
                    lod_table.size());
            continue;
        }

        const auto& entry = lod_table[static_cast<std::size_t>(idx)];
        printf("  LOD table entry: offset=%u size=%u\n", entry.offset, entry.size);

        if (entry.offset == 0 || entry.size == 0) {
            printf("  EMPTY (offset=0 or size=0)\n");
            continue;
        }

        // Check format from first 4 bytes
        // We need to read the raw LOD file data. The ModelDatabase keeps
        // it private, so let's read it ourselves.
        std::ifstream f(lod_path, std::ios::binary);
        if (!f) { printf("  cannot open LOD file\n"); continue; }
        f.seekg(entry.offset);
        std::vector<uint8_t> buf(entry.size);
        f.read(reinterpret_cast<char*>(buf.data()), entry.size);
        if (!f) { printf("  read error\n"); continue; }

        uint32_t first4;
        std::memcpy(&first4, buf.data(), 4);
        bool is_dx = f4::models::detail::is_dx_format(first4);
        printf("  first4=0x%08X  format=%s\n", first4, is_dx ? "DX" : "BSP");

        if (is_dx) {
            f4::models::DxLodData dx_data;
            std::string perr;
            bool ok = f4::models::detail::parse_dx_lod(buf.data(), buf.size(),
                                                       dx_data, perr);
            if (ok) {
                printf("  DX parse OK: %zu vertices, %zu stream bytes\n",
                        dx_data.vertices.size(), dx_data.node_stream.size());
            } else {
                printf("  DX parse FAILED: %s\n", perr.c_str());
            }
        } else {
            f4::models::BspTree tree;
            std::string perr;
            bool ok = f4::models::detail::parse_bsp_tree(buf.data(), buf.size(),
                                                          tree, perr);
            if (ok) {
                printf("  BSP parse OK: %zu nodes, %zu coords, %zu normals, %zu tex_ids, buffer=%zu bytes\n",
                        tree.nodes.size(), tree.coords.size(),
                        tree.normals.size(), tree.tex_ids.size(),
                        tree.lod_buffer.size());
            } else {
                printf("  BSP parse FAILED: %s\n", perr.c_str());
            }
        }

        // Try extracting geometry
        std::string perr = db.parse_lod(target_parent, static_cast<int>(li));
        if (!perr.empty()) {
            printf("  parse_lod error: %s\n", perr.c_str());
        } else {
            f4::models::ModelState state;
            auto geom = db.extract_model_geometry(target_parent, static_cast<int>(li), state);
            std::size_t total_tris = 0;
            for (const auto& m : geom.meshes) total_tris += m.triangles.size();
            printf("  geometry: %zu meshes, %zu triangles\n",
                    geom.meshes.size(), total_tris);
        }
    }

    return 0;
}
