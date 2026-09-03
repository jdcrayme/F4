// f4-data/src/sensor_data.cpp
//
// Sensor data lookups + canonical JSON serialization (three families:
// f4.irstdata / f4.rwrdata / f4.visualdata, all version 1).

#include "f4/data/sensor_data.hpp"

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

[[nodiscard]] std::string readTag(const json& j,
                                  const char* want,
                                  std::vector<std::string>& errors) {
    const std::string kind = j.value("kind", "");
    const std::string bare = want;
    const std::string full = "f4." + bare;
    if (kind != full && kind != bare) {
        errors.push_back("wrong kind tag: '" + kind + "'");
        return {};
    }
    return kind;
}

} // namespace

// ---------------------------------------------------------------------------
// VisualSensorData helper
// ---------------------------------------------------------------------------
double VisualSensorData::nominal_range_nm() const noexcept {
    // signal = gain / range_ft^2 >= 1  ->  range_ft = sqrt(gain)
    return std::sqrt(std::max(gain, 0.0)) / kSensorFeetPerNm;
}

// ---------------------------------------------------------------------------
// find() overloads
// ---------------------------------------------------------------------------
const IrstSensorEntry* IrstSensorData::find(
    std::string_view name) const noexcept {
    const std::string want = canonicalName(name);
    for (const auto& e : sensors) {
        if (canonicalName(e.name) == want) return &e;
    }
    return nullptr;
}

const RwrSensorEntry* RwrSensorData::find(
    std::string_view name) const noexcept {
    const std::string want = canonicalName(name);
    for (const auto& e : sensors) {
        if (canonicalName(e.name) == want) return &e;
    }
    return nullptr;
}

const VisualSensorEntry* VisualSensorDataLibrary::find(
    std::string_view name) const noexcept {
    const std::string want = canonicalName(name);
    for (const auto& e : sensors) {
        if (canonicalName(e.name) == want) return &e;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// IRST JSON
// ---------------------------------------------------------------------------
namespace {

json irstToJson(const IrstSensorData& data) {
    json j;
    j["kind"] = "f4.irstdata";
    j["version"] = 1;
    json arr = json::array();
    for (const auto& e : data.sensors) {
        arr.push_back({
            {"name", e.name},
            {"az_limit_deg", e.data.az_limit_deg},
            {"el_limit_deg", e.data.el_limit_deg},
            {"nominal_range_nm", e.data.nominal_range_nm},
            {"ground_factor", e.data.ground_factor},
            {"flare_chance", e.data.flare_chance},
        });
    }
    j["sensors"] = std::move(arr);
    return j;
}

IrstDataResult irstFromJsonString(const std::string& contents) {
    IrstDataResult result;
    json j = json::parse(contents, nullptr, false);
    if (j.is_discarded()) {
        result.errors.push_back("invalid JSON");
        return result;
    }
    if (readTag(j, "irstdata", result.errors).empty() &&
        !result.errors.empty()) {
        return result;
    }
    for (const auto& je : j.value("sensors", json::array())) {
        IrstSensorEntry e;
        e.name = je.value("name", "");
        e.data.az_limit_deg = je.value("az_limit_deg", 60.0);
        e.data.el_limit_deg = je.value("el_limit_deg", 60.0);
        e.data.nominal_range_nm = je.value("nominal_range_nm", 10.0);
        e.data.ground_factor = je.value("ground_factor", 0.001);
        e.data.flare_chance = je.value("flare_chance", 0.2);
        result.data.sensors.push_back(std::move(e));
    }
    result.ok = true;
    return result;
}

} // namespace

std::string writeIrstSensorData(const IrstSensorData& data) {
    return irstToJson(data).dump(2) + "\n";
}

bool writeIrstSensorDataFile(const IrstSensorData& data,
                              const std::string& path) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out << writeIrstSensorData(data);
    return out.good();
}

IrstDataResult loadIrstSensorDataFromString(const std::string& json) {
    return irstFromJsonString(json);
}

IrstDataResult loadIrstSensorData(const std::string& path) {
    IrstDataResult result;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        result.errors.push_back("cannot open " + path);
        return result;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return irstFromJsonString(ss.str());
}

// ---------------------------------------------------------------------------
// RWR JSON
// ---------------------------------------------------------------------------
namespace {

json rwrToJson(const RwrSensorData& data) {
    json j;
    j["kind"] = "f4.rwrdata";
    j["version"] = 1;
    json arr = json::array();
    for (const auto& e : data.sensors) {
        arr.push_back({
            {"name", e.name},
            {"az_limit_deg", e.data.az_limit_deg},
            {"el_limit_deg", e.data.el_limit_deg},
            {"sensitivity", e.data.sensitivity},
        });
    }
    j["sensors"] = std::move(arr);
    return j;
}

RwrDataResult rwrFromJsonString(const std::string& contents) {
    RwrDataResult result;
    json j = json::parse(contents, nullptr, false);
    if (j.is_discarded()) {
        result.errors.push_back("invalid JSON");
        return result;
    }
    if (readTag(j, "rwrdata", result.errors).empty() &&
        !result.errors.empty()) {
        return result;
    }
    for (const auto& je : j.value("sensors", json::array())) {
        RwrSensorEntry e;
        e.name = je.value("name", "");
        e.data.az_limit_deg = je.value("az_limit_deg", 180.0);
        e.data.el_limit_deg = je.value("el_limit_deg", 90.0);
        e.data.sensitivity = je.value("sensitivity", 1.0);
        result.data.sensors.push_back(std::move(e));
    }
    result.ok = true;
    return result;
}

} // namespace

std::string writeRwrSensorData(const RwrSensorData& data) {
    return rwrToJson(data).dump(2) + "\n";
}

bool writeRwrSensorDataFile(const RwrSensorData& data,
                             const std::string& path) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out << writeRwrSensorData(data);
    return out.good();
}

