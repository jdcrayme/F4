// f4-convert/src/mnvr_parser.cpp
//
// Implementation of the mnvrdata.dat parser (see mnvr_parser.hpp for the
// format contract and the 'A' marker quirk).

#include "f4/convert/mnvr_parser.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace f4::convert {

using f4::data::ManeuverData;
using f4::data::ManeuverChoice;
using f4::data::kNumMnvrClasses;
using f4::data::kNumInterceptTypes;
using f4::data::kNumMergeTypes;
using f4::data::kNumReactTypes;

namespace {

// Tokenizer with the reference's GetNext() semantics: strip '#' line
// comments, split on whitespace (file.cpp:411-441 — fscanf("%s") +
// skip lines whose first token starts with '#' or ';').
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

bool parseInt(const std::string& tok, long& out) {
    if (tok.empty()) return false;
    char* end = nullptr;
    out = std::strtol(tok.c_str(), &end, 10);
    return end != nullptr && *end == '\0';
}

// sscanf("%x") semantics: optional 0x/0X prefix, hex digits.
bool parseHex(const std::string& tok, unsigned long& out) {
    std::size_t i = 0;
    if (i + 1 < tok.size() && tok[i] == '0' &&
        (tok[i + 1] == 'x' || tok[i + 1] == 'X')) {
        i += 2;
    }
    if (i >= tok.size()) return false;
    unsigned long v = 0;
    for (; i < tok.size(); ++i) {
        char c = tok[i];
        int digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = 10 + (c - 'a');
        else if (c >= 'A' && c <= 'F') digit = 10 + (c - 'A');
        else return false;
        v = v * 16 + static_cast<unsigned long>(digit);
    }
    out = v;
    return true;
}

} // namespace

MnvrParseResult loadMnvString(const std::string& contents,
                              const std::string& sourceName) {
    MnvrParseResult result;
    auto tokens = tokenize(contents);
    if (tokens.empty()) {
        result.errors.push_back(sourceName + ": no tokens (empty file?)");
        return result;
    }

    // The 'A'/'B' file-type marker quirk: FreeFalcon consumes ONE byte and
    // only parses when it is '#' (digimain.cpp:819-824). The shipped file
    // begins with 'A' and is therefore skipped by the reference engine.
    // Our tokenizer leaves that byte as a single-letter token (it precedes
    // the first '#' comment); accept and drop it with a warning so the
    // authored tables load.
    if (tokens[0].size() == 1 &&
        (tokens[0][0] == 'A' || tokens[0][0] == 'B')) {
        result.warnings.push_back(
            sourceName + ": leading '" + tokens[0][0] +
            "' file-type marker (FreeFalcon reads one byte and expects "
            "'#'; the reference engine silently skips this file — "
            "accepted here, see f4/data/maneuver_data.hpp)");
        tokens.erase(tokens.begin());
    }
    if (tokens.empty()) {
        result.errors.push_back(sourceName + ": no data after marker");
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

    for (std::size_t own = 0; own < kNumMnvrClasses; ++own) {
        const std::string flagTok = next("class flags");
        unsigned long flags = 0;
        if (!result.errors.empty()) break;
        if (!parseHex(flagTok, flags)) {
            result.errors.push_back(
                sourceName + ": class " +
                f4::data::kMnvrClassNames[own] +
                " flags '" + flagTok + "' is not hex");
            break;
        }
        result.data.classFlags[own] = static_cast<std::uint32_t>(flags);

        for (std::size_t opp = 0; opp < kNumMnvrClasses; ++opp) {
            ManeuverChoice& cell = result.data.table[own][opp];
            long nI = 0, nM = 0, nR = 0;
            const std::string iTok = next("numIntercepts");
            const std::string mTok = next("numMerges");
            const std::string rTok = next("numReacts");
            if (!result.errors.empty()) break;
            if (!parseInt(iTok, nI) || !parseInt(mTok, nM) ||
                !parseInt(rTok, nR)) {
                result.errors.push_back(
                    sourceName + ": table[" +
                    f4::data::kMnvrClassNames[own] + "][" +
                    f4::data::kMnvrClassNames[opp] +
                    "] counts are not integers ('" + iTok + "' '" + mTok +
                    "' '" + rTok + "')");
                break;
            }
            if (nI < 0 || nI > static_cast<long>(kNumInterceptTypes) ||
                nM < 0 || nM > static_cast<long>(kNumMergeTypes) ||
                nR < 0 || nR > static_cast<long>(kNumReactTypes)) {
                result.errors.push_back(
                    sourceName + ": table[" +
                    f4::data::kMnvrClassNames[own] + "][" +
                    f4::data::kMnvrClassNames[opp] + "] counts out of " +
                    "range (" + std::to_string(nI) + "/" +
                    std::to_string(nM) + "/" + std::to_string(nR) + ")");
                break;
            }
            const auto readIndices = [&](long count,
                                         long limit,
                                         std::vector<int>& out,
                                         const char* what) {
                for (long k = 0; k < count; ++k) {
                    const std::string tok = next(what);
                    if (!result.errors.empty()) return;
                    long v = 0;
                    if (!parseInt(tok, v)) {
                        result.errors.push_back(
                            sourceName + ": " + what + " index '" + tok +
                            "' is not an integer");
                        return;
                    }
                    const int idx = static_cast<int>(v - 1);  // 1-based file
                    if (idx < 0 || idx >= static_cast<int>(limit)) {
                        result.errors.push_back(
                            sourceName + ": " + what + " index " + tok +
                            " out of range after 1-based conversion");
                        return;
                    }
                    out.push_back(idx);
                }
            };
            readIndices(nI, static_cast<long>(kNumInterceptTypes),
                        cell.intercepts, "intercept");
            readIndices(nM, static_cast<long>(kNumMergeTypes),
                        cell.merges, "merge");
            readIndices(nR, static_cast<long>(kNumReactTypes),
                        cell.spikeReacts, "react");
            if (!result.errors.empty()) break;
        }
        if (!result.errors.empty()) break;
    }

    if (result.errors.empty()) {
        if (pos < tokens.size()) {
            result.warnings.push_back(
                sourceName + ": " + std::to_string(tokens.size() - pos) +
                " trailing token(s) after the 9x9 table (ignored)");
        }
        result.ok = true;
    }
    return result;
}

MnvrParseResult loadMnvFile(const std::string& path) {
    MnvrParseResult result;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        result.errors.push_back("cannot open " + path);
        return result;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    auto inner = loadMnvString(ss.str(), path);
    return inner;
}

} // namespace f4::convert
