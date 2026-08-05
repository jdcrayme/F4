// f4-world-convert/include/f4/world_convert/theater_data.hpp
//
// Parsers for FreeFalcon's static per-theater "object database" files:
//
//   Falcon4.OCD  — ObjClassDataType[]    (per-objective-class metadata + names)
//   Falcon4.PHD  — PtHeaderDataType[]    (airbase ground-layout point headers)
//   Falcon4.PD   — PtDataType[]          (airbase ground-layout point coords)
//   Falcon4.UCD  — UnitClassDataType[]   (per-unit-class metadata + names)
//   Falcon4.VCD  — VehicleClassDataType[] (per-vehicle-class metadata + names)
//   Falcon4.FED  — FeatureEntry[]        (per-objective feature placement)
//   Falcon4.FCD  — FeatureClassDataType[] (per-feature-class metadata + names)
//
// These files live in the theater's `terrdata/objects/` directory and are
// shared across all campaigns in that theater. Unlike the .cam archive
// (which holds per-save dynamic state), these files hold static class
// metadata that never changes during a campaign.
//
// FILE LAYOUT (uniform across all seven files):
//
//   [short]   num_entries       (little-endian, signed)
//   [bytes]   entries[num_entries]
//
// Each entry is the struct's natural in-memory representation, written
// verbatim to disk (no padding adjustment, no compression). The on-disk
// record size therefore equals sizeof(struct) as compiled by MSVC with
// default 8-byte alignment. FreeFalcon's LoadObjectiveData/LoadPtData/etc.
// functions verify this with `if (size != sizeof(T) * entries + 2)`.
//
// FF-DB Control extension:
//   Some community-modded files have entries==0 in the header, with the
//   real count stored in the last 2 bytes of the file. We support both
//   formats: if the header says 0, we read the trailing short instead.
//
// ALIGNMENT NOTES (verified by size_probe.cpp):
//
//   sizeof(ObjClassDataType)     = 54 bytes (alignment 2)
//   sizeof(PtHeaderDataType)     = 28 bytes (alignment 4)
//   sizeof(PtDataType)           = 12 bytes (alignment 4)
//   sizeof(UnitClassDataType)    = 336 bytes (alignment 4)
//   sizeof(VehicleClassDataType) = 160 bytes (alignment 4)
//   sizeof(FeatureEntry)         = 32 bytes (alignment 4)
//   sizeof(FeatureClassDataType) = 60 bytes (alignment 4)
//
// All numeric fields are little-endian (Falcon4 was a Win32/x86 game).
// _TCHAR is char (ANSI build) — names are 20-byte (or 15-byte for vehicles)
// null-terminated char arrays.

#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace f4::world_convert {

// ============================================================================
// Constants from FreeFalcon (camplib.h, falcent.h, campweap.h, ptdata.h)
// ============================================================================

constexpr int TD_MOVEMENT_TYPES       = 8;
constexpr int TD_OTHER_DAM            = 10;       // DamageDataType::OtherDam
constexpr int TD_MAXIMUM_ROLES        = 16;
constexpr int TD_VEHICLE_GROUPS_PER_UNIT = 16;    // # of groups in a unit
constexpr int TD_MAX_FEAT_DEPEND      = 5;        // features a pt-list depends on
constexpr int TD_HARDPOINT_MAX        = 16;       // hardpoints per vehicle

// On-disk struct sizes (verified by /home/z/my-project/scripts/size_probe.cpp).
// These must match MSVC's default 8-byte alignment for the corresponding
// FreeFalcon struct definitions in src/falclib/include/entity.h.
constexpr std::size_t OCD_RECORD_SIZE = 54;   // ObjClassDataType
constexpr std::size_t PHD_RECORD_SIZE = 28;   // PtHeaderDataType
constexpr std::size_t PD_RECORD_SIZE  = 12;   // PtDataType
constexpr std::size_t UCD_RECORD_SIZE = 336;  // UnitClassDataType
constexpr std::size_t VCD_RECORD_SIZE = 160;  // VehicleClassDataType
constexpr std::size_t FED_RECORD_SIZE = 32;   // FeatureEntry
constexpr std::size_t FCD_RECORD_SIZE = 60;   // FeatureClassDataType
constexpr std::size_t RCD_RECORD_SIZE = 60;   // RadarClassDataType (Phase 3)

