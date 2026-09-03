// f4-convert/src/veh_parser.cpp
//
// Implementation of the Vehicle.lst / .veh parser (see veh_parser.hpp
// for the format contract and the reference quirks).

#include "f4/convert/veh_parser.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace f4::convert {

namespace fs = std::filesystem;

using f4::data::AircraftVehicleDef;
using f4::data::GroundVehicleDef;
using f4::data::HeloVehicleDef;
using f4::data::MoverType;
using f4::data::SensorSlot;
using f4::data::VehicleEntry;
using f4::data::WeaponVehicleDef;

namespace {

// Tokenizer with the reference's GetNext() semantics (identical to the
// mnvr/formation parsers): strip '#' line comments, split on whitespace.
std::vector<std::string> tokenize(const std::string& source) {
    std::vector<std::string> tokens;
    std::size_t i = 0;
    while (i < source.size()) {
        const char c = source[i];
        if (c == '#' || c == ';') {
            while (i < source.size() && source[i] != '\n') ++i;
        } else if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
        } else {
            std::string tok;
            while (i < source.size() &&
                   !std::isspace(static_cast<unsigned char>(source[i])) &&
                   source[i] != '#') {
                tok.push_back(source[i]);
                ++i;
            }
            tokens.push_back(std::move(tok));
        }
    }
    return tokens;
}

