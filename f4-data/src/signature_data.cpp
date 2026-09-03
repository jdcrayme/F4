// f4-data/src/signature_data.cpp
//
// SignatureGrid bilinear interpolation (the Math.TwodInterp contract,
// visual.cpp:79-99) + canonical JSON serialization (f4.sigdata v1).

#include "f4/data/signature_data.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>

namespace f4::data {

using json = nlohmann::json;

namespace {

std::string canonicalName(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(
            static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

/// Index of the last breakpoint <= v (clamped); ascending input assumed.
/// `lo` receives the bracket start index.
[[nodiscard]] std::size_t bracketIndex(const std::vector<double>& axis,
                                       double v,
                                       std::size_t& lo) noexcept {
    if (axis.empty()) {
        lo = 0;
        return 0;
    }
    if (v <= axis.front()) {
        lo = 0;
        return 0;
    }
    if (v >= axis.back()) {
        lo = axis.size() - 1;
        return lo;
    }
    // Binary search for the first breakpoint > v.
    const auto it = std::upper_bound(axis.begin(), axis.end(), v);
    const std::size_t hi = static_cast<std::size_t>(it - axis.begin());
    lo = hi - 1;
    return hi;
}

[[nodiscard]] double axisFraction(double v, double lo, double hi) noexcept {
    if (hi <= lo) return 0.0;
    return std::clamp((v - lo) / (hi - lo), 0.0, 1.0);
}

json gridToJson(const SignatureGrid& g) {
    json j;
    j["azimuth_deg"] = g.azimuth_deg;
    j["elevation_deg"] = g.elevation_deg;
    json rows = json::array();
    for (const auto& row : g.values) rows.push_back(row);
    j["values"] = std::move(rows);
    return j;
}

SignatureGrid gridFromJson(const json& j) {
    SignatureGrid g;
    if (j.contains("azimuth_deg") && j["azimuth_deg"].is_array()) {
        g.azimuth_deg = j["azimuth_deg"].get<std::vector<double>>();
    }
    if (j.contains("elevation_deg") && j["elevation_deg"].is_array()) {
        g.elevation_deg = j["elevation_deg"].get<std::vector<double>>();
    }
    if (j.contains("values") && j["values"].is_array()) {
        for (const auto& row : j["values"]) {
            if (!row.is_array()) continue;
            std::vector<double> r;
            for (const auto& v : row) r.push_back(v.get<double>());
            g.values.push_back(std::move(r));
        }
    }
    return g;
}

} // namespace

// ---------------------------------------------------------------------------
// Bilinear lookup
// ---------------------------------------------------------------------------
double SignatureGrid::value_at(double az, double el) const noexcept {
    if (values.empty() || azimuth_deg.empty() || elevation_deg.empty()) {
        return 0.0;
    }

    // Wrap azimuth onto the grid's breakpoint range: normalize to
    // [0, 360), mirror into [0, 180] (a signature at -30 off the nose is
    // the same target aspect as +30 — every shipped grid is symmetric),
    // which both breakpoint conventions (-180..180 and 0..180) contain.
    az = std::fmod(az, 360.0);
    if (az < 0.0) az += 360.0;
    if (az > 180.0) az = 360.0 - az;          // [0, 180]
    az = std::clamp(az, azimuth_deg.front(), azimuth_deg.back());

    std::size_t ax = 0, ay = 0;
    const std::size_t axHi = bracketIndex(azimuth_deg, az, ax);
    const std::size_t ayHi = bracketIndex(elevation_deg, el, ay);

    const double tx = axisFraction(az, azimuth_deg[ax],
                                   azimuth_deg[std::min(axHi, azimuth_deg.size() - 1)]);
    const double ty = axisFraction(el, elevation_deg[ay],
                                   elevation_deg[std::min(ayHi, elevation_deg.size() - 1)]);

    const auto rowAt = [&](std::size_t y) -> const std::vector<double>& {
        return values[std::min(y, values.size() - 1)];
    };
    const auto at = [&](std::size_t y, std::size_t x) -> double {
        const auto& row = rowAt(y);
        return row[std::min(x, row.size() - 1)];
    };

    // Bilinear over the four bracket corners.
    const double v00 = at(ay, ax);
    const double v01 = at(ay, axHi);
    const double v10 = at(ayHi, ax);
    const double v11 = at(ayHi, axHi);
    return v00 * (1.0 - tx) * (1.0 - ty) + v01 * tx * (1.0 - ty) +
           v10 * (1.0 - tx) * ty + v11 * tx * ty;
}

// ---------------------------------------------------------------------------
// Library lookup
// ---------------------------------------------------------------------------
const AircraftSignatureData* SignatureDataLibrary::find(
    std::string_view name) const noexcept {
    const std::string want = canonicalName(name);
    for (const auto& e : entries) {
        if (canonicalName(e.name) == want) return &e;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------
std::string writeSignatureDataLibrary(const SignatureDataLibrary& lib) {
    json j;
    j["kind"] = "f4.sigdata";
    j["version"] = 1;
    json arr = json::array();
    for (const auto& e : lib.entries) {
        arr.push_back({
            {"name", e.name},
            {"rcs", gridToJson(e.rcs)},
            {"ir0", gridToJson(e.ir0)},
            {"ir1", gridToJson(e.ir1)},
            {"ir2", gridToJson(e.ir2)},
            {"visual", gridToJson(e.visual)},
        });
    }
    j["signatures"] = std::move(arr);
    return j.dump(2) + "\n";
}

bool writeSignatureDataLibraryFile(const SignatureDataLibrary& lib,
                                   const std::string& path) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out << writeSignatureDataLibrary(lib);
    return out.good();
}

SignatureDataResult loadSignatureDataLibraryFromString(
    const std::string& contents) {
    SignatureDataResult result;
    json j = json::parse(contents, nullptr, false);
    if (j.is_discarded()) {
        result.errors.push_back("invalid JSON");
        return result;
    }
    const std::string kind = j.value("kind", "");
    if (kind != "f4.sigdata" && kind != "sigdata") {
        result.errors.push_back("wrong kind tag: '" + kind + "'");
        return result;
    }
    for (const auto& je : j.value("signatures", json::array())) {
        AircraftSignatureData e;
        e.name = je.value("name", "");
        if (je.contains("rcs")) e.rcs = gridFromJson(je["rcs"]);
        if (je.contains("ir0")) e.ir0 = gridFromJson(je["ir0"]);
        if (je.contains("ir1")) e.ir1 = gridFromJson(je["ir1"]);
        if (je.contains("ir2")) e.ir2 = gridFromJson(je["ir2"]);
        if (je.contains("visual")) e.visual = gridFromJson(je["visual"]);
        result.library.entries.push_back(std::move(e));
    }
    result.ok = true;
    return result;
}

SignatureDataResult loadSignatureDataLibrary(const std::string& path) {
    SignatureDataResult result;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        result.errors.push_back("cannot open " + path);
        return result;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return loadSignatureDataLibraryFromString(ss.str());
}

} // namespace f4::data
