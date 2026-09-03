// f4-data/src/brain_data.cpp
//
// BrainData struct methods (tolerant label matching) + canonical JSON
// serialization.

#include "f4/data/brain_data.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace f4::data {

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Label matching: case-insensitive, spaces/underscores/hyphens ignored.
// ("GroundMnvr" == "ground mnvr" == "GROUND_MNVR"; the tag row
// "Defensive Modes - This is a tag" collapses to "defensivemodesthisisatag",
// so callers matching "Defensive" must use the startsWith form below.)
// ---------------------------------------------------------------------------

namespace {

std::string canonicalLabel(std::string s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == ' ' || c == '_' || c == '-' || c == '\t') continue;
        out.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

const char* keyLabel(BrainModeKey key) {
    switch (key) {
        case BrainModeKey::GroundAvoid:   return "GroundAvoidMode";
        case BrainModeKey::CollisionAvoid:return "CollisionAvoidMode";
        case BrainModeKey::GunsJink:      return "GunsJinkMode";
        case BrainModeKey::MissileDefeat: return "MissileDefeatMode";
        case BrainModeKey::Defensive:     return "Defensive Modes";
        case BrainModeKey::FollowOrders:  return "FollowOrdersMode";
        case BrainModeKey::Landing:       return "Landing Mode";
        case BrainModeKey::Accelerate:    return "AccelerateMode";
        case BrainModeKey::Merge:         return "MergeMode";
        case BrainModeKey::MissileEngage: return "MissileEngageMode";
        case BrainModeKey::GunsEngage:    return "GunsEngageMode";
        case BrainModeKey::Roop:          return "RoopMode";
        case BrainModeKey::OverB:         return "OverBMode";
        case BrainModeKey::Overshoot:     return "OvershootMode";
        case BrainModeKey::WVREngage:     return "WVREngageMode";
        case BrainModeKey::BVREngage:     return "BVREngageMode";
        case BrainModeKey::RunAway:       return "RunAwayMode";
        case BrainModeKey::Loiter:        return "LoiterMode";
        case BrainModeKey::Separate:      return "SeparateMode";
        case BrainModeKey::RTB:           return "RTBMode";
        case BrainModeKey::Wingy:         return "WingyMode";
        case BrainModeKey::Bugout:        return "BugoutMode";
        case BrainModeKey::Waypoint:      return "WaypointMode";
        case BrainModeKey::GroundMnvr:    return "GroundMnvr";
        case BrainModeKey::LastValid:     return "LastValidMode";
    }
    return "";
}

} // namespace

const BrainModeRow* BrainArchetype::find_mode(
    BrainModeKey key) const noexcept {
    // Exact canonical match first.
    const std::string want = canonicalLabel(keyLabel(key));
    for (const auto& m : modes) {
        if (canonicalLabel(m.label) == want) return &m;
    }
    // Prefix fallback: the tag row "Defensive Modes - This is a tag"
    // canonicalizes to "defensivemodesthisisatag", which STARTS WITH the
    // Defensive key's "defensivemodes". Only reached when no exact match
    // exists, so full-name keys cannot false-positive against each other.
    for (const auto& m : modes) {
        const std::string have = canonicalLabel(m.label);
        if (have.size() > want.size() && have.compare(0, want.size(), want) == 0) {
            return &m;
        }
    }
    return nullptr;
}

bool BrainArchetype::mode_enabled(BrainModeKey key) const noexcept {
    const auto* m = find_mode(key);
    return m != nullptr && m->enabled != 0;
}

const BrainArchetype* BrainData::find_archetype(
    const std::string& name) const noexcept {
    const std::string want = canonicalLabel(name);
    for (const auto& a : archetypes) {
        if (canonicalLabel(a.name) == want) return &a;
    }
    return nullptr;
}

const BrainArchetype* BrainData::generic() const noexcept {
    const auto* g = find_archetype("Generic");
    return (g != nullptr) ? g : (archetypes.empty() ? nullptr
                                                    : &archetypes.front());
}

// ---------------------------------------------------------------------------
// JSON (canonical format: f4.braindata, version 1)
//
// {
//   "kind": "f4.braindata", "version": 1,
//   "archetypes": [
//     { "name": "Generic",
//       "modes": [ { "label": "GroundAvoidMode", "enabled": 1,
//                    "priority": 0.5, "rangeFt": 0.0, "angleDeg": 0.0,
//                    "row": 0 }, ... ] }
//   ]
// }
// ---------------------------------------------------------------------------

json toJson(const BrainData& bd) {
    json j;
    j["kind"] = "f4.braindata";
    j["version"] = 1;
    json arr = json::array();
    for (const auto& a : bd.archetypes) {
        json aj;
        aj["name"] = a.name;
        json marr = json::array();
        for (const auto& m : a.modes) {
            json mj;
            mj["label"] = m.label;
            mj["enabled"] = m.enabled;
            mj["priority"] = m.priority;
            mj["rangeFt"] = m.range_ft;
            mj["angleDeg"] = m.angle_deg;
            mj["row"] = m.row;
            marr.push_back(std::move(mj));
        }
        aj["modes"] = std::move(marr);
        arr.push_back(std::move(aj));
    }
    j["archetypes"] = std::move(arr);
    return j;
}

std::string writeBrainData(const BrainData& bd) {
    return toJson(bd).dump(2) + "\n";
}

bool writeBrainDataFile(const BrainData& bd, const std::string& path) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out << writeBrainData(bd);
    return out.good();
}

BrainDataResult loadBrainDataFromString(const std::string& text) {
    BrainDataResult result;
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
    if (j.value("kind", std::string()) != "f4.braindata") {
        result.errors.push_back("kind != \"f4.braindata\"");
        return result;
    }
    const int version = j.value("version", 0);
    if (version != 1) {
        result.warnings.push_back("unknown version " + std::to_string(version) +
                                  ", reading as version 1");
    }
    if (!j.contains("archetypes") || !j["archetypes"].is_array()) {
        result.errors.push_back("archetypes missing or not an array");
        return result;
    }

    BrainData& bd = result.data;
    for (const auto& aj : j["archetypes"]) {
        if (!aj.is_object() || !aj.contains("name")) {
            result.errors.push_back("archetype entry missing name");
            return result;
        }
        BrainArchetype a;
        a.name = aj["name"].get<std::string>();
        if (aj.contains("modes") && aj["modes"].is_array()) {
            for (const auto& mj : aj["modes"]) {
                BrainModeRow m;
                m.label = mj.value("label", std::string());
                m.enabled = mj.value("enabled", 0);
                m.priority = mj.value("priority", 0.0);
                m.range_ft = mj.value("rangeFt", 0.0);
                m.angle_deg = mj.value("angleDeg", 0.0);
                m.row = mj.value("row", a.modes.size());
                a.modes.push_back(std::move(m));
            }
        } else {
            result.warnings.push_back("archetype '" + a.name +
                                      "' has no modes array");
        }
        bd.archetypes.push_back(std::move(a));
    }
    if (bd.archetypes.empty()) {
        result.errors.push_back("no archetypes");
        return result;
    }
    result.ok = true;
    return result;
}

BrainDataResult loadBrainData(const std::string& path) {
    BrainDataResult result;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        result.errors.push_back("cannot open " + path);
        return result;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    auto inner = loadBrainDataFromString(ss.str());
    if (!inner.ok && !inner.errors.empty()) {
        inner.errors.front() = path + ": " + inner.errors.front();
    }
    return inner;
}

} // namespace f4::data