// ============================================================================
// Point type enum (from ptdata.h:40-60) — used by PtHeaderDataType.type
// and PtDataType.type. Determines the semantic role of a ground-layout
// point (runway threshold, taxiway node, parking spot, etc.).
// ============================================================================

enum PointType : uint8_t {
    PT_NOT_USED          = 0,
    PT_RUNWAY            = 1,   // Runway threshold (takeoff/landing end)
    PT_TAKEOFF           = 2,   // Takeoff position (held short of runway)
    PT_TAXI              = 3,   // Taxiway node
    PT_SAM               = 4,   // SAM site placement
    PT_ARTILLERY         = 5,   // Artillery placement
    PT_AAA               = 6,   // AAA placement
    PT_RADAR             = 7,   // Radar placement
    PT_RUNWAY_DIM        = 8,   // Runway dimensional point (length/width marks)
    PT_SUPPORT           = 9,   // Support vehicle placement
    PT_STATIC_RADAR      = 10,  // Static radar (building-sized)
    PT_SMALL_PARK        = 11,  // Small parking spot (fighters)
    PT_LARGE_PARK        = 12,  // Large parking spot (transports/bombers)
    PT_SMALL_DOCK        = 13,  // Small dock (small boats)
    PT_LARGE_DOCK        = 14,  // Large dock (capital ships)
    PT_TAKE_RUNWAY       = 15,  // Runway access point (taxiway → runway)
    PT_HELICOPTER        = 16,  // Helicopter pad
    PT_FOLLOW_ME         = 17,  // Follow-me truck rendezvous
    PT_TRACK             = 18,  // Track/path point (ground vehicle routes)
    PT_CRIT_TAXI         = 19,  // Critical taxiway intersection
};

// Point list type enum (from ptdata.h:62-78) — used by PtHeaderDataType.type
// to indicate what kind of point list this header begins.
enum PointListType : uint8_t {
    PLT_NONE              = 0,
    PLT_RUNWAY            = 1,   // Runway centerline points
    PLT_SAM               = 4,   // SAM placement points
    PLT_ARTILLERY         = 5,   // Artillery placement points
    PLT_AAA               = 6,   // AAA placement points
    PLT_RUNWAY_DIM        = 8,   // Runway dimensional marks
    PLT_STATIC_RADAR      = 10,  // Static radar placement
    PLT_PARK              = 11,  // Parking spots (small + large mixed)
    PLT_RUNWAY_LT         = 12,  // Runway left-side points
    PLT_RUNWAY_RT         = 13,  // Runway right-side points
    PLT_HELICOPTER        = 14,  // Helicopter landing spots
    PLT_FOLLOW_ME         = 15,  // Follow-me truck route
    PLT_DOCK              = 16,  // Docking points
    PLT_TRACK             = 17,  // Ground vehicle track
};

// ============================================================================
// Parsed struct types — POD mirrors of the on-disk FreeFalcon structs.
// All multi-byte fields are decoded from little-endian; all char arrays
// are trimmed of trailing NULs and exposed as std::string.
// ============================================================================

struct ObjectiveClassData {
    int16_t  index = 0;             // descriptionIndex pointing back into FALCON4.ct
    std::string name;               // 20-byte char array, e.g. "Airbase", "Bridge"
    int16_t  data_rate = 0;         // sort/recovery rate
    int16_t  deag_distance = 0;     // distance (m) at which to deaggregate
    int16_t  pt_data_index = 0;     // index into PtHeaderDataTable (airbase layout)
    std::array<uint8_t, TD_MOVEMENT_TYPES> detection{};   // electronic detection ranges
    std::array<uint8_t, TD_OTHER_DAM + 1> damage_mod{};   // damage modifiers per type
    int16_t  icon_index = 0;        // index into icon sheet
    uint8_t  features = 0;          // # of features in this objective (buildings, runways, etc.)
    uint8_t  radar_feature = 0;     // ID of the radar feature (0 if none)
    int16_t  first_feature = 0;     // index of first FeatureEntry in FED
};

