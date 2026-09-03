// f4-data/src/maneuver_data.cpp
//
// ManeuverData struct methods + canonical JSON serialization.

#include "f4/data/maneuver_data.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace f4::data {

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Struct methods
// ---------------------------------------------------------------------------

bool ManeuverData::classCan(std::size_t ownClass,
                            MnvrClassFlags flag) const noexcept {
    if (ownClass >= kNumMnvrClasses) return false;
    return (classFlags[ownClass] &
            static_cast<std::uint32_t>(flag)) != 0u;
}

const ManeuverChoice* ManeuverData::choice(
    std::size_t own, std::size_t opposing) const noexcept {
    if (own >= kNumMnvrClasses || opposing >= kNumMnvrClasses) return nullptr;
    return &table[own][opposing];
}

std::size_t ManeuverData::populatedCells() const noexcept {
    std::size_t n = 0;
    for (const auto& row : table) {
        for (const auto& cell : row) {
            if (!cell.intercepts.empty() || !cell.merges.empty() ||
                !cell.spikeReacts.empty()) {
                ++n;
            }
        }
    }
    return n;
}

// ---------------------------------------------------------------------------
// JSON (canonical format: f4.mnvrdata, version 1)
//
// {
//   "kind": "f4.mnvrdata", "version": 1,
//   "classes": ["F4", ...],                    // advisory names
//   "classFlags": [1828, ...],                 // raw ACMnverClassFlags bits
//   "table": [ [ { "intercepts": [0,1],        // 0-based enum indices
//                  "merges": [1,2],
//                  "reacts": [1,2] }, ... 9 ], ... 9 rows ]
// }
// ---------------------------------------------------------------------------

json toJson(const ManeuverData& md) {
    json j;
    j["kind"] = "f4.mnvrdata";
    j["version"] = 1;
    j["classes"] = std::vector<std::string>(
        std::begin(kMnvrClassNames), std::end(kMnvrClassNames));
    std::vector<std::uint32_t> flags(std::begin(md.classFlags),
                                     std::end(md.classFlags));
    j["classFlags"] = flags;
    json rows = json::array();
    for (const auto& row : md.table) {
        json cells = json::array();
        for (const auto& cell : row) {
            json c;
            c["intercepts"] = cell.intercepts;
            c["merges"] = cell.merges;
            c["reacts"] = cell.spikeReacts;
            cells.push_back(std::move(c));
        }
        rows.push_back(std::move(cells));
    }
    j["table"] = std::move(rows);
    return j;
}

std::string writeManeuverData(const ManeuverData& md) {
    return toJson(md).dump(2) + "\n";
}

bool writeManeuverDataFile(const ManeuverData& md, const std::string& path) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out << writeManeuverData(md);
    return out.good();
}

ManeuverDataResult loadManeuverDataFromString(const std::string& text) {
    ManeuverDataResult result;
    json j;
    try {
        j = json::parse(text);
    } catch (const json::parse_error& e) {
        result.errors.push_back(std::string("JSON parse error: ") + e.what());
        return result;
    }
    if (!j.is_object()) {
        result.errors.push_back("root is not an object");
        return result;
    }
    if (j.value("kind", std::string()) != "f4.mnvrdata") {
        result.errors.push_back("kind != \"f4.mnvrdata\"");
        return result;
    }
    const int version = j.value("version", 0);
    if (version != 1) {
        result.warnings.push_back("unknown version " + std::to_string(version) +
                                  ", reading as version 1");
    }

    ManeuverData& md = result.data;
    if (!j.contains("classFlags") || !j["classFlags"].is_array()) {
        result.errors.push_back("classFlags missing or not an array");
        return result;
    }
    const auto flags = j["classFlags"];
    if (flags.size() != kNumMnvrClasses) {
        result.errors.push_back("classFlags has " +
                                std::to_string(flags.size()) +
                                " entries, expected " +
                                std::to_string(kNumMnvrClasses));
        return result;
    }
    for (std::size_t i = 0; i < kNumMnvrClasses; ++i) {
        md.classFlags[i] = flags[i].get<std::uint32_t>();
    }

    if (!j.contains("table") || !j["table"].is_array()) {
        result.errors.push_back("table missing or not an array");
        return result;
    }
    const auto rows = j["table"];
    if (rows.size() != kNumMnvrClasses) {
        result.errors.push_back("table has " + std::to_string(rows.size()) +
                                " rows, expected " +
                                std::to_string(kNumMnvrClasses));
        return result;
    }
    for (std::size_t i = 0; i < kNumMnvrClasses; ++i) {
        if (!rows[i].is_array() || rows[i].size() != kNumMnvrClasses) {
            result.errors.push_back("table row " + std::to_string(i) +
                                    " is not a 9-cell array");
            return result;
        }
        for (std::size_t k = 0; k < kNumMnvrClasses; ++k) {
            const auto& c = rows[i][k];
            const auto readList = [&](const char* key,
                                      std::vector<int>& out) {
                out.clear();
                if (c.contains(key) && c[key].is_array()) {
                    for (const auto& v : c[key]) out.push_back(v.get<int>());
                } else {
                    result.warnings.push_back(
                        std::string("table[") + std::to_string(i) + "][" +
                        std::to_string(k) + "]." + key + " missing");
                }
            };
            readList("intercepts", md.table[i][k].intercepts);
            readList("merges", md.table[i][k].merges);
            readList("reacts", md.table[i][k].spikeReacts);
        }
    }

    result.ok = true;
    return result;
}

ManeuverDataResult loadManeuverData(const std::string& path) {
    ManeuverDataResult result;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        result.errors.push_back("cannot open " + path);
        return result;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    auto inner = loadManeuverDataFromString(ss.str());
    if (!inner.ok && !inner.errors.empty()) {
        inner.errors.front() = path + ": " + inner.errors.front();
    }
    return inner;
}

} // namespace f4::data
