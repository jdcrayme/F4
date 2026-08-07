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

// ============================================================================
// Format constants — KoreaObj.HDR / KoreaObj.DXH header format.
//
// Sources: FreeFalcon src/graphics/bsplib/objectparent.cpp (ReadParentFile)
// and the BMS derived format. Constants gathered here so the parser below
// reads as a description of the format rather than a sea of bare integers.
// ============================================================================
constexpr uint32_t FORMAT_VERSION = 0x03087000;  // FreeFalcon HDR format v3.08.70.00 — written by WriteParentFile
constexpr uint32_t OLD_FORMAT_SENTINEL = 0xFEEF; // TextureBank.maxCS value indicating pre-DX (classic BSP) format.
                                                  // When maxCS == 0xFEEF the file has no DX texture slots; otherwise
                                                  // it's the new-format DX texture count.
constexpr int S_PFR      = 48;   // ParentFileRecord on disk
constexpr int S_LODREC   = 8;    // LOD record (obj + maxRange)
constexpr int S_PPOINT   = 12;   // 3 floats
constexpr int S_PCOLOR   = 16;
constexpr int S_DPAL     = 1032;
constexpr int S_TEXENTRY = 40;
constexpr int LOD_ENTRY_BYTES = 20;   // 12 spare + 4 offset + 4 size
constexpr int LOD_ENTRY_SPARE = 12;   // unknown 12 bytes preceding offset+size

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

    // Read the ColorBank as n_colors × 16 bytes (4 floats: r, g, b, a).
    // FreeFalcon's Pcolor is `struct Pcolor { float r, g, b, a; };` —
    // see `graphics/include/grtypes.h`. Written by
    // `ColorBankClass::ReadPool` in `graphics/bsplib/colorbank.cpp`.
    //
    // We pre-multiply into a uint8 RGBA to make the runtime path cheap
    // (viewer calls rgba_at(index) and unpacks to Raylib Color).
    result.color_bank.colors.clear();
    result.color_bank.colors.reserve(static_cast<std::size_t>(result.n_colors));
    result.color_bank.n_darkened = result.n_dark_colors;
    for (int i = 0; i < result.n_colors; ++i) {
        float fr = 0, fg = 0, fb = 0, fa = 0;
        if (!r.read(fr) || !r.read(fg) || !r.read(fb) || !r.read(fa)) {
            err = "HDR truncated in ColorBank entry " + std::to_string(i);
            return false;
        }
        ColorEntry c;
        // Falcon floats are typically 0..1; clamp just in case.
        auto clamp_f = [](float v) -> uint8_t {
            if (v <= 0.0f) return 0;
            if (v >= 1.0f) return 255;
            return static_cast<uint8_t>(v * 255.0f + 0.5f);
        };
        c.r = clamp_f(fr);
        c.g = clamp_f(fg);
        c.b = clamp_f(fb);
        c.a = clamp_f(fa);
        // Many Pcolor entries have a=0 in the source data; treat 0 alpha
        // as fully opaque (Falcon treats them as opaque; alpha 0 was the
        // engine's "darkened" placeholder). Only PolyTex* variants with
        // a real chroma key actually use alpha.
        if (c.a == 0 && (c.r != 0 || c.g != 0 || c.b != 0)) {
            c.a = 255;
        }
        result.color_bank.colors.push_back(c);
    }

    // PaletteBank
    if (!r.read(result.n_palettes)) {
        err = "HDR truncated in PaletteBank header";
        return false;
    }
    // Read each palette: 256 × 4-byte ARGB entries + 8 bytes padding = 1032 bytes
    result.palettes.reserve(static_cast<std::size_t>(result.n_palettes));
    for (int i = 0; i < result.n_palettes; ++i) {
        DiskPalette pal;
        // Read 256 ARGB uint32 entries
        for (int j = 0; j < 256; ++j) {
            uint32_t argb;
            if (!r.read(argb)) {
                err = "HDR truncated in palette " + std::to_string(i) +
                      " entry " + std::to_string(j);
                return false;
            }
            pal.colors[static_cast<std::size_t>(j)] = argb;
        }
        // Skip 8 bytes of padding (1032 - 256*4 = 8)
        if (!r.skip(8)) {
            err = "HDR truncated in palette " + std::to_string(i) + " padding";
            return false;
        }
        result.palettes.push_back(pal);
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
    result.is_new_format = (max_cs != OLD_FORMAT_SENTINEL);
    // Read each texture entry: 40 bytes on disk (10 × uint32).
    //
    // The on-disk layout corresponds to FreeFalcon's TempTexBankEntry
    // struct (texbank.h), which contains a Texture object by value.
    // The original Falcon 4.0 Texture struct (24 bytes on x86) has
    // an extra 4-byte field between imageData and flags that is not
    // present in modern FreeFalcon's DiskTexture — likely a remnant
    // of the original DX5-era struct (possibly a secondary image
    // pointer or a dimensions duplicate). The empirical layout,
    // verified by hex-dumping raw HDR bytes and cross-referencing
    // with known flag values (0xA0 = MPR_TI_CHROMAKEY|ALPHA|PALETTE)
    // and chroma-key values (0xFFFF0000 = blue), is:
    //
    //   [0]  fileOffset       — byte offset into .TEX file
    //   [1]  fileSize         — compressed size in bytes
    //   [2]  dimension        — texture width = height (power of 2)
    //   [3]  imageData (ptr)  — always 0 on disk (runtime pointer)
    //   [4]  unused           — always 0 (legacy field / padding)
    //   [5]  flags            — MPR_TI_* flags (0xA0=palette+alpha+chroma)
    //   [6]  chromaKey        — transparency color key (ABGR format)
    //   [7]  palette (ptr)    — always 0 on disk (runtime pointer)
    //   [8]  palID            — index into PaletteBank
    //   [9]  refCount         — always 0 on disk (runtime counter)
    //
    // CRITICAL: palID is at index [8], NOT [3]. The previous code
    // read palette_id from vals[3] (the imageData pointer, always 0),
    // causing ALL textures to use palette 0. Textures that needed
    // palette 1, 2, or 3 were resolved through the wrong palette and
    // appeared as random noise.
    //
    // References:
    //   FreeFalcon: src/graphics/include/texbank.h (TempTexBankEntry)
    //   FreeFalcon: src/graphics/include/tex.h (Texture class)
    //   FreeFalcon: src/graphics/include/context.h (MPR_TI_* flags)
    result.tex_entries.reserve(static_cast<std::size_t>(result.n_textures));
    for (int i = 0; i < result.n_textures; ++i) {
        uint32_t vals[10];
        for (int j = 0; j < 10; ++j) {
            if (!r.read(vals[j])) {
                err = "HDR truncated in TextureBank entry " + std::to_string(i);
                return false;
            }
        }
        TexBankEntry entry;
        entry.file_offset = vals[0];
        entry.file_size   = vals[1];
        entry.dimension   = vals[2];
        entry.palette_id  = static_cast<int32_t>(vals[8]);  // palID at [8]
        entry.flags       = vals[5];
        entry.chroma_key  = vals[6];
        result.tex_entries.push_back(entry);
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
        // Each entry is LOD_ENTRY_BYTES: LOD_ENTRY_SPARE spare + 4 offset + 4 size
        if (!r.skip(LOD_ENTRY_SPARE) || !r.read(e.offset) || !r.read(e.size)) {
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
