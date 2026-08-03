// f4-world-viewer/src/hex/decoders.cpp

#include <f4/viewer/decoders.hpp>

#include <f4/convert/cam_archive.hpp>

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

/// Format an integer as hex with 0x prefix. width=0 means no padding.
std::string hex_str(uint64_t v, int width = 0) {
    std::ostringstream ss;
    ss << "0x" << std::hex;
    if (width > 0) ss << std::setfill('0') << std::setw(width);
    ss << v;
    return ss.str();
}

/// Format a byte as two hex digits.
std::string hex_byte(uint8_t b) {
    static const char* digits = "0123456789ABCDEF";
    std::string s(2, '0');
    s[0] = digits[b >> 4];
    s[1] = digits[b & 0x0F];
    return s;
}

/// Format a byte slice as a hex string (space-separated).
std::string hex_dump(const uint8_t* data, std::size_t n) {
    std::string s;
    s.reserve(n * 3);
    for (std::size_t i = 0; i < n; ++i) {
        if (i > 0) s += ' ';
        s += hex_byte(data[i]);
    }
    return s;
}

/// Format a byte slice as an ASCII preview (non-printable → '.').
std::string ascii_preview(const uint8_t* data, std::size_t n) {
    std::string s;
    s.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const uint8_t b = data[i];
        s.push_back((b >= 0x20 && b < 0x7F) ? static_cast<char>(b) : '.');
    }
    return s;
}

} // namespace

// ---------------------------------------------------------------------------
// .cam container manifest decoder
// ---------------------------------------------------------------------------

std::vector<Annotation> decode_cam_manifest(const HexModel& m) {
    std::vector<Annotation> out;
    if (!m.loaded()) return out;

    // Delegate to f4-world-convert's CamArchive for the actual parsing.
    // We catch any exception — the decoder must be permissive.
    f4::convert::CamArchive cam;
    try {
        cam.load(m.path());
    } catch (const std::exception&) {
        // Not a valid .cam — fall back to generic.
        return decode_generic(m);
    }

    // Annotate the manifest offset (first 4 bytes).
    const uint32_t manifest_off = static_cast<uint32_t>(m.read_le(0, 4));
    out.push_back({
        {0, 4},
        "manifest_offset",
        std::to_string(manifest_off),
        "Byte offset of the sub-file directory within this .cam file.",
        "header"
    });

    // Annotate each sub-file's byte range.
    for (const auto& sf : cam.subfiles()) {
        Annotation a;
        a.range = {static_cast<std::size_t>(sf.offset),
                   static_cast<std::size_t>(sf.size)};
        a.label = "subfile: " + sf.name;
        std::ostringstream ss;
        ss << sf.size << " bytes";
        if (sf.data.size() >= 4) {
            ss << "  magic=" << hex_byte(sf.data[0]) << hex_byte(sf.data[1])
               << hex_byte(sf.data[2]) << hex_byte(sf.data[3]);
        }
        a.value = ss.str();
        a.description = "Sub-file '" + sf.name + "' inside the .cam container. "
                        "Extension '." + sf.ext() + "' identifies its type.";
        a.category = "field";
        out.push_back(std::move(a));
    }

    // Annotate the manifest directory itself.
    Annotation manifest;
    manifest.range = {manifest_off, m.size() - manifest_off};
    manifest.label = "manifest_directory";
    manifest.value = std::to_string(cam.subfiles().size()) + " sub-files";
    manifest.description = "Trailing directory listing every sub-file's name, "
                           "offset, and size. Read by CamArchive::load().";
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

        std::ostringstream ss;
        ss << "domain=" << static_cast<int>(domain)
           << " class=" << static_cast<int>(cls)
           << " type=" << static_cast<int>(type)
           << " stype=" << static_cast<int>(stype);
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