struct PtHeaderData {
    int16_t  obj_id = 0;            // index into ObjDataTable (which objective this layout belongs to)
    uint8_t  type = 0;              // PointListType (1=runway, 11=parking, etc.)
    uint8_t  count = 0;             // # of PtData points in this list
    std::array<uint8_t, TD_MAX_FEAT_DEPEND> features{};  // feature indices this list depends on (255 = unused)
    int16_t  data = 0;              // type-specific (e.g. runway heading * 10)
    float    sin_heading = 0.0f;    // sin(heading) precomputed
    float    cos_heading = 0.0f;    // cos(heading) precomputed
    int16_t  first = 0;             // index of first PtData point
    int16_t  tex_idx = 0;           // texture index for runway rendering
    int8_t   runway_num = 0;        // -1 if not a runway, else which runway this list applies to
    int8_t   ltrt = 0;              // left/right offset flag for base point
    int16_t  next_header = 0;       // index of next header in the chain (0 = none)
};

struct PtData {
    float    x_offset = 0.0f;       // X offset (feet) from objective tile center
    float    y_offset = 0.0f;       // Y offset (feet) from objective tile center
    uint8_t  type = 0;              // PointType (1=runway, 3=taxi, 11=small park, etc.)
    uint8_t  flags = 0;             // PT_FIRST / PT_LAST / PT_OCCUPIED
};

struct UnitClassData {
    int16_t  index = 0;
    std::array<int32_t, TD_VEHICLE_GROUPS_PER_UNIT> num_elements{};  // per-group vehicle count
    std::array<int16_t, TD_VEHICLE_GROUPS_PER_UNIT> vehicle_type{};  // class-table index per group
    std::array<std::array<uint8_t, 8>, TD_VEHICLE_GROUPS_PER_UNIT> vehicle_class{};  // 8-byte class descriptors
    uint16_t flags = 0;             // VEH_ capability flags
    std::string name;               // 20-byte char array, e.g. "Armor", "Infantry"
    int32_t  movement_type = 0;     // MoveType enum (Foot=1, Wheeled=2, Tracked=3, ...)
    int16_t  movement_speed = 0;    // cruise speed (kph)
    int16_t  max_range = 0;         // movement/flight range with full supply (km)
    int32_t  fuel = 0;              // internal fuel (lbs)
    int16_t  rate = 0;              // fuel usage (lbs/min at cruise)
    int16_t  pt_data_index = 0;     // index into PtHeaderDataTable (formation layout)
    std::array<uint8_t, TD_MAXIMUM_ROLES> scores{};  // score per mission role
    uint8_t  role = 0;              // standard mission role
    std::array<uint8_t, TD_MOVEMENT_TYPES> hit_chance{};  // best hit chance per movement type
    std::array<uint8_t, TD_MOVEMENT_TYPES> strength{};    // full strength per movement type
    std::array<uint8_t, TD_MOVEMENT_TYPES> range{};       // firing range per movement type
    std::array<uint8_t, TD_MOVEMENT_TYPES> detection{};   // electronic detection ranges
    std::array<uint8_t, TD_OTHER_DAM + 1> damage_mod{};   // damage modifiers per type
    uint8_t  radar_vehicle = 0;     // ID of the radar vehicle in this unit (group index)
    int16_t  special_index = 0;     // for squadrons: index to max stores table
    int16_t  icon_index = 0;        // index to this unit's icon type
};

struct VehicleClassData {
    int16_t  index = 0;
    int16_t  hit_points = 0;
    uint32_t flags = 0;             // VEH_ flags (see vehicle.h)
    std::string name;               // 15-byte char array, e.g. "M1A2 Abrams"
    std::string nctr;               // 5-byte char array, e.g. "M1A2"
    float    rcs_factor = 0.0f;     // log2(1 + RCS relative to F-16)
    int32_t  max_wt = 0;            // max loaded weight (lbs)
    int32_t  empty_wt = 0;          // empty weight (lbs)
    int32_t  fuel_wt = 0;           // max fuel weight (lbs)
    int16_t  fuel_econ = 0;         // fuel usage (lbs/min)
    int16_t  engine_sound = 0;      // SoundFX sample index
    int16_t  high_alt = 0;          // in hundreds of feet
    int16_t  low_alt = 0;
    int16_t  cruise_alt = 0;
    int16_t  max_speed = 0;         // kph
    int16_t  radar_type = 0;        // index into RadarDataTable
    int16_t  number_of_pilots = 0;  // # of pilots (for eject)
    uint16_t rack_flags = 0;        // bit per hardpoint: needs a rack?
    uint16_t visible_flags = 0;     // bit per hardpoint: visible?
    uint8_t  callsign_index = 0;
    uint8_t  callsign_slots = 0;
    std::array<uint8_t, TD_MOVEMENT_TYPES> hit_chance{};
    std::array<uint8_t, TD_MOVEMENT_TYPES> strength{};
    std::array<uint8_t, TD_MOVEMENT_TYPES> range{};
    std::array<uint8_t, TD_MOVEMENT_TYPES> detection{};
    std::array<int16_t, TD_HARDPOINT_MAX> weapon{};   // weapon ID per hardpoint (or weapon list ID)
    std::array<uint8_t, TD_HARDPOINT_MAX> weapons{};  // # of shots per hardpoint (full supply)
    std::array<uint8_t, TD_OTHER_DAM + 1> damage_mod{};
};

