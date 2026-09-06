// f4-world-viewer/src/hex/decoders.cpp

#include <f4/viewer/decoders.hpp>

#include <f4/viewer/enum_text.hpp>   // domain_name, vu_class_name, data_type_name
#include <f4/world_types/class_table.hpp>     // unit_subtype_name, DOMAIN_*

#include "hex_utils.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>

namespace f4::viewer {

namespace {

using hex::hex_byte;
using hex::hex_dump;
using hex::ascii_preview;

/// Format an integer as hex with 0x prefix. width=0 means no padding.
std::string hex_str(uint64_t v, int width = 0) {
    std::ostringstream ss;
    ss << "0x" << std::hex;
    if (width > 0) ss << std::setfill('0') << std::setw(width);
    ss << v;
    return ss.str();
}

} // namespace

// ---------------------------------------------------------------------------
// .cam container manifest decoder
// ---------------------------------------------------------------------------

std::vector<Annotation> decode_cam_manifest(const HexModel& m) {
    std::vector<Annotation> out;
    if (!m.loaded()) return out;

    // Tranche 0d: the manifest is parsed directly here — the .cam
    // container directory is three fields (see cam_archive.hpp's format
    // comment) and the hex inspector must not link f4-world-convert (P2
    // boundary). Sub-file data extraction / LZSS stays in the importer
    // library; the inspector only annotates byte ranges.
    //
    // Any malformed structure falls back to the generic decoder — the
    // inspector must be permissive.
    auto fail = [&]() { return decode_generic(m); };

    const uint32_t manifest_off = static_cast<uint32_t>(m.read_le(0, 4));
    if (manifest_off == 0 || manifest_off + 4 > m.size()) return fail();
    const int32_t num = static_cast<int32_t>(m.read_le(manifest_off, 4));
    if (num < 0 || num > 4096) return fail();

    std::size_t p = manifest_off + 4;
    std::size_t count = 0;
    while (count < static_cast<std::size_t>(num)) {
        if (p + 1 > m.size()) return fail();
        const uint8_t name_len = m.byte_at(p++);
        if (name_len == 0 || p + name_len + 8 > m.size()) return fail();
        std::string name;
        name.reserve(name_len);
        for (int i = 0; i < name_len; ++i) {
            name.push_back(static_cast<char>(m.byte_at(p++)));
        }
        if (p + 8 > m.size()) return fail();
        const uint32_t off = static_cast<uint32_t>(m.read_le(p, 4));
        p += 4;
        const uint32_t size = static_cast<uint32_t>(m.read_le(p, 4));
        p += 4;
        if (off > m.size() || off + size > m.size()) return fail();

        // Extension without the dot (mirrors CamArchive SubFile::ext()).
        const auto dot = name.find_last_of('.');
        const std::string ext =
            (dot == std::string::npos) ? "" : name.substr(dot + 1);

        Annotation a;
        a.range = {static_cast<std::size_t>(off), static_cast<std::size_t>(size)};
        a.label = "subfile: " + name;
        std::ostringstream ss;
        ss << size << " bytes";
        if (size >= 4) {
            ss << "  magic=" << hex_byte(m.byte_at(off))
               << hex_byte(m.byte_at(off + 1))
               << hex_byte(m.byte_at(off + 2))
               << hex_byte(m.byte_at(off + 3));
        }
        a.value = ss.str();
        a.description = "Sub-file '" + name + "' inside the .cam container. "
                        "Extension '." + ext + "' identifies its type.";
        a.category = "field";
        out.push_back(std::move(a));
        ++count;
    }

    // Annotate the manifest directory itself.
    Annotation manifest;
    manifest.range = {manifest_off, m.size() - manifest_off};
    manifest.label = "manifest_directory";
    manifest.value = std::to_string(count) + " sub-files";
    manifest.description = "Trailing directory listing every sub-file's name, "
                           "offset, and size (uint8 name_len, char[name_len] "
                           "name, int32 offset, int32 size per entry).";
    manifest.category = "header";
    out.push_back(std::move(manifest));

    return out;
}

// ---------------------------------------------------------------------------
// .cmp header decoder
// ---------------------------------------------------------------------------

std::vector<Annotation> decode_cmp_header(const HexModel& m) {
    std::vector<Annotation> out;
    if (m.size() < 8) return out;

    // [0..3] int32 reserved_skip — always 0 in practice
    out.push_back({
        {0, 4},
        "reserved_skip",
        std::to_string(static_cast<int32_t>(m.read_le(0, 4))),
        "Reserved field, always 0 in shipping .cmp files. Skipped by the loader.",
        "header"
    });

    // [4..7] int32 decompressed_size
    const int32_t decomp_size = static_cast<int32_t>(m.read_le(4, 4));
    out.push_back({
        {4, 4},
        "decompressed_size",
        std::to_string(decomp_size) + " bytes",
        "Size of the LZSS-decompressed payload. The compressed payload starts "
        "at offset 8 and runs to end of file.",
        "header"
    });

    // The rest is LZSS-compressed — annotate as a single "compressed payload" block.
    if (m.size() > 8) {
        out.push_back({
            {8, m.size() - 8},
            "lzss_payload",
            std::to_string(m.size() - 8) + " bytes (compressed)",
            "LZSS-compressed campaign metadata. Decompressed by f4-world-convert's "
            "lzss_expand() to reveal the team_name[8][20], team_motto[8][200], "
            "current_time, TE block, and other campaign fields.",
            "unknown"
        });
    }

    return out;
}

// ---------------------------------------------------------------------------
// THEATER.MAP header decoder
// ---------------------------------------------------------------------------

std::vector<Annotation> decode_theater_map(const HexModel& m) {
    std::vector<Annotation> out;
    if (m.size() < 16) return out;

    // [0..3] uint32 magic — 0x444CFFAE
    const uint32_t magic = static_cast<uint32_t>(m.read_le(0, 4));
    out.push_back({
        {0, 4},
        "magic",
        hex_str(magic, 8),
        "Theater header magic. Expected 0x444CFFAE (\"LD\" + magic bytes).",
        "header"
    });

    // [4..7] uint32 width
    const uint32_t w = static_cast<uint32_t>(m.read_le(4, 4));
    out.push_back({
        {4, 4},
        "width",
        std::to_string(w),
        "Grid columns (e.g. 128 for Korea).",
        "header"
    });

    // [8..11] uint32 height
    const uint32_t h = static_cast<uint32_t>(m.read_le(8, 4));
    out.push_back({
        {8, 4},
        "height",
        std::to_string(h),
        "Grid rows (e.g. 128 for Korea).",
        "header"
    });

    // [12..15] uint32 ft_to_cell
    const uint32_t ftc = static_cast<uint32_t>(m.read_le(12, 4));
    out.push_back({
        {12, 4},
        "ft_to_mea_cell",
        std::to_string(ftc),
        "Feet-to-cell conversion factor for elevation grid spacing.",
        "header"
    });

    // [16..] RGBA palette — 271 entries × 4 bytes = 1084 bytes
    // Annotate just the first few entries as samples.
    const std::size_t palette_start = 16;
    const std::size_t palette_entry_size = 4;
    const std::size_t max_palette_entries =
        (m.size() > palette_start)
            ? (m.size() - palette_start) / palette_entry_size : 0;
    const std::size_t sample_count = std::min<std::size_t>(max_palette_entries, 8);
    for (std::size_t i = 0; i < sample_count; ++i) {
        const std::size_t off = palette_start + i * palette_entry_size;
        if (off + 4 > m.size()) break;
        const uint8_t r = m.byte_at(off);
        const uint8_t g = m.byte_at(off + 1);
        const uint8_t b = m.byte_at(off + 2);
        const uint8_t a = m.byte_at(off + 3);
        std::ostringstream ss;
        ss << "RGBA(" << static_cast<int>(r) << "," << static_cast<int>(g)
           << "," << static_cast<int>(b) << "," << static_cast<int>(a) << ")";
        out.push_back({
            {off, 4},
            "palette[" + std::to_string(i) + "]",
            ss.str(),
            "Terrain type palette entry. Index 0 = water, indices 1..N = land types.",
            "field"
        });
    }
    if (max_palette_entries > sample_count) {
        const std::size_t remaining = max_palette_entries - sample_count;
        const std::size_t off = palette_start + sample_count * palette_entry_size;
        out.push_back({
            {off, remaining * palette_entry_size},
            "palette[" + std::to_string(sample_count) + ".." +
                std::to_string(max_palette_entries - 1) + "]",
            std::to_string(remaining) + " more entries",
            "Remaining palette entries. Total: " + std::to_string(max_palette_entries) + ".",
            "field"
        });
    }

    return out;
}

// ---------------------------------------------------------------------------
// FALCON4.ct class table decoder
// ---------------------------------------------------------------------------

std::vector<Annotation> decode_falcon4_ct(const HexModel& m) {
    std::vector<Annotation> out;
    if (m.size() < 2) return out;

    // [0..1] int16 num_entities
    const int16_t num = static_cast<int16_t>(m.read_le(0, 2));
    out.push_back({
        {0, 2},
        "num_entities",
        std::to_string(num),
        "Number of class table entries that follow. Each entry is 81 bytes.",
        "header"
    });

    // Annotate the first few entries' classInfo_ fields (offset 8 within entry).
    constexpr std::size_t ENTRY_SIZE = 81;
    constexpr std::size_t CLASSINFO_OFFSET = 8;
    const std::size_t sample_count = std::min<std::size_t>(
        std::max<int>(0, num), 16);
    for (std::size_t i = 0; i < sample_count; ++i) {
        const std::size_t entry_off = 2 + i * ENTRY_SIZE;
        if (entry_off + ENTRY_SIZE > m.size()) break;

        // classInfo_[8] at offset 8: [domain, class, type, stype, ...]
        const std::size_t ci_off = entry_off + CLASSINFO_OFFSET;
        const uint8_t domain = m.byte_at(ci_off);
        const uint8_t cls    = m.byte_at(ci_off + 1);
        const uint8_t type   = m.byte_at(ci_off + 2);
        const uint8_t stype  = m.byte_at(ci_off + 3);

        // Decode the four classInfo bytes to human-readable names so the
        // user doesn't have to keep the classtbl.h constants in their head.
        // The domain+stype pair resolves to a unit-subtype name (e.g.
        // "Armor" / "Fighter" / "Carrier") via f4-world-types's lookup.
        std::ostringstream ss;
        ss << "domain=" << static_cast<int>(domain)
           << " (" << f4::viewer::domain_name(domain) << ")  "
           << "class=" << static_cast<int>(cls)
           << " (" << f4::viewer::vu_class_name(cls) << ")  "
           << "type=" << static_cast<int>(type) << "  "
           << "stype=" << static_cast<int>(stype);
        // For unit entries (class == CLASS_UNIT == 6), also resolve the
        // subtype name. For objective entries (class == 4), `type` is the
        // ObjectiveType enum (1-39) — we don't decode it here because
        // the ObjectiveType names live in objective_decoder.hpp, which
        // would be a heavier include. The user can look up `type` in
        // the inspector's "Type:" line if they want the name.
        if (cls == 6 /* CLASS_UNIT */ && stype > 0) {
            ss << " (" << f4::world_types::unit_subtype_name(domain, stype) << ")";
        }
        out.push_back({
            {entry_off, ENTRY_SIZE},
            "entry[" + std::to_string(i) + "] (entity_type " +
                std::to_string(100 + i) + ")",
            ss.str(),
            "ClassTableEntry. entity_type values start at 100, so this entry "
            "is referenced as entity_type=" + std::to_string(100 + i) + ". "
            "classInfo_[0..3] = (domain, class, type, stype).",
            "field"
        });

        // Annotate the trailing dataType byte (offset 76 within entry) and
        // the dataPtr index (offset 77, 4 bytes LE). These are the keys
        // for navigating from a class-table entry to its corresponding
        // OCD/UCD/VCD/FCD record — without them, the user can't tell
        // where to look up the per-class metadata.
        if (entry_off + 81 <= m.size()) {
            const std::size_t dt_off = entry_off + 76;
            const std::size_t dp_off = entry_off + 77;
            const uint8_t dt = m.byte_at(dt_off);
            const uint32_t dp = static_cast<uint32_t>(m.read_le(dp_off, 4));
            out.push_back({
                {dt_off, 1},
                "entry[" + std::to_string(i) + "].dataType",
                std::to_string(static_cast<int>(dt)) + " (" +
                    f4::viewer::data_type_name(dt) + ")",
                "DataType enum: tells which theater-data table dataPtr "
                "indexes into (1=FCD, 3=OCD, 4=UCD, 5=VCD, 6=WCD, 7=SSD).",
                "field"
            });
            out.push_back({
                {dp_off, 4},
                "entry[" + std::to_string(i) + "].dataPtr",
                std::to_string(dp),
                "Index into the " + std::string(f4::viewer::data_type_name(dt)) +
                    " table.",
                "field"
            });
        }
    }
    if (num > static_cast<int>(sample_count)) {
        const std::size_t remaining = num - sample_count;
        const std::size_t off = 2 + sample_count * ENTRY_SIZE;
        out.push_back({
            {off, remaining * ENTRY_SIZE},
            "entry[" + std::to_string(sample_count) + ".." +
                std::to_string(num - 1) + "]",
            std::to_string(remaining) + " more entries",
            "Remaining class table entries (not annotated individually to keep "
            "the annotation list readable).",
            "field"
        });
    }

    return out;
}

// ---------------------------------------------------------------------------
// Generic decoder (fallback for unknown / text / binary files)
// ---------------------------------------------------------------------------

std::vector<Annotation> decode_generic(const HexModel& m) {
    std::vector<Annotation> out;
    if (!m.loaded()) return out;

    // File size annotation covering the whole file.
    out.push_back({
        {0, m.size()},
        "file_size",
        std::to_string(m.size()) + " bytes",
        "Total file size.",
        "header"
    });

    // Magic bytes — first 16 bytes as hex + ASCII preview.
    const std::size_t magic_len = std::min<std::size_t>(m.size(), 16);
    const auto magic_bytes = m.slice(0, magic_len);
    std::ostringstream magic_val;
    magic_val << hex_dump(magic_bytes.data(), magic_len);
    magic_val << "  (\"" << ascii_preview(magic_bytes.data(), magic_len) << "\")";
    out.push_back({
        {0, magic_len},
        "magic",
        magic_val.str(),
        "First " + std::to_string(magic_len) + " bytes of the file. "
        "Used by identify_file() to detect format when the extension is unknown.",
        "header"
    });

    // Entropy annotation (not tied to a byte range — use the whole file).
    const double h = m.entropy();
    std::string entropy_desc;
    if (h < 1.0)      entropy_desc = "very low — likely all-zero or single-byte";
    else if (h < 3.0) entropy_desc = "low — likely structured text or sparse data";
    else if (h < 6.0) entropy_desc = "medium — typical of structured binary";
    else if (h < 7.5) entropy_desc = "high — likely compressed or encrypted";
    else              entropy_desc = "very high — likely encrypted or random";
    out.push_back({
        {0, m.size()},
        "entropy",
        std::to_string(h) + " bits/byte",
        "Shannon entropy over the whole file. " + entropy_desc + ".",
        "field"
    });

    // ASCII string runs >= 4 chars — surface the first 50 to avoid
    // swamping the annotation list on text-heavy files.
    const auto strings = m.find_ascii_strings(4);
    const std::size_t max_strings = 50;
    for (std::size_t i = 0; i < std::min(strings.size(), max_strings); ++i) {
        const auto& r = strings[i];
        const auto bytes = m.slice(r.offset, std::min<std::size_t>(r.length, 80));
        std::string s(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        // Truncate long strings in the value column.
        if (s.size() > 60) {
            s = s.substr(0, 57) + "...";
        }
        out.push_back({
            r,
            "string[" + std::to_string(i) + "]",
            "\"" + s + "\"",
            "ASCII string run, " + std::to_string(r.length) + " bytes.",
            "string"
        });
    }
    if (strings.size() > max_strings) {
        out.push_back({
            {0, 0},  // no specific range — this is a meta-annotation
            "strings_omitted",
            std::to_string(strings.size() - max_strings) + " more strings",
            "Additional ASCII string runs not shown. Use the find_ascii_strings() "
            "API to enumerate all of them programmatically.",
            "field"
        });
    }

    return out;
}

} // namespace f4::viewer
