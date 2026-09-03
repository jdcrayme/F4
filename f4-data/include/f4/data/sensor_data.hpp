// f4-data/sensor_data.hpp
//
// Data-only representation of the SimData sensor authoring files
// (sim/SENSDATA/IRST/*.IRS, sim/SENSDATA/RWR/*.RWR,
// sim/SENSDATA/VISUAL/*.VSS + their .LST index files).
//
// FIDELITY NOTE (documented, load-bearing): these text files have NO
// readers in the FreeFalcon tree — the runtime loads precompiled binary
// tables (.ICD/.VSD/.RWD via entity.cpp LoadIRSTData/LoadVisualData/
// LoadRwrData, fread of IRSTDataType/VisualDataType/RwrDataType). The
// text files in SimData.zip are the 1998 authoring source for those
// tables (the same design-data situation as BRAINDAT.brn). This port
// parses the TEXT (what the zip actually ships), with field names taken
// from each file's own '#' comments, and records the compiled-struct
// mapping in the notes below.
//
// Formats (5 whitespace-separated values each, '#' comments):
//   .IRS  (IRST seeker):   azimuth limit (deg), elevation limit (deg),
//                          nominal range (NM), ground factor,
//                          flare chance.
//                          Compiled IRSTDataType (irstdata.h):
//                          NominalRange, FOVHalfAngle <- az limit,
//                          GimbalLimitHalfAngle <- el limit, GroundFactor,
//                          FlareChance.
//   .RWR  (RWR receiver):  azimuth limit (deg), elevation limit (deg),
//                          sensitivity — "fraction of nominal range of
//                          emitter at which it is detected" (1.0 generic,
//                          2.0 harm — the HARM's passive receiver hears
//                          emitters twice as far).
//                          Compiled RwrDataType (rwrdata.h): nominalRange,
//                          top/bottom <- el limit, left/right <- az limit,
//                          flag.
//   .VSS  (visual sensor): azimuth limit (deg), elevation limit (deg),
//                          gain. Gain is nominalRange squared in ft^2
//                          (the original signal model: signal = gain /
//                          range_ft^2 >= 1 -> detect, visual.cpp's now
//                          commented-out threshold). generic.vss ships
//                          3.7e9 = (10 NM in feet)^2 within 1%.
//
// The .LST files are one count followed by one file name per line
// (lowercase in the shipped files, e.g. "generic.irs").

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace f4::data {

/// Feet per nautical mile (matches f4-geo's FEET_PER_NM; restated here
/// because this header must stay dependency-free like the other
/// f4-data SimData headers).
inline constexpr double kSensorFeetPerNm = 6076.11548;

// ---------------------------------------------------------------------------
// IRST seekers (.IRS)
// ---------------------------------------------------------------------------
struct IrstSeekerData {
    double az_limit_deg = 60.0;       // FOV half angle (aim9l: 60)
    double el_limit_deg = 60.0;       // gimbal limit half angle
    double nominal_range_nm = 10.0;   // detection range vs F-16-sized target
    double ground_factor = 0.001;     // range multiplier vs ground targets
    double flare_chance = 0.2;        // base P(a flare defeats the seeker)

    [[nodiscard]] bool operator==(const IrstSeekerData&) const = default;
};

// ---------------------------------------------------------------------------
// RWR receivers (.RWR)
// ---------------------------------------------------------------------------
struct RwrReceiverData {
    double az_limit_deg = 180.0;   // full sphere for generic (180 az + 90 el)
    double el_limit_deg = 90.0;
    /// Fraction of an emitter's nominal range at which the receiver
    /// detects it (generic 1.0, harm 2.0).
    double sensitivity = 1.0;

    [[nodiscard]] bool operator==(const RwrReceiverData&) const = default;
};

// ---------------------------------------------------------------------------
// Visual sensors (.VSS) — the Mk1 eyeball / seeker TV camera.
// ---------------------------------------------------------------------------
struct VisualSensorData {
    double az_limit_deg = 181.0;   // generic: "everything"
    double el_limit_deg = 91.0;
    /// nominalRange^2 in ft^2 (signal = gain / range_ft^2 >= 1).
    double gain = 3.7e9;

    [[nodiscard]] bool operator==(const VisualSensorData&) const = default;

    /// The original model's detection range implied by the gain
    /// (sqrt(gain) feet; generic = ~10 NM).
    [[nodiscard]] double nominal_range_nm() const noexcept;
};

// ---------------------------------------------------------------------------
// Named entries + per-family libraries.
// ---------------------------------------------------------------------------
template <typename T>
struct NamedSensor {
    std::string name;   // stem from the .LST ("generic", "aim9l", ...)
    T data{};

    [[nodiscard]] bool operator==(const NamedSensor&) const = default;
};

using IrstSensorEntry = NamedSensor<IrstSeekerData>;
using RwrSensorEntry = NamedSensor<RwrReceiverData>;
using VisualSensorEntry = NamedSensor<VisualSensorData>;

struct IrstSensorData {
    std::vector<IrstSensorEntry> sensors;
    /// Case-insensitive lookup; nullptr when absent.
    [[nodiscard]] const IrstSensorEntry* find(
        std::string_view name) const noexcept;
};

struct RwrSensorData {
    std::vector<RwrSensorEntry> sensors;
    [[nodiscard]] const RwrSensorEntry* find(
        std::string_view name) const noexcept;
};

struct VisualSensorDataLibrary {
    std::vector<VisualSensorEntry> sensors;
    [[nodiscard]] const VisualSensorEntry* find(
        std::string_view name) const noexcept;
};

// ---------------------------------------------------------------------------
// JSON serialization (canonical format; f4-convert delegates here).
// Tags: f4.irstdata / f4.rwrdata / f4.visualdata, version 1.
// ---------------------------------------------------------------------------
struct IrstDataResult {
    IrstSensorData data;
    bool ok = false;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

struct RwrDataResult {
    RwrSensorData data;
    bool ok = false;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

struct VisualDataResult {
    VisualSensorDataLibrary data;
    bool ok = false;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

[[nodiscard]] IrstDataResult loadIrstSensorData(const std::string& path);
[[nodiscard]] IrstDataResult loadIrstSensorDataFromString(
    const std::string& json);
[[nodiscard]] std::string writeIrstSensorData(const IrstSensorData& data);
bool writeIrstSensorDataFile(const IrstSensorData& data,
                              const std::string& path);

[[nodiscard]] RwrDataResult loadRwrSensorData(const std::string& path);
[[nodiscard]] RwrDataResult loadRwrSensorDataFromString(
    const std::string& json);
[[nodiscard]] std::string writeRwrSensorData(const RwrSensorData& data);
bool writeRwrSensorDataFile(const RwrSensorData& data,
                             const std::string& path);

[[nodiscard]] VisualDataResult loadVisualSensorData(const std::string& path);
[[nodiscard]] VisualDataResult loadVisualSensorDataFromString(
    const std::string& json);
[[nodiscard]] std::string writeVisualSensorData(
    const VisualSensorDataLibrary& data);
bool writeVisualSensorDataFile(const VisualSensorDataLibrary& data,
                               const std::string& path);

} // namespace f4::data