struct FeatureClassData {
    int16_t  index = 0;
    int16_t  repair_time = 0;       // seconds to repair from destroyed to operational
    uint8_t  priority = 0;          // display priority
    uint16_t flags = 0;             // FEAT_ flags (see feature.h)
    std::string name;               // 20-byte char array, e.g. "Control Tower"
    int16_t  hit_points = 0;
    int16_t  height = 0;            // height of vehicle ramp (if any)
    float    angle = 0.0f;          // angle of vehicle ramp (if any)
    int16_t  radar_type = 0;        // index into RadarDataTable
    std::array<uint8_t, TD_MOVEMENT_TYPES> detection{};
    std::array<uint8_t, TD_OTHER_DAM + 1> damage_mod{};
};

struct FeatureEntryData {
    int16_t  index = 0;             // entity class index of the feature
    uint16_t flags = 0;
    std::array<uint8_t, 8> e_class{};   // 8-byte entity class array
    uint8_t  value = 0;             // % loss in operational status for destruction
    float    offset_x = 0.0f;       // X offset from objective center (feet)
    float    offset_y = 0.0f;
    float    offset_z = 0.0f;
    int16_t  facing = 0;            // facing angle (degrees)
};

/// RadarClassData — one entry in Falcon4.RCD (RadarDataTable).
///
/// Phase 3 fix: previously NOT parsed, which forced the viewer's radar arcs
/// overlay to use a fabricated "32 grid units ≈ 10 km" constant radius
/// (see canvas.cpp's nominal_radius_grid). With the real RCD data, each
/// radar-equipped objective's coverage can be drawn at its actual range.
///
/// On-disk record size: 60 bytes (verified against snapshot dumps showing
/// 56 records × 60 bytes). The struct layout below is a partial decode of
/// FreeFalcon's RadarClassDataType (from simdata.h) — we capture the
/// fields most useful for visualization (Index, Name, Range). The
/// remaining bytes are skipped as opaque padding; consumers that need
/// other fields (RcsType, DetectionChance per band, etc.) can extend
/// this struct and the parser later.
///
/// Best-known on-disk layout (Phase 3; will be refined when more
/// ground-truth is available):
///   off 0: Index       (short, 2)
///   off 2: Name[28]    (char[28], 28) — radar name, e.g. "APG-68", "Straight Flush"
///   off30: Range       (float, 4)     — detection range (km)
///   off34: *(remaining 26 bytes — opaque; includes radar type flags,
///           per-band detection ratios, etc.)*
///   total = 60 bytes
struct RadarClassData {
    int16_t  index = 0;            // cross-reference index used by VCD/FCD radar_type
    std::string name;              // 28-byte char array, e.g. "APG-68", "Pat Hand"
    float    range_km = 0.0f;      // detection range in kilometers
};

// ============================================================================
// Container types — one struct per file, holding all parsed records.
// ============================================================================

struct ObjectiveClassTable {
    std::vector<ObjectiveClassData> entries;
    [[nodiscard]] bool loaded() const noexcept { return !entries.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return entries.size(); }
    /// Lookup by objective index (0-based, matches ObjDataTable[i] in FF).
    [[nodiscard]] const ObjectiveClassData* at(std::size_t i) const noexcept {
        return i < entries.size() ? &entries[i] : nullptr;
    }
};