std::string lowercase(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

bool parseInt(const std::string& tok, long& out) {
    if (tok.empty()) return false;
    char* end = nullptr;
    out = std::strtol(tok.c_str(), &end, 10);
    return end != nullptr && *end == '\0';
}

bool parseDouble(const std::string& tok, double& out) {
    if (tok.empty()) return false;
    char* end = nullptr;
    out = std::strtod(tok.c_str(), &end);
    return end != nullptr && *end == '\0';
}

// Case-insensitive stem of a listed path: "Sim\VehDef\f16.veh" -> "f16".
std::string stemOf(const std::string& listedPath) {
    std::size_t slash = listedPath.find_last_of("/\\");
    const std::string base =
        slash == std::string::npos ? listedPath : listedPath.substr(slash + 1);
    std::size_t dot = base.find_last_of('.');
    return lowercase(dot == std::string::npos ? base : base.substr(0, dot));
}

// Case-insensitive file resolution inside `dir`, honoring the reference's
// Windows-style paths: "Sim\VehDef\f16.veh" resolves against dir/f16.veh.
// Returns an empty path when not found.
fs::path resolveVehPath(const std::string& vehDir, const std::string& listed) {
    if (vehDir.empty()) return {};
    std::size_t slash = listed.find_last_of("/\\");
    const std::string base =
        slash == std::string::npos ? listed : listed.substr(slash + 1);
    const std::string want = lowercase(base);
    std::error_code ec;
    for (fs::directory_iterator it(vehDir, ec), end; it != end && !ec;
         it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        if (lowercase(it->path().filename().string()) == want) {
            return it->path();
        }
    }
    return {};
}

struct VehFileParser {
    // Token cursor over one .veh file, mirroring the reference's
    // positional GetNext() reads (vehdef.cpp:96-252).
    std::vector<std::string> tokens;
    std::size_t pos = 0;
    std::vector<std::string>& errors;
    std::vector<std::string>& warnings;
    const std::string src;

    const std::string* next(const char* what) {
        if (pos >= tokens.size()) {
            errors.push_back(src + ": unexpected EOF while reading " + what);
            return nullptr;
        }
        return &tokens[pos++];
    }

    bool nextInt(const char* what, int& out) {
        const std::string* tok = next(what);
        if (tok == nullptr) return false;
        long v = 0;
        if (!parseInt(*tok, v)) {
            // Reference semantics: atoi("garbage") == 0 — the shipped
            // ALQxxx.veh (its "# Data Idx" block leads instead of
            // trailing) shifts every field and the reference silently
            // reads 0. We match the read and WARN (see the header).
            out = 0;
            warnings.push_back(src + ": " + what + " '" + *tok +
                               "' is not an integer — the reference's "
                               "atoi() reads 0 (kept)");
            return true;
        }
        out = static_cast<int>(v);
        return true;
    }

    bool nextDouble(const char* what, double& out) {
        const std::string* tok = next(what);
        if (tok == nullptr) return false;
        double v = 0;
        if (!parseDouble(*tok, v)) {
            // Same as nextInt: atof("garbage") == 0.
            out = 0.0;
            warnings.push_back(src + ": " + what + " '" + *tok +
                               "' is not a number — the reference's "
                               "atof() reads 0.0 (kept)");
            return true;
        }
        out = v;
        return true;
    }

    std::vector<SensorSlot> sensorPairs(const char* what, int count) {
        std::vector<SensorSlot> out;
        for (int i = 0; i < count; ++i) {
            SensorSlot s;
            if (!nextInt(what, s.type)) break;
            const std::string idxWhat = std::string(what) + " index";
            if (!nextInt(idxWhat.c_str(), s.index)) break;
            if (s.type < 0 ||
                s.type >= static_cast<int>(f4::data::kNumSensorTypes)) {
                warnings.push_back(
                    src + ": " + what + " type " + std::to_string(s.type) +
                    " outside the SensorType enum (kept verbatim)");
            }
            out.push_back(s);
        }
        return out;
    }
};

void parseVehBody(VehFileParser& p, MoverType type, VehicleEntry& entry) {
    switch (type) {
        case MoverType::Aircraft: {
            // vehdef.cpp:96-159 — combat class, airframe index, player
            // sensors, AI sensors.
            AircraftVehicleDef d;
            if (!p.nextInt("combat class", d.combat_class)) return;
            if (!p.nextInt("airframe index", d.airframe_index)) return;
            int nPlayer = 0;
            if (!p.nextInt("player sensor count", nPlayer)) return;
            if (nPlayer < 0 || nPlayer > 64) {
                p.errors.push_back(p.src + ": implausible player sensor count " +
                                   std::to_string(nPlayer));
                return;
            }
            d.player_sensors = p.sensorPairs("player sensor", nPlayer);
            int nAI = 0;
            if (!p.nextInt("AI sensor count", nAI)) return;
            if (nAI < 0 || nAI > 64) {
                p.errors.push_back(p.src + ": implausible AI sensor count " +
                                   std::to_string(nAI));
                return;
            }
            d.ai_sensors = p.sensorPairs("AI sensor", nAI);
            entry.def = std::move(d);
            break;
        }
        case MoverType::Helicopter: {
            // vehdef.cpp:186-218 — airframe index, sensors.
            HeloVehicleDef d;
            if (!p.nextInt("airframe index", d.airframe_index)) return;
            int n = 0;
            if (!p.nextInt("sensor count", n)) return;
            if (n < 0 || n > 64) {
                p.errors.push_back(p.src + ": implausible sensor count " +
                                   std::to_string(n));
                return;
            }
            d.sensors = p.sensorPairs("sensor", n);
            entry.def = std::move(d);
            break;
        }
        case MoverType::Ground: {
            // vehdef.cpp:220-252 — sensors only.
            GroundVehicleDef d;
            int n = 0;
            if (!p.nextInt("sensor count", n)) return;
            if (n < 0 || n > 64) {
                p.errors.push_back(p.src + ": implausible sensor count " +
                                   std::to_string(n));
                return;
            }
            d.sensors = p.sensorPairs("sensor", n);
            entry.def = std::move(d);
            break;
        }
        case MoverType::Weapon: {
            // vehdef.cpp:161-184 — the physical card.
            WeaponVehicleDef d;
            if (!p.nextInt("flags", d.flags)) return;
            if (!p.nextDouble("cd", d.cd)) return;
            if (!p.nextDouble("weight", d.weight)) return;
            if (!p.nextDouble("area", d.area)) return;
            if (!p.nextDouble("x ejection", d.x_ejection)) return;
            if (!p.nextDouble("y ejection", d.y_ejection)) return;
            if (!p.nextDouble("z ejection", d.z_ejection)) return;
            const std::string* mn = p.next("mnemonic");
            if (mn == nullptr) return;
            d.mnemonic = *mn;
            if (!p.nextInt("weapon class", d.weapon_class)) return;
            if (!p.nextInt("domain", d.domain)) return;
            if (!p.nextInt("weapon type", d.weapon_type)) return;
            if (!p.nextInt("data idx", d.data_idx)) return;
            if (d.cd < 0.0 || d.weight < 0.0 || d.area < 0.0) {
                p.errors.push_back(p.src + ": negative physical values (cd=" +
                                   std::to_string(d.cd) + " weight=" +
                                   std::to_string(d.weight) + " area=" +
                                   std::to_string(d.area) + ")");
                return;
            }
            entry.def = std::move(d);
            break;
        }
        default:
            break;
    }
    if (p.errors.empty() && p.pos < p.tokens.size()) {
        p.warnings.push_back(p.src + ": " + std::to_string(p.tokens.size() - p.pos) +
                             " trailing token(s) after the definition (ignored)");
    }
}

VehParseResult parseVehTokens(std::vector<std::string> tokens, MoverType type,
                              const std::string& src) {
    VehParseResult result;
    VehicleEntry entry;
    entry.type = type;
    entry.name = stemOf(src);
    entry.file = src;
    VehFileParser p{std::move(tokens), 0, result.errors, result.warnings, src};
    parseVehBody(p, type, entry);
    if (result.errors.empty()) {
        result.library.entries.push_back(std::move(entry));
        result.ok = true;
    }
    return result;
}

std::string readFileToString(const std::string& path, bool& ok) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        ok = false;
        return {};
    }
    std::stringstream ss;
    ss << in.rdbuf();
    ok = true;
    return ss.str();
}

} // namespace

