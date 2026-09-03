// f4-data/src/vehicle_def_data.cpp
//
// VehicleDefinitionLibrary lookups + canonical JSON serialization.

#include "f4/data/vehicle_def_data.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
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

json sensorSlotsToJson(const std::vector<SensorSlot>& slots) {
    json arr = json::array();
    for (const auto& s : slots) {
        arr.push_back({{"type", s.type}, {"index", s.index}});
    }
    return arr;
}

std::vector<SensorSlot> sensorSlotsFromJson(const json& arr) {
    std::vector<SensorSlot> out;
    if (!arr.is_array()) return out;
    for (const auto& j : arr) {
        SensorSlot s;
        s.type = j.value("type", 0);
        s.index = j.value("index", 0);
        out.push_back(s);
    }
    return out;
}

json entryToJson(const VehicleEntry& e) {
    json j;
    j["type"] = static_cast<int>(e.type);
    j["type_name"] = e.type == MoverType::Unused
                         ? "Unused"
                         : kMoverTypeNames[static_cast<int>(e.type)];
    j["file"] = e.file;
    j["name"] = e.name;
    if (!e.has_definition()) {
        j["definition"] = nullptr;
        return j;
    }
    switch (e.type) {
        case MoverType::Aircraft: {
            const auto* d = e.aircraft();
            j["definition"] = {
                {"combat_class", d->combat_class},
                {"combat_class_name",
                 d->combat_class >= 0 &&
                         d->combat_class <
                             static_cast<int>(kNumCombatClasses)
                     ? kCombatClassNames[d->combat_class]
                     : ""},
                {"airframe_index", d->airframe_index},
                {"player_sensors", sensorSlotsToJson(d->player_sensors)},
                {"ai_sensors", sensorSlotsToJson(d->ai_sensors)},
            };
            break;
        }
        case MoverType::Helicopter: {
            const auto* d = e.helo();
            j["definition"] = {
                {"airframe_index", d->airframe_index},
                {"sensors", sensorSlotsToJson(d->sensors)},
            };
            break;
        }
        case MoverType::Ground: {
            const auto* d = e.ground();
            j["definition"] = {{"sensors", sensorSlotsToJson(d->sensors)}};
            break;
        }
        case MoverType::Weapon: {
            const auto* d = e.weapon();
            j["definition"] = {
                {"flags", d->flags},
                {"cd", d->cd},
                {"weight", d->weight},
                {"area", d->area},
                {"x_ejection", d->x_ejection},
                {"y_ejection", d->y_ejection},
                {"z_ejection", d->z_ejection},
                {"mnemonic", d->mnemonic},
                {"weapon_class", d->weapon_class},
                {"weapon_class_name",
                 d->weapon_class >= 0 &&
                         d->weapon_class <
                             static_cast<int>(kNumWeaponClasses)
                     ? kWeaponClassNames[d->weapon_class]
                     : ""},
                {"domain", d->domain},
                {"weapon_type", d->weapon_type},
                {"weapon_type_name",
                 d->weapon_type >= 0 &&
                         d->weapon_type < static_cast<int>(kNumWeaponTypes)
                     ? kWeaponTypeName[d->weapon_type]
                     : ""},
                {"data_idx", d->data_idx},
            };
            break;
        }
        default:
            j["definition"] = nullptr;
            break;
    }
    return j;
}

VehicleEntry entryFromJson(const json& j) {
    VehicleEntry e;
    e.type = static_cast<MoverType>(j.value("type", 4));
    e.file = j.value("file", "");
    e.name = j.value("name", "");
    const auto& d = j.value("definition", json(nullptr));
    if (!d.is_null() && e.has_definition()) {
        switch (e.type) {
            case MoverType::Aircraft: {
                AircraftVehicleDef a;
                a.combat_class = d.value("combat_class", 0);
                a.airframe_index = d.value("airframe_index", 0);
                a.player_sensors = sensorSlotsFromJson(d.value("player_sensors", json::array()));
                a.ai_sensors = sensorSlotsFromJson(d.value("ai_sensors", json::array()));
                e.def = std::move(a);
                break;
            }
            case MoverType::Helicopter: {
                HeloVehicleDef h;
                h.airframe_index = d.value("airframe_index", 0);
                h.sensors = sensorSlotsFromJson(d.value("sensors", json::array()));
                e.def = std::move(h);
                break;
            }
            case MoverType::Ground: {
                GroundVehicleDef g;
                g.sensors = sensorSlotsFromJson(d.value("sensors", json::array()));
                e.def = std::move(g);
                break;
            }
            case MoverType::Weapon: {
                WeaponVehicleDef w;
                w.flags = d.value("flags", 0);
                w.cd = d.value("cd", 0.0);
                w.weight = d.value("weight", 0.0);
                w.area = d.value("area", 0.0);
                w.x_ejection = d.value("x_ejection", 0.0);
                w.y_ejection = d.value("y_ejection", 0.0);
                w.z_ejection = d.value("z_ejection", 0.0);
                w.mnemonic = d.value("mnemonic", "");
                w.weapon_class = d.value("weapon_class", 0);
                w.domain = d.value("domain", 0);
                w.weapon_type = d.value("weapon_type", 0);
                w.data_idx = d.value("data_idx", 0);
                e.def = std::move(w);
                break;
            }
            default:
                break;
        }
    }
    return e;
}

} // namespace

const VehicleEntry* VehicleDefinitionLibrary::find(
    std::string_view name) const noexcept {
    const std::string want = canonicalName(name);
    for (const auto& e : entries) {
        if (e.name == want) return &e;
    }
    return nullptr;
}

std::size_t VehicleDefinitionLibrary::count_of_type(
    MoverType type) const noexcept {
    return static_cast<std::size_t>(std::count_if(
        entries.begin(), entries.end(),
        [type](const VehicleEntry& e) { return e.type == type; }));
}

std::string writeVehicleDefinitionLibrary(
    const VehicleDefinitionLibrary& lib) {
    json j;
    j["kind"] = "f4.vehdef";
    j["version"] = 1;
    json arr = json::array();
    for (const auto& e : lib.entries) arr.push_back(entryToJson(e));
    j["entries"] = std::move(arr);
    return j.dump(2) + "\n";
}

bool writeVehicleDefinitionLibraryFile(const VehicleDefinitionLibrary& lib,
                                       const std::string& path) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out << writeVehicleDefinitionLibrary(lib);
    return out.good();
}

VehicleDefLibraryResult loadVehicleDefinitionLibraryFromString(
    const std::string& contents) {
    VehicleDefLibraryResult result;
    json j = json::parse(contents, nullptr, false);
    if (j.is_discarded()) {
        result.errors.push_back("invalid JSON");
        return result;
    }
    // Tag check with the same prefix fallback as the other SimData
    // loaders (f4.vehdef v1; "vehdef" alone also accepted).
    const std::string kind = j.value("kind", "");
    if (kind != "f4.vehdef" && kind != "vehdef") {
        result.errors.push_back("wrong kind tag: '" + kind + "'");
        return result;
    }
    const auto& arr = j.value("entries", json::array());
    if (!arr.is_array()) {
        result.errors.push_back("entries is not an array");
        return result;
    }
    for (const auto& je : arr) {
        result.library.entries.push_back(entryFromJson(je));
    }
    result.ok = true;
    return result;
}

VehicleDefLibraryResult loadVehicleDefinitionLibrary(
    const std::string& path) {
    VehicleDefLibraryResult result;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        result.errors.push_back("cannot open " + path);
        return result;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return loadVehicleDefinitionLibraryFromString(ss.str());
}

} // namespace f4::data
