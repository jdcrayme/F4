// f4-convert/src/signature_parser.cpp
//
// Implementation of the SIGDATA grid parser (see signature_parser.hpp
// for the format contract).

#include "f4/convert/signature_parser.hpp"

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

} // namespace

SigGridParseResult loadSigGridString(const std::string& contents,
                                     const std::string& sourceName) {
    SigGridParseResult result;
    const auto tokens = tokenize(contents);
    if (tokens.size() < 4) {
        result.errors.push_back(sourceName +
                                ": too few tokens for a grid (need numAz, "
                                "numEl, breakpoints, rows)");
        return result;
    }

    std::size_t pos = 0;
    long numAz = 0, numEl = 0;
    char* end = nullptr;
    numAz = std::strtol(tokens[pos].c_str(), &end, 10);
    if (end == nullptr || *end != '\0') {
        result.errors.push_back(sourceName + ": numAz '" + tokens[pos] +
                                "' is not an integer");
        return result;
    }
    ++pos;
    numEl = std::strtol(tokens[pos].c_str(), &end, 10);
    if (end == nullptr || *end != '\0') {
        result.errors.push_back(sourceName + ": numEl '" + tokens[pos] +
                                "' is not an integer");
        return result;
    }
    ++pos;
    if (numAz < 1 || numAz > 512 || numEl < 1 || numEl > 512) {
        result.errors.push_back(sourceName + ": implausible grid shape " +
                                std::to_string(numAz) + "x" +
                                std::to_string(numEl));
        return result;
    }

    // Azimuth breakpoints (numAz values).
    result.grid.azimuth_deg.reserve(static_cast<std::size_t>(numAz));
    for (long i = 0; i < numAz; ++i) {
        if (pos >= tokens.size()) {
            result.errors.push_back(sourceName +
                                    ": unexpected EOF in azimuth breakpoints");
            return result;
        }
        double v = 0;
        if (!parseDouble(tokens[pos], v)) {
            result.errors.push_back(sourceName + ": azimuth breakpoint '" +
                                    tokens[pos] + "' is not a number");
            return result;
        }
        result.grid.azimuth_deg.push_back(v);
        ++pos;
    }
    // The data rows: "# Elevation, then the data" — one row per
    // elevation breakpoint, each row = the elevation value followed by
    // numAz values. The row labels ARE the elevation breakpoints (there
    // is no separate elevation block in the file).
    result.grid.values.assign(
        static_cast<std::size_t>(numEl),
        std::vector<double>(static_cast<std::size_t>(numAz), 0.0));
    result.grid.elevation_deg.reserve(static_cast<std::size_t>(numEl));
    for (long el = 0; el < numEl; ++el) {
        if (pos >= tokens.size()) {
            result.errors.push_back(
                sourceName + ": unexpected EOF: want " +
                std::to_string(numEl) + " data rows, found " +
                std::to_string(el));
            return result;
        }
        double rowEl = 0;
        if (!parseDouble(tokens[pos], rowEl)) {
            result.errors.push_back(sourceName + ": data row " +
                                    std::to_string(el) + " elevation '" +
                                    tokens[pos] + "' is not a number");
            return result;
        }
        ++pos;
        result.grid.elevation_deg.push_back(rowEl);
        for (long az = 0; az < numAz; ++az) {
            if (pos >= tokens.size()) {
                result.errors.push_back(
                    sourceName + ": unexpected EOF in data row " +
                    std::to_string(el));
                return result;
            }
            double v = 0;
            if (!parseDouble(tokens[pos], v)) {
                result.errors.push_back(sourceName + ": data row " +
                                        std::to_string(el) + " value '" +
                                        tokens[pos] + "' is not a number");
                return result;
            }
            result.grid.values[el][az] = v;
            ++pos;
        }
    }

    if (pos < tokens.size()) {
        result.warnings.push_back(
            sourceName + ": " + std::to_string(tokens.size() - pos) +
            " trailing token(s) after the grid (ignored)");
    }

    // Breakpoint sanity: ascending within each axis (the interpolation
    // contract).
    for (std::size_t i = 1; i < result.grid.azimuth_deg.size(); ++i) {
        if (result.grid.azimuth_deg[i] <= result.grid.azimuth_deg[i - 1]) {
            result.errors.push_back(sourceName +
                                    ": azimuth breakpoints not ascending");
            return result;
        }
    }
    for (std::size_t i = 1; i < result.grid.elevation_deg.size(); ++i) {
        if (result.grid.elevation_deg[i] <= result.grid.elevation_deg[i - 1]) {
            result.errors.push_back(
                sourceName + ": elevation breakpoints not ascending");
            return result;
        }
    }

    result.ok = true;
    return result;
}

