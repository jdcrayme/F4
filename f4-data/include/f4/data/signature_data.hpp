// f4-data/signature_data.hpp
//
// Data-only representation of the SimData signature grids
// (sim/SIGDATA/RCSDAT/*.RCS, sim/SIGDATA/IR/*.IR0/.IR1/.IR2,
// sim/SIGDATA/VISUAL/*.VIS + sim/SIGDATA/SIGDATA.LST).
//
// FIDELITY NOTE (documented, load-bearing): like the SENSDATA text
// files, these grids have NO readers in the FreeFalcon tree — the
// runtime consumes precompiled binary campdata (the RCD tables the
// f4-sensors detection.hpp header has been waiting for). The text
// grids in SimData.zip are the authoring source: azimuth breakpoint
// list, elevation breakpoint list, then one row of values per
// elevation breakpoint (numAz values each). VisualClass::GetSignature
// (visual.cpp:79-99) documents the intended consumption exactly:
//   "return Math.TwodInterp(obj->localData->azFrom, elFrom,
//    visData->azData, visData->elData, visData->signature, numAz, numEl)"
// — i.e. bilinear interpolation of the grid at (azimuth, elevation)
// from the OBSERVER to the target.
//
// Grid semantics per family (from the shipped generic data):
//   .RCS  — radar cross section in m^2 by azimuth/elevation off the
//           TARGET's axes (generic: flat 10.0 m^2 everywhere; the
//           breakpoints run -180/0/180 azimuth, -90/0/90 elevation).
//   .IR0  — baseline IR signature ratio (azimuth 0/90/180 with the
//           hot rear: el 0 row "0.02 0.03 0.1").
//   .IR1  — afterburner IR signature ratio (el 0 row "0.2 0.5 0.1").
//   .IR2  — maximum IR signature ratio (el 0 row "0.8 3.5 4.0").
//   .VIS  — visual detection size factor (generic: flat 1.0).
//
// SIGDATA.LST is one count followed by one signature stem per line
// ("generic"); each stem loads the five grids rcsdat/<stem>.RCS,
// ir/<stem>.IR0/.IR1/.IR2, visual/<stem>.VIS.

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace f4::data {

// ---------------------------------------------------------------------------
// SignatureGrid — azimuth x elevation breakpoint table with bilinear
// lookup (the Math.TwodInterp contract from visual.cpp:79-99).
// ---------------------------------------------------------------------------
struct SignatureGrid {
    /// Ascending azimuth breakpoints (degrees; RCS/VIS grids run
    /// -180..180, IR grids 0..180).
    std::vector<double> azimuth_deg;

    /// Ascending elevation breakpoints (degrees, -90..90).
    std::vector<double> elevation_deg;

    /// values[elevation index][azimuth index].
    std::vector<std::vector<double>> values;

    [[nodiscard]] bool operator==(const SignatureGrid&) const = default;

    /// Bilinear interpolated value at (azimuth, elevation), clamped to
    /// the grid edges. Azimuth wraps modulo 360 into the breakpoint
    /// range (aspect -30 == 330 == 30 by symmetry). Returns 0.0 for an
    /// empty grid.
    [[nodiscard]] double value_at(double azimuth_deg,
                                  double elevation_deg) const noexcept;
};

// ---------------------------------------------------------------------------
// AircraftSignatureData — the five grids for one SIGDATA.LST stem.
// ---------------------------------------------------------------------------
struct AircraftSignatureData {
    std::string name;   // "generic"
    SignatureGrid rcs;      // radar cross section, m^2
    SignatureGrid ir0;      // IR baseline ratio
    SignatureGrid ir1;      // IR afterburner ratio
    SignatureGrid ir2;      // IR maximum ratio
    SignatureGrid visual;   // visual size factor

    [[nodiscard]] bool operator==(const AircraftSignatureData&) const = default;
};

struct SignatureDataLibrary {
    std::vector<AircraftSignatureData> entries;

    /// Case-insensitive lookup; nullptr when absent.
    [[nodiscard]] const AircraftSignatureData* find(
        std::string_view name) const noexcept;
};

// ---------------------------------------------------------------------------
// JSON serialization (canonical format; f4-convert delegates here).
// Tag: f4.sigdata, version 1.
// ---------------------------------------------------------------------------
struct SignatureDataResult {
    SignatureDataLibrary library;
    bool ok = false;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

/// Load from a JSON file on disk.
[[nodiscard]] SignatureDataResult loadSignatureDataLibrary(
    const std::string& path);

/// Load from a JSON string.
[[nodiscard]] SignatureDataResult loadSignatureDataLibraryFromString(
    const std::string& json);

/// Serialize to a pretty-printed JSON string.
[[nodiscard]] std::string writeSignatureDataLibrary(
    const SignatureDataLibrary& lib);

/// Write to a JSON file. Returns true on success.
bool writeSignatureDataLibraryFile(const SignatureDataLibrary& lib,
                                   const std::string& path);

} // namespace f4::data
