// f4-convert/src/sensor_parser.cpp
//
// Implementation of the SENSDATA text-file parser (see sensor_parser.hpp
// for the format contract).

#include "f4/convert/sensor_parser.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace f4::convert {

namespace fs = std::filesystem;

namespace {

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

bool parseDouble(const std::string& tok, double& out) {
    if (tok.empty()) return false;
    char* end = nullptr;
    out = std::strtod(tok.c_str(), &end);
    return end != nullptr && *end == '\0';
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

fs::path resolveInDir(const std::string& dir, const std::string& name) {
    if (dir.empty()) return {};
    const std::string want = lowercase(name);
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; it != end && !ec;
         it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        if (lowercase(it->path().filename().string()) == want) {
            return it->path();
        }
    }
    return {};
}

/// The .LST contract: one count token, then that many file-name tokens.
/// Extra tokens warn (matches the family style of the other parsers).
std::vector<std::string> parseListFile(const std::string& lstContents,
                                       const std::string& sourceName,
                                       std::vector<std::string>& errors,
                                       std::vector<std::string>& warnings) {
    const auto tokens = tokenize(lstContents);
    if (tokens.empty()) {
        errors.push_back(sourceName + ": no tokens (empty file?)");
        return {};
    }
    long count = 0;
    char* end = nullptr;
    count = std::strtol(tokens[0].c_str(), &end, 10);
    if (end == nullptr || *end != '\0') {
        errors.push_back(sourceName + ": count '" + tokens[0] +
                         "' is not an integer");
        return {};
    }
    if (count < 0 || count > 4096) {
        errors.push_back(sourceName + ": implausible count " +
                         std::to_string(count));
        return {};
    }
    if (tokens.size() < 1 + static_cast<std::size_t>(count)) {
        errors.push_back(sourceName + ": expected " + std::to_string(count) +
                         " entries, found " +
                         std::to_string(tokens.size() - 1));
        return {};
    }
    if (tokens.size() > 1 + static_cast<std::size_t>(count)) {
        warnings.push_back(
            sourceName + ": " +
            std::to_string(tokens.size() - 1 - static_cast<std::size_t>(count)) +
            " trailing token(s) after the list (ignored)");
    }
    return {tokens.begin() + 1, tokens.begin() + 1 + count};
}

/// Read the 5-value sensor body shared by the IRST family (RWR/visual
/// files carry 3 values and parse inline). `fields` names the values
/// for error messages.
bool parseSensorBody(const std::string& contents, const std::string& src,
                     const char* const (&fields)[5],
                     double (&out)[5],
                     std::vector<std::string>& errors,
                     std::vector<std::string>& warnings) {
    const auto tokens = tokenize(contents);
    if (tokens.size() < 5) {
        errors.push_back(src + ": expected 5 values (" +
                         std::string(fields[0]) + ", " + fields[1] + ", " +
                         fields[2] + ", " + fields[3] + ", " + fields[4] +
                         "), found " + std::to_string(tokens.size()));
        return false;
    }
    for (int i = 0; i < 5; ++i) {
        if (!parseDouble(tokens[i], out[i])) {
            errors.push_back(src + ": " + fields[i] + " '" + tokens[i] +
                             "' is not a number");
            return false;
        }
    }
    if (tokens.size() > 5) {
        warnings.push_back(src + ": " + std::to_string(tokens.size() - 5) +
                           " trailing token(s) after 5 values (ignored)");
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// IRST
// ---------------------------------------------------------------------------
IrstParseResult loadIrstString(const std::string& contents,
                               const std::string& sourceName) {
    IrstParseResult result;
    static const char* const kFields[5] = {
        "azimuth limit", "elevation limit", "nominal range",
        "ground factor", "flare chance"};
    double v[5] = {0, 0, 0, 0, 0};
    if (!parseSensorBody(contents, sourceName, kFields, v, result.errors,
                         result.warnings)) {
        return result;
    }
    if (v[0] <= 0.0 || v[0] > 360.0 || v[1] <= 0.0 || v[1] > 180.0) {
        result.errors.push_back(sourceName +
                                ": implausible FOV limits (az " +
                                std::to_string(v[0]) + " el " +
                                std::to_string(v[1]) + ")");
        return result;
    }
    if (v[2] < 0.0 || v[3] < 0.0 || v[4] < 0.0 || v[4] > 1.0) {
        result.errors.push_back(
            sourceName + ": implausible range/ground/flare values");
        return result;
    }
    f4::data::IrstSensorEntry e;
    e.name = "single";
    e.data.az_limit_deg = v[0];
    e.data.el_limit_deg = v[1];
    e.data.nominal_range_nm = v[2];
    e.data.ground_factor = v[3];
    e.data.flare_chance = v[4];
    result.data.sensors.push_back(std::move(e));
    result.ok = true;
    return result;
}

IrstParseResult loadIrstListString(const std::string& lstContents,
                                    const std::string& sensorDir,
                                    const std::string& sourceName) {
    IrstParseResult result;
    const auto names = parseListFile(lstContents, sourceName, result.errors,
                                     result.warnings);
    if (!result.errors.empty()) return result;
    for (const auto& file : names) {
        const fs::path resolved = resolveInDir(sensorDir, file);
        if (resolved.empty()) {
            result.errors.push_back(sourceName + ": entry '" + file +
                                    "' does not exist in '" + sensorDir + "'");
            return result;
        }
        bool ok = false;
        const std::string contents = readFileToString(resolved.string(), ok);
        if (!ok) {
            result.errors.push_back("cannot open " + resolved.string());
            return result;
        }
        static const char* const kFields[5] = {
            "azimuth limit", "elevation limit", "nominal range",
            "ground factor", "flare chance"};
        double v[5] = {0, 0, 0, 0, 0};
        if (!parseSensorBody(contents, resolved.string(), kFields, v,
                             result.errors, result.warnings)) {
            return result;
        }
        if (v[0] <= 0.0 || v[0] > 360.0 || v[1] <= 0.0 || v[1] > 180.0 ||
            v[2] < 0.0 || v[3] < 0.0 || v[4] < 0.0 || v[4] > 1.0) {
            result.errors.push_back(resolved.string() +
                                    ": implausible sensor values");
            return result;
        }
        f4::data::IrstSensorEntry e;
        // Stem of "aim9l.irs" -> "aim9l" (the reference indexes the
        // .LST position; the stem is the stable name for lookups).
        std::string stem = lowercase(file);
        const std::size_t dot = stem.find_last_of('.');
        if (dot != std::string::npos) stem = stem.substr(0, dot);
        e.name = stem;
        e.data.az_limit_deg = v[0];
        e.data.el_limit_deg = v[1];
        e.data.nominal_range_nm = v[2];
        e.data.ground_factor = v[3];
        e.data.flare_chance = v[4];
        result.data.sensors.push_back(std::move(e));
    }
    result.ok = true;
    return result;
}

IrstParseResult loadIrstListFile(const std::string& lstPath) {
    IrstParseResult result;
    bool ok = false;
    const std::string contents = readFileToString(lstPath, ok);
    if (!ok) {
        result.errors.push_back("cannot open " + lstPath);
        return result;
    }
    return loadIrstListString(
        contents, fs::path(lstPath).parent_path().string(), lstPath);
}

// ---------------------------------------------------------------------------
// RWR
// ---------------------------------------------------------------------------
RwrParseResult loadRwrString(const std::string& contents,
                             const std::string& sourceName) {
    RwrParseResult result;
    static const char* const kFields[5] = {
        "azimuth limit", "elevation limit", "sensitivity", "", ""};
    double v[5] = {0, 0, 0, 0, 0};
    // RWR files carry THREE values; reuse the body parser with a 3-value
    // contract (the generic comment header documents the three).
    const auto tokens = tokenize(contents);
    if (tokens.size() < 3) {
        result.errors.push_back(sourceName +
                                ": expected 3 values (azimuth limit, "
                                "elevation limit, sensitivity), found " +
                                std::to_string(tokens.size()));
        return result;
    }
    for (int i = 0; i < 3; ++i) {
        if (!parseDouble(tokens[i], v[i])) {
            result.errors.push_back(
                sourceName + ": value '" + tokens[i] + "' is not a number");
            return result;
        }
    }
    if (tokens.size() > 3) {
        result.warnings.push_back(
            sourceName + ": " + std::to_string(tokens.size() - 3) +
            " trailing token(s) after 3 values (ignored)");
    }
    (void)kFields;
    if (v[0] <= 0.0 || v[0] > 360.0 || v[1] <= 0.0 || v[1] > 180.0 ||
        v[2] <= 0.0) {
        result.errors.push_back(sourceName +
                                ": implausible RWR limits/sensitivity");
        return result;
    }
    f4::data::RwrSensorEntry e;
    e.name = "single";
    e.data.az_limit_deg = v[0];
    e.data.el_limit_deg = v[1];
    e.data.sensitivity = v[2];
    result.data.sensors.push_back(std::move(e));
    result.ok = true;
    return result;
}

RwrParseResult loadRwrListString(const std::string& lstContents,
                                 const std::string& sensorDir,
                                 const std::string& sourceName) {
    RwrParseResult result;
    const auto names = parseListFile(lstContents, sourceName, result.errors,
                                     result.warnings);
    if (!result.errors.empty()) return result;
    for (const auto& file : names) {
        const fs::path resolved = resolveInDir(sensorDir, file);
        if (resolved.empty()) {
            result.errors.push_back(sourceName + ": entry '" + file +
                                    "' does not exist in '" + sensorDir + "'");
            return result;
        }
        bool ok = false;
        const std::string contents = readFileToString(resolved.string(), ok);
        if (!ok) {
            result.errors.push_back("cannot open " + resolved.string());
            return result;
        }
        const auto tokens = tokenize(contents);
        if (tokens.size() < 3) {
            result.errors.push_back(resolved.string() +
                                    ": expected 3 values (azimuth limit, "
                                    "elevation limit, sensitivity), found " +
                                    std::to_string(tokens.size()));
            return result;
        }
        double v[3] = {0, 0, 0};
        for (int i = 0; i < 3; ++i) {
            if (!parseDouble(tokens[i], v[i])) {
                result.errors.push_back(resolved.string() + ": value '" +
                                        tokens[i] + "' is not a number");
                return result;
            }
        }
        if (tokens.size() > 3) {
            result.warnings.push_back(
                resolved.string() + ": " +
                std::to_string(tokens.size() - 3) +
                " trailing token(s) after 3 values (ignored)");
        }
        if (v[0] <= 0.0 || v[0] > 360.0 || v[1] <= 0.0 || v[1] > 180.0 ||
            v[2] <= 0.0) {
            result.errors.push_back(resolved.string() +
                                    ": implausible RWR limits/sensitivity");
            return result;
        }
        f4::data::RwrSensorEntry e;
        std::string stem = lowercase(file);
        const std::size_t dot = stem.find_last_of('.');
        if (dot != std::string::npos) stem = stem.substr(0, dot);
        e.name = stem;
        e.data.az_limit_deg = v[0];
        e.data.el_limit_deg = v[1];
        e.data.sensitivity = v[2];
        result.data.sensors.push_back(std::move(e));
    }
    result.ok = true;
    return result;
}

RwrParseResult loadRwrListFile(const std::string& lstPath) {
    RwrParseResult result;
    bool ok = false;
    const std::string contents = readFileToString(lstPath, ok);
    if (!ok) {
        result.errors.push_back("cannot open " + lstPath);
        return result;
    }
    return loadRwrListString(
        contents, fs::path(lstPath).parent_path().string(), lstPath);
}

// ---------------------------------------------------------------------------
// Visual
// ---------------------------------------------------------------------------
VisualParseResult loadVisualString(const std::string& contents,
                                   const std::string& sourceName) {
    VisualParseResult result;
    const auto tokens = tokenize(contents);
    if (tokens.size() < 3) {
        result.errors.push_back(
            sourceName +
            ": expected 3 values (azimuth limit, elevation limit, gain), "
            "found " +
            std::to_string(tokens.size()));
        return result;
    }
    double v[3] = {0, 0, 0};
    for (int i = 0; i < 3; ++i) {
        if (!parseDouble(tokens[i], v[i])) {
            result.errors.push_back(sourceName + ": value '" + tokens[i] +
                                    "' is not a number");
            return result;
        }
    }
    if (tokens.size() > 3) {
        result.warnings.push_back(sourceName + ": " +
                                  std::to_string(tokens.size() - 3) +
                                  " trailing token(s) (ignored)");
    }
    if (v[0] <= 0.0 || v[0] > 361.0 || v[1] <= 0.0 || v[1] > 181.0 ||
        v[2] < 0.0) {
        result.errors.push_back(sourceName + ": implausible visual limits/gain");
        return result;
    }
    f4::data::VisualSensorEntry e;
    e.name = "single";
    e.data.az_limit_deg = v[0];
    e.data.el_limit_deg = v[1];
    e.data.gain = v[2];
    result.data.sensors.push_back(std::move(e));
    result.ok = true;
    return result;
}

VisualParseResult loadVisualListString(const std::string& lstContents,
                                       const std::string& sensorDir,
                                       const std::string& sourceName) {
    VisualParseResult result;
    const auto names = parseListFile(lstContents, sourceName, result.errors,
                                     result.warnings);
    if (!result.errors.empty()) return result;
    for (const auto& file : names) {
        const fs::path resolved = resolveInDir(sensorDir, file);
        if (resolved.empty()) {
            result.errors.push_back(sourceName + ": entry '" + file +
                                    "' does not exist in '" + sensorDir + "'");
            return result;
        }
        bool ok = false;
        const std::string contents = readFileToString(resolved.string(), ok);
        if (!ok) {
            result.errors.push_back("cannot open " + resolved.string());
            return result;
        }
        const auto tokens = tokenize(contents);
        if (tokens.size() < 3) {
            result.errors.push_back(
                resolved.string() +
                ": expected 3 values (azimuth limit, elevation limit, gain), "
                "found " +
                std::to_string(tokens.size()));
            return result;
        }
        double v[3] = {0, 0, 0};
        for (int i = 0; i < 3; ++i) {
            if (!parseDouble(tokens[i], v[i])) {
                result.errors.push_back(resolved.string() + ": value '" +
                                        tokens[i] + "' is not a number");
                return result;
            }
        }
        if (tokens.size() > 3) {
            result.warnings.push_back(
                resolved.string() + ": " + std::to_string(tokens.size() - 3) +
                " trailing token(s) (ignored)");
        }
        if (v[0] <= 0.0 || v[0] > 361.0 || v[1] <= 0.0 || v[1] > 181.0 ||
            v[2] < 0.0) {
            result.errors.push_back(resolved.string() +
                                    ": implausible visual limits/gain");
            return result;
        }
        f4::data::VisualSensorEntry e;
        std::string stem = lowercase(file);
        const std::size_t dot = stem.find_last_of('.');
        if (dot != std::string::npos) stem = stem.substr(0, dot);
        e.name = stem;
        e.data.az_limit_deg = v[0];
        e.data.el_limit_deg = v[1];
        e.data.gain = v[2];
        result.data.sensors.push_back(std::move(e));
    }
    result.ok = true;
    return result;
}

VisualParseResult loadVisualListFile(const std::string& lstPath) {
    VisualParseResult result;
    bool ok = false;
    const std::string contents = readFileToString(lstPath, ok);
    if (!ok) {
        result.errors.push_back("cannot open " + lstPath);
        return result;
    }
    return loadVisualListString(
        contents, fs::path(lstPath).parent_path().string(), lstPath);
}

} // namespace f4::convert
