// f4-data/src/formation_data.cpp
//
// FormationLibrary struct methods + canonical JSON serialization.

#include "f4/data/formation_data.hpp"

#include <nlohmann/json.hpp>

#include <cctype>
#include <fstream>
#include <sstream>

namespace f4::data {

using json = nlohmann::json;

namespace {

std::string canonicalName(std::string s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == ' ' || c == '_' || c == '-') continue;
        out.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

json slotToJson(const FormationSlot& s) {
    json j;
    j["relAzDeg"] = s.rel_az_deg;
    j["relElDeg"] = s.rel_el_deg;
    j["rangeNm"] = s.range_nm;
    j["formNum"] = s.form_num;
    return j;
}

FormationSlot slotFromJson(const json& j, int defaultFormNum) {
    FormationSlot s;
    s.rel_az_deg = j.value("relAzDeg", 0.0);
    s.rel_el_deg = j.value("relElDeg", 0.0);
    s.range_nm = j.value("rangeNm", 0.0);
    s.form_num = j.value("formNum", defaultFormNum);
    return s;
}

} // namespace

const Formation* FormationLibrary::find_by_form_num(
    int formNum) const noexcept {
    for (const auto& f : formations) {
        if (f.form_num == formNum) return &f;
    }
    return nullptr;
}

const Formation* FormationLibrary::find_by_name(
    const std::string& name) const noexcept {
    const std::string want = canonicalName(name);
    for (const auto& f : formations) {
        if (canonicalName(f.name) == want) return &f;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// JSON (canonical format: f4.formdata, version 1)
//
// {
//   "kind": "f4.formdata", "version": 1,
//   "formations": [
//     { "name": "spread", "formNum": 0,
//       "slots": [ { "relAzDeg": -90.0, "relElDeg": 0.0, "rangeNm": 0.5 } ],
//       "twoShip": { ... },
//       "twoShipExplicit": false }
//   ]
// }
// ---------------------------------------------------------------------------

json toJson(const FormationLibrary& lib) {
    json j;
    j["kind"] = "f4.formdata";
    j["version"] = 1;
    json arr = json::array();
    for (const auto& f : lib.formations) {
        json fj;
        fj["name"] = f.name;
        fj["formNum"] = f.form_num;
        json sarr = json::array();
        for (const auto& s : f.slots) sarr.push_back(slotToJson(s));
        fj["slots"] = std::move(sarr);
        fj["twoShip"] = slotToJson(f.two_ship);
        fj["twoShipExplicit"] = f.two_ship_explicit;
        arr.push_back(std::move(fj));
    }
    j["formations"] = std::move(arr);
    return j;
}

std::string writeFormationLibrary(const FormationLibrary& lib) {
    return toJson(lib).dump(2) + "\n";
}

bool writeFormationLibraryFile(const FormationLibrary& lib,
                               const std::string& path) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out << writeFormationLibrary(lib);
    return out.good();
}

FormationLibraryResult loadFormationLibraryFromString(
    const std::string& text) {
    FormationLibraryResult result;
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
    if (j.value("kind", std::string()) != "f4.formdata") {
        result.errors.push_back("kind != \"f4.formdata\"");
        return result;
    }
    const int version = j.value("version", 0);
    if (version != 1) {
        result.warnings.push_back("unknown version " + std::to_string(version) +
                                  ", reading as version 1");
    }
    if (!j.contains("formations") || !j["formations"].is_array()) {
        result.errors.push_back("formations missing or not an array");
        return result;
    }

    FormationLibrary& lib = result.data;
    for (const auto& fj : j["formations"]) {
        if (!fj.is_object() || !fj.contains("name")) {
            result.errors.push_back("formation entry missing name");
            return result;
        }
        Formation f;
        f.name = fj["name"].get<std::string>();
        f.form_num = fj.value("formNum", 0);
        if (fj.contains("slots") && fj["slots"].is_array()) {
            for (const auto& sj : fj["slots"]) {
                f.slots.push_back(slotFromJson(sj, f.form_num));
            }
        }
        if (f.slots.empty()) {
            result.warnings.push_back("formation '" + f.name +
                                      "' has no slots");
        }
        if (fj.contains("twoShip")) {
            f.two_ship = slotFromJson(fj["twoShip"], f.form_num);
            f.two_ship_explicit = fj.value("twoShipExplicit", true);
        } else if (!f.slots.empty()) {
            // formdata.cpp:85-91: no dedicated 2-ship triple -> slot[0]
            f.two_ship = f.slots.front();
            f.two_ship_explicit = false;
        }
        lib.formations.push_back(std::move(f));
    }
    if (lib.formations.empty()) {
        result.errors.push_back("no formations");
        return result;
    }
    result.ok = true;
    return result;
}

FormationLibraryResult loadFormationLibrary(const std::string& path) {
    FormationLibraryResult result;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        result.errors.push_back("cannot open " + path);
        return result;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    auto inner = loadFormationLibraryFromString(ss.str());
    if (!inner.ok && !inner.errors.empty()) {
        inner.errors.front() = path + ": " + inner.errors.front();
    }
    return inner;
}

} // namespace f4::data