RwrDataResult loadRwrSensorDataFromString(const std::string& json) {
    return rwrFromJsonString(json);
}

RwrDataResult loadRwrSensorData(const std::string& path) {
    RwrDataResult result;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        result.errors.push_back("cannot open " + path);
        return result;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return rwrFromJsonString(ss.str());
}

// ---------------------------------------------------------------------------
// Visual JSON
// ---------------------------------------------------------------------------
namespace {

json visualToJson(const VisualSensorDataLibrary& data) {
    json j;
    j["kind"] = "f4.visualdata";
    j["version"] = 1;
    json arr = json::array();
    for (const auto& e : data.sensors) {
        arr.push_back({
            {"name", e.name},
            {"az_limit_deg", e.data.az_limit_deg},
            {"el_limit_deg", e.data.el_limit_deg},
            {"gain", e.data.gain},
            {"nominal_range_nm", e.data.nominal_range_nm()},
        });
    }
    j["sensors"] = std::move(arr);
    return j;
}

VisualDataResult visualFromJsonString(const std::string& contents) {
    VisualDataResult result;
    json j = json::parse(contents, nullptr, false);
    if (j.is_discarded()) {
        result.errors.push_back("invalid JSON");
        return result;
    }
    if (readTag(j, "visualdata", result.errors).empty() &&
        !result.errors.empty()) {
        return result;
    }
    for (const auto& je : j.value("sensors", json::array())) {
        VisualSensorEntry e;
        e.name = je.value("name", "");
        e.data.az_limit_deg = je.value("az_limit_deg", 181.0);
        e.data.el_limit_deg = je.value("el_limit_deg", 91.0);
        e.data.gain = je.value("gain", 3.7e9);
        result.data.sensors.push_back(std::move(e));
    }
    result.ok = true;
    return result;
}

} // namespace

std::string writeVisualSensorData(const VisualSensorDataLibrary& data) {
    return visualToJson(data).dump(2) + "\n";
}

bool writeVisualSensorDataFile(const VisualSensorDataLibrary& data,
                               const std::string& path) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out << writeVisualSensorData(data);
    return out.good();
}

VisualDataResult loadVisualSensorDataFromString(const std::string& json) {
    return visualFromJsonString(json);
}

VisualDataResult loadVisualSensorData(const std::string& path) {
    VisualDataResult result;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        result.errors.push_back("cannot open " + path);
        return result;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return visualFromJsonString(ss.str());
}

} // namespace f4::data