VehParseResult loadVehString(const std::string& contents, MoverType type,
                             const std::string& sourceName) {
    return parseVehTokens(tokenize(contents), type, sourceName);
}

VehParseResult loadVehFile(const std::string& vehPath, MoverType type) {
    bool ok = false;
    const std::string contents = readFileToString(vehPath, ok);
    if (!ok) {
        VehParseResult result;
        result.errors.push_back("cannot open " + vehPath);
        return result;
    }
    return loadVehString(contents, type, vehPath);
}

VehParseResult loadVehicleLstString(const std::string& lstContents,
                                    const std::string& vehDir,
                                    const std::string& sourceName) {
    VehParseResult result;
    const auto tokens = tokenize(lstContents);
    if (tokens.empty()) {
        result.errors.push_back(sourceName + ": no tokens (empty file?)");
        return result;
    }

    std::size_t pos = 0;
    long count = 0;
    if (!parseInt(tokens[pos], count)) {
        result.errors.push_back(sourceName + ": row count '" + tokens[pos] +
                                "' is not an integer");
        return result;
    }
    ++pos;
    if (count < 0 || count > 100000) {
        result.errors.push_back(sourceName + ": implausible row count " +
                                std::to_string(count));
        return result;
    }

    std::unordered_map<std::string, std::size_t> seenNames;
    for (long row = 0; row < count; ++row) {
        if (pos + 2 > tokens.size()) {
            result.errors.push_back(sourceName + ": unexpected EOF at row " +
                                    std::to_string(row + 1) + " (want " +
                                    std::to_string(count) + " rows)");
            return result;
        }
        long typeVal = 0;
        if (!parseInt(tokens[pos], typeVal)) {
            result.errors.push_back(
                sourceName + ": row " + std::to_string(row + 1) +
                " type '" + tokens[pos] + "' is not an integer");
            return result;
        }
        const std::string file = tokens[pos + 1];
        pos += 2;
        if (typeVal < -1 || typeVal > 4) {
            result.warnings.push_back(
                sourceName + ": row " + std::to_string(row + 1) + " type " +
                std::to_string(typeVal) + " outside MoverType (treated as "
                "Sea/default — matches the reference's default branch)");
            typeVal = 4;
        }

        VehicleEntry entry;
        entry.type = static_cast<MoverType>(typeVal);
        entry.file = file;
        entry.name = stemOf(file);

        if (entry.has_definition()) {
            const fs::path resolved = resolveVehPath(vehDir, file);
            if (resolved.empty()) {
                result.errors.push_back(
                    sourceName + ": row " + std::to_string(row + 1) +
                    " references '" + file + "' which does not exist in '" +
                    vehDir + "'");
                return result;
            }
            bool ok = false;
            const std::string vehContents =
                readFileToString(resolved.string(), ok);
            if (!ok) {
                result.errors.push_back("cannot open " + resolved.string());
                return result;
            }
            const auto vehTokens = tokenize(vehContents);
            VehFileParser p{vehTokens, 0, result.errors, result.warnings,
                            resolved.string()};
            parseVehBody(p, entry.type, entry);
            if (!result.errors.empty()) return result;
        }
        // Sea / Unused rows: the filename is recorded and never opened
        // (vehdef.cpp:74-78 — including the shipped "dpthchrg,veh" typo
        // row, which is a Sea row and therefore harmless).

        if (entry.name == "unused") {
            // The "-1 unused" filler row: keep the literal name; it is
            // never a lookup target.
        } else if (const auto it = seenNames.find(entry.name);
                   it != seenNames.end()) {
            result.warnings.push_back(
                sourceName + ": duplicate stem '" + entry.name + "' (rows " +
                std::to_string(it->second + 1) + " and " +
                std::to_string(row + 1) + " — find() returns the first)");
        } else {
            seenNames.emplace(entry.name, row);
        }
        result.library.entries.push_back(std::move(entry));
    }

    if (result.errors.empty()) {
        if (pos < tokens.size()) {
            result.warnings.push_back(
                sourceName + ": " + std::to_string(tokens.size() - pos) +
                " trailing token(s) after the last row (ignored)");
        }
        result.ok = true;
    }
    return result;
}

VehParseResult loadVehicleLstFile(const std::string& lstPath) {
    VehParseResult result;
    bool ok = false;
    const std::string contents = readFileToString(lstPath, ok);
    if (!ok) {
        result.errors.push_back("cannot open " + lstPath);
        return result;
    }
    const std::string dir = fs::path(lstPath).parent_path().string();
    return loadVehicleLstString(contents, dir, lstPath);
}

} // namespace f4::convert