struct PtHeaderTable {
    std::vector<PtHeaderData> entries;
    [[nodiscard]] bool loaded() const noexcept { return !entries.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return entries.size(); }
    [[nodiscard]] const PtHeaderData* at(std::size_t i) const noexcept {
        return i < entries.size() ? &entries[i] : nullptr;
    }
};

struct PtDataTable {
    std::vector<PtData> entries;
    [[nodiscard]] bool loaded() const noexcept { return !entries.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return entries.size(); }
    [[nodiscard]] const PtData* at(std::size_t i) const noexcept {
        return i < entries.size() ? &entries[i] : nullptr;
    }
};

struct UnitClassTable {
    std::vector<UnitClassData> entries;
    [[nodiscard]] bool loaded() const noexcept { return !entries.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return entries.size(); }
    [[nodiscard]] const UnitClassData* at(std::size_t i) const noexcept {
        return i < entries.size() ? &entries[i] : nullptr;
    }
};

struct VehicleClassTable {
    std::vector<VehicleClassData> entries;
    [[nodiscard]] bool loaded() const noexcept { return !entries.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return entries.size(); }
    [[nodiscard]] const VehicleClassData* at(std::size_t i) const noexcept {
        return i < entries.size() ? &entries[i] : nullptr;
    }
};

struct FeatureClassTable {
    std::vector<FeatureClassData> entries;
    [[nodiscard]] bool loaded() const noexcept { return !entries.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return entries.size(); }
    [[nodiscard]] const FeatureClassData* at(std::size_t i) const noexcept {
        return i < entries.size() ? &entries[i] : nullptr;
    }
};

struct FeatureEntryTable {
    std::vector<FeatureEntryData> entries;
    [[nodiscard]] bool loaded() const noexcept { return !entries.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return entries.size(); }
    [[nodiscard]] const FeatureEntryData* at(std::size_t i) const noexcept {
        return i < entries.size() ? &entries[i] : nullptr;
    }
};

struct RadarClassTable {
    std::vector<RadarClassData> entries;
    [[nodiscard]] bool loaded() const noexcept { return !entries.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return entries.size(); }
    [[nodiscard]] const RadarClassData* at(std::size_t i) const noexcept {
        return i < entries.size() ? &entries[i] : nullptr;
    }
};

// ============================================================================
// Top-level loaders — one per file. Each reads the file, verifies the
// size-assertion (file_size == sizeof(struct) * count + 2), and decodes
// every record. Throws std::runtime_error on I/O or parse error.
//
// All loaders accept the path WITHOUT extension; FreeFalcon's OpenCampFile
// appends the extension. We follow the same convention to match the
// canonical usage: e.g. load_objective_data("Falcon4") reads "Falcon4.OCD"
// from the same directory. If the no-extension path doesn't exist, the
// loader also tries the path verbatim (with whatever extension it has).
// ============================================================================

/// Load Falcon4.OCD — per-objective-class metadata (name, features, layout).
void load_objective_data(const std::filesystem::path& base_path,
                         ObjectiveClassTable& out);

/// Load Falcon4.PHD — airbase ground-layout point headers (one per runway/
/// taxiway/parking-spot list). Each header points to a contiguous run of
/// PtData entries via `first` / `count`.
void load_pt_header_data(const std::filesystem::path& base_path,
                         PtHeaderTable& out);

/// Load Falcon4.PD — airbase ground-layout point coordinates. Indexed by
/// PtHeaderData.first / .count.
void load_pt_data(const std::filesystem::path& base_path,
                  PtDataTable& out);

/// Load Falcon4.UCD — per-unit-class metadata (vehicle composition, fuel,
/// movement, combat stats).
void load_unit_data(const std::filesystem::path& base_path,
                    UnitClassTable& out);

/// Load Falcon4.VCD — per-vehicle-class metadata (hit points, weight,
/// fuel, weapons, RCS).
void load_vehicle_data(const std::filesystem::path& base_path,
                       VehicleClassTable& out);

/// Load Falcon4.FCD — per-feature-class metadata (a "feature" is a building
/// or structure within an objective — control tower, runway section, etc.).
void load_feature_data(const std::filesystem::path& base_path,
                       FeatureClassTable& out);

