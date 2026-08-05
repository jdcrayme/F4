// f4-models/src/hdr_parser.cpp
//
// Implementation of HDR binary format parsing.
//
// KoreaObj.HDR / KoreaObj.DXH layout:
//   [0..3]     uint32  FORMAT_VERSION  (0x03087000)
//   [4..]      ColorBank
//   [..]       PaletteBank
//   [..]       TextureBank
//   [..]       int     maxTagList
//   [..]       int     TheObjectLODsCount
//   [..]       LOD entries (20 bytes each: 12 spare + 4 offset + 4 size)
//   [..]       int     TheObjectListLength
//   [..]       ParentFileRecord[TheObjectListLength] (48 bytes each)
//   [..]       Per-parent: slot positions + LOD records
//
// References:
//   FreeFalcon: src/graphics/bsplib/objectparent.cpp (ReadParentFile)
//   f4-world-viewer: src/model_snapshot.cpp (parse_hdr)

#include "hdr_parser.hpp"
#include "bin_reader.hpp"

#include <algorithm>

namespace f4::models::detail {

namespace {

constexpr uint32_t FORMAT_VERSION = 0x03087000;
constexpr int S_PFR      = 48;   // ParentFileRecord on disk
constexpr int S_LODREC   = 8;    // LOD record (obj + maxRange)
constexpr int S_PPOINT   = 12;   // 3 floats
constexpr int S_PCOLOR   = 16;
constexpr int S_DPAL     = 1032;
constexpr int S_TEXENTRY = 40;

} // anonymous namespace

bool parse_hdr(
    const uint8_t* data, std::size_t size,
    HdrParseResult& result,
    std::string& err)
{
    BinReader r{data, size};

    // Version
    if (!r.read(result.version)) {
        err = "HDR too small for version";
        return false;
    }

    // ColorBank
    if (!r.read(result.n_colors) || !r.read(result.n_dark_colors)) {
        err = "HDR truncated in ColorBank header";
        return false;
    }
    if (!r.skip(result.n_colors * S_PCOLOR)) {
        err = "HDR truncated in ColorBank data";
        return false;
    }

    // PaletteBank
    if (!r.read(result.n_palettes)) {
        err = "HDR truncated in PaletteBank header";
        return false;
    }
    if (!r.skip(result.n_palettes * S_DPAL)) {
        err = "HDR truncated in PaletteBank data";
        return false;
    }

    // TextureBank
    int32_t n_tex;
    if (!r.read(n_tex)) {
        err = "HDR truncated in TextureBank header";
        return false;
    }
    result.n_textures = n_tex;
    int32_t max_cs;
    if (!r.read(max_cs)) {
        err = "HDR truncated in TextureBank maxCS";
        return false;
    }
    result.is_new_format = (max_cs != 0xFEEF);
    if (!r.skip(result.n_textures * S_TEXENTRY)) {
        err = "HDR truncated in TextureBank data";
        return false;
    }

    // LOD table
    if (!r.read(result.max_tags)) {
        err = "HDR truncated at maxTags";
        return false;
    }
    if (!r.read(result.n_lod_entries)) {
        err = "HDR truncated at nLodEntries";
        return false;
    }

    result.lod_entries.reserve(result.n_lod_entries);
    for (int i = 0; i < result.n_lod_entries; ++i) {
        LodTableEntry e;
        e.index = i;
        // Each entry is 20 bytes: 12 unknown + 4 offset + 4 size
        if (!r.skip(12) || !r.read(e.offset) || !r.read(e.size)) {
            err = "HDR truncated in LOD table entry " + std::to_string(i);
            return false;
        }
        result.lod_entries.push_back(e);
    }

    // Parent list
    if (!r.read(result.n_parents)) {
        err = "HDR truncated at nParents";
        return false;
    }

    result.parents.reserve(result.n_parents);
    for (int i = 0; i < result.n_parents; ++i) {
        ModelRecord p{};
        p.index = i;

        // Read 48-byte ParentFileRecord
        auto start = r.pos;
        if (!r.read(p.radius)) {
            err = "HDR truncated in parent " + std::to_string(i);
            return false;
        }
        if (!r.read(p.bbox.min_x) || !r.read(p.bbox.max_x)) {
            err = "HDR truncated in parent bbox X";
            return false;
        }
        if (!r.read(p.bbox.min_y) || !r.read(p.bbox.max_y)) {
            err = "HDR truncated in parent bbox Y";
            return false;
        }
        if (!r.read(p.bbox.min_z) || !r.read(p.bbox.max_z)) {
            err = "HDR truncated in parent bbox Z";
            return false;
        }
        if (!r.read(p.radar_signature) || !r.read(p.ir_signature)) {
            err = "HDR truncated in parent signatures";
            return false;
        }
        if (!r.read(p.n_texture_sets) || !r.read(p.n_dynamic_coords)) {
            err = "HDR truncated in parent texSet/dynCoord";
            return false;
        }
        if (!r.read(p.n_lods) || !r.read(p.n_switch) ||
            !r.read(p.n_dof) || !r.read(p.n_slots)) {
            err = "HDR truncated in parent LOD/switch/DOF/slot counts";
            return false;
        }
        if (!r.read(p.n_switches) || !r.read(p.n_dofs)) {
            err = "HDR truncated in parent extended switch/DOF counts";
            return false;
        }
        // Ensure we consumed exactly 48 bytes
        r.seek(start + S_PFR);

        result.parents.push_back(p);
    }

    // Detect LOD names (same heuristic as FreeFalcon and Python script)
    auto try_parse = [&](bool with_names) -> bool {
        auto base = r.pos;          // save entry position
        auto save_pos = r.pos;
        for (auto& p : result.parents) {
            if (p.n_lods == 0) continue;
            int nsd = p.n_slots + p.n_dynamic_coords;
            auto skip = static_cast<std::size_t>(nsd) * S_PPOINT +
                        static_cast<std::size_t>(p.n_lods) * (S_LODREC + (with_names ? 32 : 0));
            if (save_pos + skip > size) {
                r.pos = base;       // restore on failure
                return false;
            }
            save_pos += skip;
        }
        // Check if computed end matches file size
        bool matches = (save_pos == size);
        r.pos = base;               // ALWAYS restore position
        return matches;
    };

    result.has_lod_names = false;
    if (!try_parse(false)) {
        if (try_parse(true)) {
            result.has_lod_names = true;
        }
    }

    // Parse slot positions + LOD records for each parent
    for (auto& p : result.parents) {
        if (p.n_lods == 0) continue;
        int nsd = p.n_slots + p.n_dynamic_coords;

        // Read slot positions (nSlots + nDynamicCoords points)
        if (nsd > 0) {
            for (int j = 0; j < nsd; ++j) {
                Vec3 pt;
                if (!r.read(pt.x) || !r.read(pt.y) || !r.read(pt.z)) {
                    err = "HDR truncated reading slot/dynamic coord for parent " +
                          std::to_string(p.index);
                    return false;
                }
                if (j < p.n_slots) {
                    // Slot position (rotation is not stored in HDR — only in the
                    // BSP tree's BSlotNode). Store the position.
                    SlotInfo si;
                    si.position = pt;
                    p.slots.push_back(si);
                } else {
                    p.dynamic_coord_defaults.push_back(pt);
                }
            }
        }

        // Read LOD records
        for (int j = 0; j < p.n_lods; ++j) {
            LodRef lr;
            if (result.has_lod_names) {
                char name[33] = {};
                if (!r.read_bytes(name, 32)) {
                    err = "HDR truncated reading LOD name";
                    return false;
                }
                name[32] = '\0';
                lr.name = name;
                // Trim trailing nulls
                while (!lr.name.empty() && lr.name.back() == '\0')
                    lr.name.pop_back();
            }
            uint32_t obj;
            if (!r.read(obj) || !r.read(lr.max_range)) {
                err = "HDR truncated reading LOD record";
                return false;
            }
            lr.lod_table_idx = static_cast<int>(obj >> 1);
            p.lods.push_back(lr);
        }
    }

    return true;
}

} // namespace f4::models::detail