SigGridParseResult loadSigGridFile(const std::string& path) {
    SigGridParseResult result;
    bool ok = false;
    const std::string contents = readFileToString(path, ok);
    if (!ok) {
        result.errors.push_back("cannot open " + path);
        return result;
    }
    return loadSigGridString(contents, path);
}

SigParseResult loadSignatureDataDir(const std::string& dir) {
    SigParseResult result;

    // SIGDATA.LST: count + stems.
    const fs::path lstPath = resolveInDir(dir, "SIGDATA.LST");
    if (lstPath.empty()) {
        result.errors.push_back("cannot find SIGDATA.LST in '" + dir + "'");
        return result;
    }
    bool ok = false;
    const std::string lstContents = readFileToString(lstPath.string(), ok);
    if (!ok) {
        result.errors.push_back("cannot open " + lstPath.string());
        return result;
    }
    const auto tokens = tokenize(lstContents);
    if (tokens.empty()) {
        result.errors.push_back(lstPath.string() + ": no tokens (empty?)");
        return result;
    }
    long count = 0;
    char* end = nullptr;
    count = std::strtol(tokens[0].c_str(), &end, 10);
    if (end == nullptr || *end != '\0' || count < 0 || count > 1024) {
        result.errors.push_back(lstPath.string() + ": bad count '" +
                                tokens[0] + "'");
        return result;
    }
    if (tokens.size() < 1 + static_cast<std::size_t>(count)) {
        result.errors.push_back(lstPath.string() + ": expected " +
                                std::to_string(count) +
                                " stems, found " +
                                std::to_string(tokens.size() - 1));
        return result;
    }
    if (tokens.size() > 1 + static_cast<std::size_t>(count)) {
        result.warnings.push_back(
            lstPath.string() + ": " +
            std::to_string(tokens.size() - 1 - static_cast<std::size_t>(count)) +
            " trailing token(s) after the list (ignored)");
    }

    for (long i = 0; i < count; ++i) {
        const std::string stem = tokens[1 + static_cast<std::size_t>(i)];
        f4::data::AircraftSignatureData entry;
        entry.name = lowercase(stem);

        // The five grid families. Every family must be present for a
        // stem (the reference's class table expects the full set).
        struct GridSpec {
            const char* subdir;
            const char* suffix;
            f4::data::SignatureGrid* target;
        };
        GridSpec specs[] = {
            {"RCSDAT", ".RCS", &entry.rcs},
            {"IR", ".IR0", &entry.ir0},
            {"IR", ".IR1", &entry.ir1},
            {"IR", ".IR2", &entry.ir2},
            {"VISUAL", ".VIS", &entry.visual},
        };
        for (const auto& spec : specs) {
            const fs::path file = resolveInDir(
                (fs::path(dir) / spec.subdir).string(), stem + spec.suffix);
            if (file.empty()) {
                result.errors.push_back(
                    std::string("cannot find ") + spec.suffix + " for '" +
                    entry.name + "' (" + spec.subdir + ") in '" + dir + "'");
                return result;
            }
            const auto grid = loadSigGridFile(file.string());
            if (!grid.ok) {
                result.errors.insert(result.errors.end(),
                                     grid.errors.begin(), grid.errors.end());
                return result;
            }
            result.warnings.insert(result.warnings.end(),
                                   grid.warnings.begin(),
                                   grid.warnings.end());
            *spec.target = std::move(grid.grid);
        }
        result.library.entries.push_back(std::move(entry));
    }

    result.ok = true;
    return result;
}

} // namespace f4::convert