/// Load Falcon4.FED — per-objective feature placement (which feature goes
/// where within each objective).
void load_feature_entry_data(const std::filesystem::path& base_path,
                             FeatureEntryTable& out);

/// Load Falcon4.RCD — radar class definitions (range + detection ratios).
/// Phase 3 fix: previously NOT parsed, which forced the viewer's radar arcs
/// overlay to use a fabricated constant radius. With this loader, the
/// viewer can draw radar coverage at the actual range for each radar type.
void load_radar_data(const std::filesystem::path& base_path,
                     RadarClassTable& out);

// ============================================================================
// Convenience: a single aggregate holding all seven tables. Use this when
// you want to load the entire static-object database in one shot.
// ============================================================================

/// Per-file load outcome, captured by TheaterObjectDatabase::load_all.
/// One entry per Falcon4.* file attempted. Allows callers (viewer,
/// diagnostics, AI data init) to tell *why* a table is empty: because
/// the file was missing, because it failed to parse, or because it
/// parsed cleanly but contained zero records.
struct TheaterFileLoadResult {
    std::string filename;        // e.g. "Falcon4.OCD"
    enum class Status { Missing, ParseError, Loaded } status{Status::Missing};
    std::size_t record_count{0}; // entries decoded (0 if Missing or ParseError)
    std::string message;         // empty if Loaded; else human-readable cause
};

struct TheaterObjectDatabase {
    ObjectiveClassTable objectives;     // Falcon4.OCD
    PtHeaderTable       pt_headers;     // Falcon4.PHD
    PtDataTable         pt_data;        // Falcon4.PD
    UnitClassTable      units;          // Falcon4.UCD
    VehicleClassTable   vehicles;       // Falcon4.VCD
    FeatureClassTable   features;       // Falcon4.FCD
    FeatureEntryTable   feature_entries; // Falcon4.FED
    RadarClassTable     radars;         // Falcon4.RCD (Phase 3)

    /// Diagnostics from the last `load_all` call, one entry per file tried.
    /// Cleared at the start of each `load_all`. Stable order matches the
    /// declaration order above (OCD, PHD, PD, UCD, VCD, FCD, FED, RCD).
    std::vector<TheaterFileLoadResult> load_diagnostics;

    /// Load all eight files from `dir/Falcon4.{OCD,PHD,PD,UCD,VCD,FCD,FED,RCD}`.
    /// Missing files are recorded in `load_diagnostics` with status=Missing
    /// (their table stays empty). Parse errors are recorded with status=
    /// ParseError and the exception message; the table still stays empty.
    /// Use the per-table `loaded()` method or `load_diagnostics` to inspect.
    void load_all(const std::filesystem::path& dir);

    /// True if at least one of the eight tables is loaded.
    [[nodiscard]] bool loaded() const noexcept {
        return objectives.loaded() || pt_headers.loaded() || pt_data.loaded()
            || units.loaded()       || vehicles.loaded()  || features.loaded()
            || feature_entries.loaded() || radars.loaded();
    }
};

// ============================================================================
// Helpers — exposed for the JSON emitter and viewer.
// ============================================================================

/// Human-readable name for a PointType (1=Runway, 3=Taxi, 11=Small Park, ...).
/// Returns "Unknown" for unrecognized values.
[[nodiscard]] const char* point_type_name(uint8_t pt_type) noexcept;

/// Human-readable name for a PointListType (1=Runway list, 11=Parking list, ...).
/// Returns "Unknown" for unrecognized values.
[[nodiscard]] const char* point_list_type_name(uint8_t plt_type) noexcept;

/// Human-readable name for a MoveType (1=Foot, 2=Wheeled, 3=Tracked, ...).
/// Returns "Unknown" for unrecognized values.
[[nodiscard]] const char* movement_type_name(int32_t mt) noexcept;

/// Find a file with the given base name and extension, trying several
/// search paths. Returns an empty path if not found.
///
/// Search order:
///   1. base_path + "." + ext  (e.g. "Falcon4" + ".OCD" = "Falcon4.OCD")
///   2. base_path as-is (if it already has the extension)
///   3. Case-insensitive match in base_path's parent directory
[[nodiscard]] std::filesystem::path
find_theater_file(const std::filesystem::path& base_path,
                  const std::string& ext);

} // namespace f4::world_convert
