// f4-world-convert/src/theater_data.cpp
//
// Implementation of the Falcon4.OCD/PHD/PD/UCD/VCD/FED/FCD parsers.
// See theater_data.hpp for the file-format documentation and struct sizes.

#include <f4/world_convert/theater_data.hpp>

#include <f4/io/cursor.hpp>
#include <f4/io/read_file.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <stdexcept>
#include <string>

namespace f4::world_convert {

namespace {

// ============================================================================
// I/O helpers — thin wrappers over the shared f4::io primitives.
//
// The local read_file delegate keeps the "theater_data:" diagnostic prefix
// that callers (and TheaterObjectDatabase::load_all) rely on when labelling
// per-file failures. The Cursor is the shared sticky-flag reader; the
// bounds-check is now a post-parse `if (c.error) throw ...` at the end of
// each load_X function below.
// ============================================================================

std::vector<uint8_t> read_file(const std::filesystem::path& path) {
    return f4::io::read_file(path, "theater_data");
}

using f4::io::Cursor;

// ============================================================================
// FF-DB Control: read entries count, handling the trailing-short fallback.
// ============================================================================

int16_t read_entry_count(const std::vector<uint8_t>& buf, std::size_t record_size) {
    if (buf.size() < 2) throw std::runtime_error("theater_data: file too small for header");
    int16_t entries;
    std::memcpy(&entries, buf.data(), 2);

    // FF-DB Control: if header says 0, the real count is in the last 2 bytes.
    if (entries == 0) {
        if (buf.size() < 4) throw std::runtime_error("theater_data: FF-DB file too small");
        std::memcpy(&entries, buf.data() + buf.size() - 2, 2);
        if (entries <= 0) {
            throw std::runtime_error("theater_data: FF-DB Control: trailing count also 0");
        }
        // Sanity check: file should be at least 2 + entries * record_size + 2
        const std::size_t expected = 2 + static_cast<std::size_t>(entries) * record_size + 2;
        if (buf.size() < expected) {
            throw std::runtime_error("theater_data: FF-DB Control: file too small for "
                                     + std::to_string(entries) + " entries");
        }
        return entries;
    }

    if (entries < 0) throw std::runtime_error("theater_data: negative entry count");

    // Standard format: file size must equal 2 + entries * record_size.
    const std::size_t expected = 2 + static_cast<std::size_t>(entries) * record_size;
    if (buf.size() < expected) {
        throw std::runtime_error("theater_data: file too small for "
                                 + std::to_string(entries) + " entries (expected "
                                 + std::to_string(expected) + ", got "
                                 + std::to_string(buf.size()) + ")");
    }
    return entries;
}

// ============================================================================
// char-array → std::string helper. Trims trailing NULs.
// ============================================================================

std::string trim_name(const uint8_t* src, std::size_t len) {
    std::size_t actual = 0;
    while (actual < len && src[actual] != 0) ++actual;
    return std::string(reinterpret_cast<const char*>(src), actual);
}

} // namespace

// ============================================================================
// Helpers — point/movement type names
// ============================================================================

const char* point_type_name(uint8_t pt) noexcept {
    switch (pt) {
        case PT_RUNWAY:        return "Runway";
        case PT_TAKEOFF:       return "Takeoff";
        case PT_TAXI:          return "Taxi";
        case PT_SAM:           return "SAM";
        case PT_ARTILLERY:     return "Artillery";
        case PT_AAA:           return "AAA";
        case PT_RADAR:         return "Radar";
        case PT_RUNWAY_DIM:    return "Runway Dim";
        case PT_SUPPORT:       return "Support";
        case PT_STATIC_RADAR:  return "Static Radar";
        case PT_SMALL_PARK:    return "Small Park";
        case PT_LARGE_PARK:    return "Large Park";
        case PT_SMALL_DOCK:    return "Small Dock";
        case PT_LARGE_DOCK:    return "Large Dock";
        case PT_TAKE_RUNWAY:   return "Take Runway";
        case PT_HELICOPTER:    return "Helicopter";
        case PT_FOLLOW_ME:     return "Follow Me";
        case PT_TRACK:         return "Track";
        case PT_CRIT_TAXI:     return "Crit Taxi";
        default:               return "Unknown";
    }
}

const char* point_list_type_name(uint8_t plt) noexcept {
    switch (plt) {
        case PLT_RUNWAY:        return "Runway";
        case PLT_SAM:           return "SAM";
        case PLT_ARTILLERY:     return "Artillery";
        case PLT_AAA:           return "AAA";
        case PLT_RUNWAY_DIM:    return "Runway Dim";
        case PLT_STATIC_RADAR:  return "Static Radar";
        case PLT_PARK:          return "Parking";
        case PLT_RUNWAY_LT:     return "Runway Left";
        case PLT_RUNWAY_RT:     return "Runway Right";
        case PLT_HELICOPTER:    return "Helicopter";
        case PLT_FOLLOW_ME:     return "Follow Me";
        case PLT_DOCK:          return "Dock";
        case PLT_TRACK:         return "Track";
        default:                return "Unknown";
    }
}

const char* movement_type_name(int32_t mt) noexcept {
    switch (mt) {
        case 0: return "NoMove";
        case 1: return "Foot";
        case 2: return "Wheeled";
        case 3: return "Tracked";
        case 4: return "LowAir";
        case 5: return "Air";
        case 6: return "Naval";
        case 7: return "Rail";
        default: return "Unknown";
    }
}

// ============================================================================
// File finders — case-insensitive fallback for cross-platform
// ============================================================================

std::filesystem::path
find_theater_file(const std::filesystem::path& base_path,
                  const std::string& ext) {
    // 1. base_path + "." + ext
    {
        auto p = base_path;
        p += ".";
        p += ext;
        if (std::filesystem::exists(p)) return p;
    }
    // 2. base_path verbatim
    if (std::filesystem::exists(base_path)) return base_path;

    // 3. Case-insensitive search in base_path's parent directory.
    //    Look for any file matching "<stem>.<ext>" case-insensitively.
    const auto parent = base_path.parent_path().empty()
        ? std::filesystem::current_path()
        : base_path.parent_path();
    const auto stem = base_path.filename().string();
    if (stem.empty()) return {};
    if (parent.empty() || !std::filesystem::exists(parent)) return {};

    std::string stem_lower = stem;
    std::string ext_lower = ext;
    std::transform(stem_lower.begin(), stem_lower.end(), stem_lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    std::transform(ext_lower.begin(), ext_lower.end(), ext_lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    for (const auto& entry : std::filesystem::directory_iterator(parent)) {
        if (!entry.is_regular_file()) continue;
        auto name = entry.path().filename().string();
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (name == stem_lower + "." + ext_lower) {
            return entry.path();
        }
    }
    return {};
}

// ============================================================================
// Per-file loaders
// ============================================================================

void load_objective_data(const std::filesystem::path& base_path,
                         ObjectiveClassTable& out) {
    const auto path = find_theater_file(base_path, "OCD");
    if (path.empty()) throw std::runtime_error("theater_data: Falcon4.OCD not found");
    const auto buf = read_file(path);
    const int16_t n = read_entry_count(buf, OCD_RECORD_SIZE);

    Cursor c(buf);
    c.p = buf.data() + 2;  // skip the 2-byte count

    out.entries.clear();
    out.entries.reserve(static_cast<std::size_t>(n));
    for (int16_t i = 0; i < n; ++i) {
        ObjectiveClassData e;
        e.index         = c.s16();
        uint8_t name_buf[20];
        c.read_bytes(name_buf, 20);
        e.name          = trim_name(name_buf, 20);
        e.data_rate     = c.s16();
        e.deag_distance = c.s16();
        e.pt_data_index = c.s16();
        c.read_bytes(e.detection.data(), TD_MOVEMENT_TYPES);
        c.read_bytes(e.damage_mod.data(), TD_OTHER_DAM + 1);
        // 1 byte of padding after DamageMod[11] to align IconIndex to offset 48
        // within the entry (struct uses default 2-byte alignment; DamageMod
        // ends at offset 47 → next 2-byte-aligned slot is 48).
        c.p += 1;
        e.icon_index    = c.s16();
        e.features      = c.u8();
        e.radar_feature = c.u8();
        e.first_feature = c.s16();
        out.entries.push_back(std::move(e));
    }
    if (c.error) throw std::runtime_error("theater_data: unexpected end of file");
}

void load_pt_header_data(const std::filesystem::path& base_path,
                         PtHeaderTable& out) {
    const auto path = find_theater_file(base_path, "PHD");
    if (path.empty()) throw std::runtime_error("theater_data: Falcon4.PHD not found");
    const auto buf = read_file(path);
    const int16_t n = read_entry_count(buf, PHD_RECORD_SIZE);

    Cursor c(buf);
    c.p = buf.data() + 2;

    out.entries.clear();
    out.entries.reserve(static_cast<std::size_t>(n));
    for (int16_t i = 0; i < n; ++i) {
        // MSVC layout (default 8-byte alignment):
        //   off 0: objID       (short, 2)
        //   off 2: type        (uchar, 1)
        //   off 3: count       (uchar, 1)
        //   off 4: features[5] (uchar[5], 5)
        //   off 9: *1 byte pad* (for 2-byte align of `data`)
        //   off10: data        (short, 2)
        //   off12: sinHeading  (float, 4)
        //   off16: cosHeading  (float, 4)
        //   off20: first       (short, 2)
        //   off22: texIdx      (short, 2)
        //   off24: runwayNum   (char, 1)
        //   off25: ltrt        (char, 1)
        //   off26: nextHeader  (short, 2)
        //   total = 28 bytes (no trailing pad; 28 is already a multiple of 4)
        PtHeaderData e;
        e.obj_id     = c.s16();
        e.type       = c.u8();
        e.count      = c.u8();
        c.read_bytes(e.features.data(), TD_MAX_FEAT_DEPEND);  // 5 bytes, cursor at 9
        c.p += 1;                                                          // skip pad, cursor at 10
        e.data        = c.s16();
        e.sin_heading = c.f32();
        e.cos_heading = c.f32();
        e.first       = c.s16();
        e.tex_idx     = c.s16();
        e.runway_num  = c.s8();
        e.ltrt        = c.s8();
        e.next_header = c.s16();
        // cursor at 28, no trailing pad needed
        out.entries.push_back(std::move(e));
    }
    if (c.error) throw std::runtime_error("theater_data: unexpected end of file");
}

void load_pt_data(const std::filesystem::path& base_path,
                  PtDataTable& out) {
    const auto path = find_theater_file(base_path, "PD");
    if (path.empty()) throw std::runtime_error("theater_data: Falcon4.PD not found");
    const auto buf = read_file(path);
    const int16_t n = read_entry_count(buf, PD_RECORD_SIZE);

    Cursor c(buf);
    c.p = buf.data() + 2;

    out.entries.clear();
    out.entries.reserve(static_cast<std::size_t>(n));
    for (int16_t i = 0; i < n; ++i) {
        PtData e;
        e.x_offset = c.f32();
        e.y_offset = c.f32();
        e.type     = c.u8();
        e.flags    = c.u8();
        // The struct's on-disk size is 12 bytes (2 bytes of trailing padding
        // for 4-byte float alignment). Skip the padding.
        c.p += 2;
        out.entries.push_back(std::move(e));
    }
    if (c.error) throw std::runtime_error("theater_data: unexpected end of file");
}

void load_unit_data(const std::filesystem::path& base_path,
                    UnitClassTable& out) {
    const auto path = find_theater_file(base_path, "UCD");
    if (path.empty()) throw std::runtime_error("theater_data: Falcon4.UCD not found");
    const auto buf = read_file(path);
    const int16_t n = read_entry_count(buf, UCD_RECORD_SIZE);

    Cursor c(buf);
    c.p = buf.data() + 2;

    out.entries.clear();
    out.entries.reserve(static_cast<std::size_t>(n));
    for (int16_t i = 0; i < n; ++i) {
        // MSVC layout (default 8-byte alignment). Verified against real
        // Falcon4.UCD bytes — see scripts/parse_snapshot.py.
        //   off   0: index             (short, 2)
        //   off   2: *2 bytes pad*     (for 4-byte align of NumElements)
        //   off   4: num_elements[16]  (int[16], 64)
        //   off  68: vehicle_type[16]  (short[16], 32)
        //   off 100: vehicle_class[16][8] (uchar[128], 128)
        //   off 228: flags             (ushort, 2)
        //   off 230: name[20]          (char[20], 20)
        //   off 250: *2 bytes pad*     (for 4-byte align of MovementType)
        //   off 252: movement_type     (int, 4)
        //   off 256: movement_speed    (short, 2)
        //   off 258: max_range         (short, 2)
        //   off 260: fuel              (long, 4)
        //   off 264: rate              (short, 2)
        //   off 266: pt_data_index     (short, 2)
        //   off 268: scores[16]        (uchar[16], 16)
        //   off 284: role              (uchar, 1)
        //   off 285: hit_chance[8]     (uchar[8], 8)
        //   off 293: strength[8]       (uchar[8], 8)
        //   off 301: range[8]          (uchar[8], 8)
        //   off 309: detection[8]      (uchar[8], 8)
        //   off 317: damage_mod[11]    (uchar[11], 11)
        //   off 328: radar_vehicle     (uchar, 1)
        //   off 329: *1 byte pad*      (for 2-byte align of SpecialIndex)
        //   off 330: special_index     (short, 2)
        //   off 332: icon_index        (short, 2)
        //   off 334: *2 bytes trailing pad* (struct size must be multiple of 4)
        //   total = 336 bytes
        UnitClassData e;
        e.index = c.s16();
        c.p += 2;  // skip 2 bytes pad
        for (int j = 0; j < TD_VEHICLE_GROUPS_PER_UNIT; ++j) e.num_elements[j] = c.s32();
        for (int j = 0; j < TD_VEHICLE_GROUPS_PER_UNIT; ++j) e.vehicle_type[j] = c.s16();
        for (int j = 0; j < TD_VEHICLE_GROUPS_PER_UNIT; ++j)
            c.read_bytes(e.vehicle_class[j].data(), 8);
        e.flags = c.u16();
        uint8_t name_buf[20];
        c.read_bytes(name_buf, 20);
        e.name = trim_name(name_buf, 20);
        c.p += 2;  // skip 2 bytes pad before movement_type (4-byte align)
        e.movement_type = c.s32();
        e.movement_speed = c.s16();
        e.max_range      = c.s16();
        e.fuel           = c.s32();
        e.rate           = c.s16();
        e.pt_data_index  = c.s16();
        c.read_bytes(e.scores.data(), TD_MAXIMUM_ROLES);
        e.role           = c.u8();
        c.read_bytes(e.hit_chance.data(), TD_MOVEMENT_TYPES);
        c.read_bytes(e.strength.data(), TD_MOVEMENT_TYPES);
        c.read_bytes(e.range.data(), TD_MOVEMENT_TYPES);
        c.read_bytes(e.detection.data(), TD_MOVEMENT_TYPES);
        c.read_bytes(e.damage_mod.data(), TD_OTHER_DAM + 1);
        e.radar_vehicle  = c.u8();
        c.p += 1;  // skip 1 byte pad before special_index (2-byte align)
        e.special_index  = c.s16();
        e.icon_index     = c.s16();
        c.p += 2;  // skip 2 bytes trailing pad (struct size to multiple of 4)
        out.entries.push_back(std::move(e));
    }
    if (c.error) throw std::runtime_error("theater_data: unexpected end of file");
}

void load_vehicle_data(const std::filesystem::path& base_path,
                       VehicleClassTable& out) {
    const auto path = find_theater_file(base_path, "VCD");
    if (path.empty()) throw std::runtime_error("theater_data: Falcon4.VCD not found");
    const auto buf = read_file(path);
    const int16_t n = read_entry_count(buf, VCD_RECORD_SIZE);

    Cursor c(buf);
    c.p = buf.data() + 2;

    out.entries.clear();
    out.entries.reserve(static_cast<std::size_t>(n));
    for (int16_t i = 0; i < n; ++i) {
        // MSVC layout (default 8-byte alignment). NO mid-struct padding —
        // every field is naturally aligned at its current offset.
        // Total field bytes = 157, trailing pad = 3 bytes → record size = 160.
        VehicleClassData e;
        e.index            = c.s16();
        e.hit_points       = c.s16();
        e.flags            = c.u32();
        uint8_t name_buf[15];
        c.read_bytes(name_buf, 15);
        e.name = trim_name(name_buf, 15);
        uint8_t nctr_buf[5];
        c.read_bytes(nctr_buf, 5);
        e.nctr = trim_name(nctr_buf, 5);
        e.rcs_factor       = c.f32();
        e.max_wt           = c.s32();
        e.empty_wt         = c.s32();
        e.fuel_wt          = c.s32();
        e.fuel_econ        = c.s16();
        e.engine_sound     = c.s16();
        e.high_alt         = c.s16();
        e.low_alt          = c.s16();
        e.cruise_alt       = c.s16();
        e.max_speed        = c.s16();
        e.radar_type       = c.s16();
        e.number_of_pilots = c.s16();
        e.rack_flags       = c.u16();
        e.visible_flags    = c.u16();
        e.callsign_index   = c.u8();
        e.callsign_slots   = c.u8();
        c.read_bytes(e.hit_chance.data(), TD_MOVEMENT_TYPES);
        c.read_bytes(e.strength.data(), TD_MOVEMENT_TYPES);
        c.read_bytes(e.range.data(), TD_MOVEMENT_TYPES);
        c.read_bytes(e.detection.data(), TD_MOVEMENT_TYPES);
        for (int j = 0; j < TD_HARDPOINT_MAX; ++j) e.weapon[j]  = c.s16();
        for (int j = 0; j < TD_HARDPOINT_MAX; ++j) e.weapons[j] = c.u8();
        c.read_bytes(e.damage_mod.data(), TD_OTHER_DAM + 1);
        // cursor at 157; skip 3 bytes of trailing pad to reach 160
        c.p += 3;
        out.entries.push_back(std::move(e));
    }
    if (c.error) throw std::runtime_error("theater_data: unexpected end of file");
}

void load_feature_data(const std::filesystem::path& base_path,
                       FeatureClassTable& out) {
    const auto path = find_theater_file(base_path, "FCD");
    if (path.empty()) throw std::runtime_error("theater_data: Falcon4.FCD not found");
    const auto buf = read_file(path);
    const int16_t n = read_entry_count(buf, FCD_RECORD_SIZE);

    Cursor c(buf);
    c.p = buf.data() + 2;

    out.entries.clear();
    out.entries.reserve(static_cast<std::size_t>(n));
    for (int16_t i = 0; i < n; ++i) {
        // MSVC layout (default 8-byte alignment):
        //   off 0: index        (short, 2)
        //   off 2: repair_time  (short, 2)
        //   off 4: priority     (uchar, 1)
        //   off 5: *1 byte pad* (for 2-byte align of Flags)
        //   off 6: flags        (ushort, 2)
        //   off 8: name[20]     (char[20], 20)
        //   off28: hit_points   (short, 2)
        //   off30: height       (short, 2)
        //   off32: angle        (float, 4) — 4-byte aligned ✓
        //   off36: radar_type   (short, 2)
        //   off38: detection[8] (uchar[8], 8)
        //   off46: damage_mod[11] (uchar[11], 11) → ends at 57
        //   off57: *3 bytes trailing pad* (struct size to multiple of 4)
        //   total = 60 bytes
        FeatureClassData e;
        e.index        = c.s16();
        e.repair_time  = c.s16();
        e.priority     = c.u8();
        c.p += 1;  // skip 1 byte pad before flags (2-byte align)
        e.flags        = c.u16();
        uint8_t name_buf[20];
        c.read_bytes(name_buf, 20);
        e.name = trim_name(name_buf, 20);
        e.hit_points   = c.s16();
        e.height       = c.s16();
        e.angle        = c.f32();
        e.radar_type   = c.s16();
        c.read_bytes(e.detection.data(), TD_MOVEMENT_TYPES);
        c.read_bytes(e.damage_mod.data(), TD_OTHER_DAM + 1);
        // cursor at 57; skip 3 bytes of trailing pad to reach 60
        c.p += 3;
        out.entries.push_back(std::move(e));
    }
    if (c.error) throw std::runtime_error("theater_data: unexpected end of file");
}

void load_feature_entry_data(const std::filesystem::path& base_path,
                             FeatureEntryTable& out) {
    const auto path = find_theater_file(base_path, "FED");
    if (path.empty()) throw std::runtime_error("theater_data: Falcon4.FED not found");
    const auto buf = read_file(path);
    const int16_t n = read_entry_count(buf, FED_RECORD_SIZE);

    Cursor c(buf);
    c.p = buf.data() + 2;

    out.entries.clear();
    out.entries.reserve(static_cast<std::size_t>(n));
    for (int16_t i = 0; i < n; ++i) {
        // MSVC layout (default 8-byte alignment). Verified against real
        // Falcon4.FED bytes — see scripts/parse_snapshot.py.
        //   off 0: index     (short, 2)
        //   off 2: flags     (ushort, 2)
        //   off 4: eClass[8] (uchar[8], 8)
        //   off12: value     (uchar, 1)
        //   off13: *3 bytes pad* (for 4-byte align of Offset which is a vector)
        //   off16: offset.x  (float, 4)
        //   off20: offset.y  (float, 4)
        //   off24: offset.z  (float, 4)
        //   off28: facing    (Int16, 2)
        //   off30: *2 bytes trailing pad* (struct size to multiple of 4)
        //   total = 32 bytes
        FeatureEntryData e;
        e.index    = c.s16();
        e.flags    = c.u16();
        c.read_bytes(e.e_class.data(), 8);
        e.value    = c.u8();
        c.p += 3;  // skip 3 bytes pad before Offset (4-byte align of vector)
        e.offset_x = c.f32();
        e.offset_y = c.f32();
        e.offset_z = c.f32();
        e.facing   = c.s16();
        c.p += 2;  // skip 2 bytes trailing pad
        out.entries.push_back(std::move(e));
    }
    if (c.error) throw std::runtime_error("theater_data: unexpected end of file");
}

void load_radar_data(const std::filesystem::path& base_path,
                     RadarClassTable& out) {
    const auto path = find_theater_file(base_path, "RCD");
    if (path.empty()) throw std::runtime_error("theater_data: Falcon4.RCD not found");
    const auto buf = read_file(path);
    const int16_t n = read_entry_count(buf, RCD_RECORD_SIZE);

    Cursor c(buf);
    c.p = buf.data() + 2;

    out.entries.clear();
    out.entries.reserve(static_cast<std::size_t>(n));
    for (int16_t i = 0; i < n; ++i) {
        // Best-known MSVC layout for RadarClassDataType (Phase 3 — partial
        // decode). The full FreeFalcon struct has more fields (RcsType,
        // per-band DetectionChance, radar flags, etc.) but for visualization
        // we only need Index + Name + Range. The remaining 26 bytes per
        // record are skipped as opaque padding; consumers that need other
        // fields can extend this struct and parser later.
        //
        //   off 0: Index    (short, 2)
        //   off 2: Name[28] (char[28], 28) — radar name
        //   off30: Range    (float, 4)     — detection range (km)
        //   off34: *(26 bytes opaque padding)*
        //   total = 60 bytes
        RadarClassData e;
        e.index = c.s16();
        uint8_t name_buf[28];
        c.read_bytes(name_buf, 28);
        e.name = trim_name(name_buf, 28);
        e.range_km = c.f32();
        // Skip the remaining 26 bytes of opaque fields (radar type flags,
        // per-band detection ratios, etc.).
        c.p += 26;
        out.entries.push_back(std::move(e));
    }
    if (c.error) throw std::runtime_error("theater_data: unexpected end of file");
}

// ============================================================================
// TheaterObjectDatabase — convenience bulk loader
// ============================================================================

namespace {

// Try one loader, recording the outcome in `diag`. The `ext` is used both
// to locate the file (via find_theater_file) and to label the diagnostic.
// Returns true iff the file was found AND parsed without throwing.
//
// We deliberately probe find_theater_file ourselves rather than letting the
// loader's internal "not found" throw propagate, so we can distinguish
// "file missing" (Status::Missing) from "file present but corrupt"
// (Status::ParseError). The previous implementation swallowed both cases
// under the same `catch (...) {}` and lost the diagnostic entirely.
template <class TableT>
void try_one(void (*fn)(const std::filesystem::path&, TableT&),
             const std::filesystem::path& base,
             const std::string& ext,
             TableT& tbl,
             TheaterFileLoadResult& diag) {
    diag.filename = std::string("Falcon4.") + ext;
    const auto path = find_theater_file(base, ext);
    if (path.empty()) {
        diag.status = TheaterFileLoadResult::Status::Missing;
        diag.message = "file not found in theater directory";
        return;
    }
    try {
        fn(base, tbl);
        diag.status = TheaterFileLoadResult::Status::Loaded;
        diag.record_count = tbl.entries.size();
        diag.message.clear();
    } catch (const std::exception& e) {
        diag.status = TheaterFileLoadResult::Status::ParseError;
        diag.record_count = 0;
        diag.message = e.what();
        // tbl may be partially populated if the loader threw mid-loop; clear
        // it so loaded() reflects reality and downstream code doesn't see
        // a half-decoded table.
        tbl.entries.clear();
    }
}

} // anonymous namespace

void TheaterObjectDatabase::load_all(const std::filesystem::path& dir) {
    const auto base = dir / "Falcon4";
    load_diagnostics.clear();
    load_diagnostics.reserve(8);

    // Inline each call rather than looping over a function-pointer table —
    // the eight loaders have eight distinct table types and a template is
    // cleaner than a void*-erasing wrapper.
    load_diagnostics.emplace_back();
    try_one(load_objective_data,     base, "OCD", objectives,      load_diagnostics.back());
    load_diagnostics.emplace_back();
    try_one(load_pt_header_data,     base, "PHD", pt_headers,      load_diagnostics.back());
    load_diagnostics.emplace_back();
    try_one(load_pt_data,            base, "PD",  pt_data,         load_diagnostics.back());
    load_diagnostics.emplace_back();
    try_one(load_unit_data,          base, "UCD", units,           load_diagnostics.back());
    load_diagnostics.emplace_back();
    try_one(load_vehicle_data,       base, "VCD", vehicles,        load_diagnostics.back());
    load_diagnostics.emplace_back();
    try_one(load_feature_data,       base, "FCD", features,        load_diagnostics.back());
    load_diagnostics.emplace_back();
    try_one(load_feature_entry_data, base, "FED", feature_entries, load_diagnostics.back());
    load_diagnostics.emplace_back();
    try_one(load_radar_data,         base, "RCD", radars,          load_diagnostics.back());
}

} // namespace f4::world_convert
