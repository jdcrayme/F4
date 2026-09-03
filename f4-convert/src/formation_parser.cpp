// f4-convert/src/formation_parser.cpp
//
// Implementation of the formdat.fil parser (see formation_parser.hpp for
// the format contract).

#include "f4/convert/formation_parser.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace f4::convert {

using f4::data::Formation;
using f4::data::FormationSlot;

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

} // namespace

FormationParseResult loadFormString(const std::string& contents,
                                    const std::string& sourceName) {
    FormationParseResult result;
    auto tokens = tokenize(contents);
    if (tokens.empty()) {
        result.errors.push_back(sourceName + ": no tokens (empty file?)");
        return result;
    }

    std::size_t pos = 0;
    const auto next = [&](const char* what) -> std::string {
        if (pos >= tokens.size()) {
            result.errors.push_back(sourceName + ": unexpected EOF while "
                                                 "reading " + what);
            return {};
        }
        return tokens[pos++];
    };
    const auto nextInt = [&](const char* what, long& out) -> bool {
        const std::string tok = next(what);
        if (result.errors.empty()) {
            char* end = nullptr;
            out = std::strtol(tok.c_str(), &end, 10);
            if (end == nullptr || *end != '\0') {
                result.errors.push_back(sourceName + ": " + what +
                                         " token '" + tok +
                                         "' is not an integer");
                return false;
            }
            return true;
        }
        return false;
    };
    const auto nextDouble = [&](const char* what, double& out) -> bool {
        const std::string tok = next(what);
        if (result.errors.empty()) {
            char* end = nullptr;
            out = std::strtod(tok.c_str(), &end);
            if (end == nullptr || *end != '\0') {
                result.errors.push_back(sourceName + ": " + what +
                                         " token '" + tok +
                                         "' is not a number");
                return false;
            }
            return true;
        }
        return false;
    };

    // Sanity ceilings matching the reference's array allocations
    // (formdata.cpp allocates exactly num4Slots slots; a rogue count is
    // corruption, not a big formation).
    constexpr long kMaxFormations = 64;
    constexpr long kMaxSlots = 8;

    long numFormations = 0;
    if (!nextInt("numFormations", numFormations) ||
        numFormations < 1 || numFormations > kMaxFormations) {
        if (result.errors.empty()) {
            result.errors.push_back(
                sourceName + ": numFormations out of range (" +
                std::to_string(numFormations) + ")");
        }
        return result;
    }

    for (long fi = 0; fi < numFormations; ++fi) {
        long num4 = 0, num2 = 0, formNum = 0;
        if (!nextInt("num4Slots", num4)) return result;
        if (!nextInt("num2Slots", num2)) return result;
        if (!nextInt("formNum", formNum)) return result;
        const std::string name = next("formation name");
        if (!result.errors.empty()) return result;
        if (num4 < 1 || num4 > kMaxSlots) {
            result.errors.push_back(
                sourceName + ": formation '" + name + "' num4Slots " +
                std::to_string(num4) + " out of range");
            return result;
        }
        if (num2 < 0 || num2 > 1) {
            result.errors.push_back(
                sourceName + ": formation '" + name + "' num2Slots " +
                std::to_string(num2) + " out of range (expected 0 or 1)");
            return result;
        }

        Formation f;
        f.name = name;
        f.form_num = static_cast<int>(formNum);

        const auto readSlot = [&](FormationSlot& s) -> bool {
            double az = 0.0, el = 0.0, rng = 0.0;
            if (!nextDouble("relAz", az)) return false;
            if (!nextDouble("relEl", el)) return false;
            if (!nextDouble("range", rng)) return false;
            s.rel_az_deg = az;
            s.rel_el_deg = el;
            s.range_nm = rng;
            s.form_num = f.form_num;
            return true;
        };

        for (long k = 0; k < num4; ++k) {
            FormationSlot s;
            if (!readSlot(s)) return result;
            f.slots.push_back(std::move(s));
        }
        if (num2 > 0) {
            if (!readSlot(f.two_ship)) return result;
            f.two_ship_explicit = true;
        } else {
            // formdata.cpp:85-91 — no dedicated triple: the 2-ship slot
            // inherits slot[0].
            f.two_ship = f.slots.front();
            f.two_ship_explicit = false;
        }

        // Geometry sanity (the shipped files are well-formed; violations
        // indicate token drift, so they are errors, not warnings).
        for (const auto& s : f.slots) {
            if (s.rel_az_deg < -360.0 || s.rel_az_deg > 360.0 ||
                s.rel_el_deg < -90.0 || s.rel_el_deg > 90.0 ||
                s.range_nm < 0.0 || s.range_nm > 20.0) {
                result.errors.push_back(
                    sourceName + ": formation '" + name +
                    "' slot geometry implausible (az " +
                    std::to_string(s.rel_az_deg) + ", el " +
                    std::to_string(s.rel_el_deg) + ", range " +
                    std::to_string(s.range_nm) + " NM)");
                return result;
            }
        }

        result.data.formations.push_back(std::move(f));
    }

    if (result.errors.empty()) {
        if (pos < tokens.size()) {
            result.warnings.push_back(
                sourceName + ": " + std::to_string(tokens.size() - pos) +
                " trailing token(s) after the formations (ignored)");
        }
        result.ok = true;
    }
    return result;
}

FormationParseResult loadFormFile(const std::string& path) {
    FormationParseResult result;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        result.errors.push_back("cannot open " + path);
        return result;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return loadFormString(ss.str(), path);
}

} // namespace f4::convert
