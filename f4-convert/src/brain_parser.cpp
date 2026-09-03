// f4-convert/src/brain_parser.cpp
//
// Implementation of the .brn parser (see brain_parser.hpp for the
// reconstructed format contract).

#include "f4/convert/brain_parser.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace f4::convert {

using f4::data::BrainArchetype;
using f4::data::BrainModeRow;

namespace {

std::string trim(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e &&
           std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b &&
           std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

bool isCommentLine(const std::string& line) {
    return !line.empty() && line[0] == '#';
}

std::string commentLabel(const std::string& line) {
    return trim(line.substr(1));
}

bool parseDoubles(const std::string& line, double out[3]) {
    std::istringstream iss(line);
    return static_cast<bool>(iss >> out[0] >> out[1] >> out[2]) &&
           iss.peek() == std::char_traits<char>::eof();
}

bool parseIntLine(const std::string& line, int& out) {
    std::istringstream iss(line);
    return static_cast<bool>(iss >> out) &&
           iss.peek() == std::char_traits<char>::eof();
}

} // namespace

BrainParseResult loadBrainString(const std::string& contents,
                                 const std::string& sourceName) {
    BrainParseResult result;

    // Split into lines, strip CR, drop blanks.
    std::vector<std::string> lines;
    {
        std::istringstream iss(contents);
        std::string line;
        while (std::getline(iss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const std::string t = trim(line);
            if (!t.empty()) lines.push_back(t);
        }
    }
    if (lines.empty()) {
        result.errors.push_back(sourceName + ": no lines (empty file?)");
        return result;
    }

    std::size_t i = 0;

    // Optional archetype count header (BRAINDAT.brn): a lone small int,
    // possibly preceded by comment lines, and followed by a comment (the
    // first section name). GENERIC.BRN starts with a mode row whose
    // enabled-int would ALSO be a lone small int — the discriminator is
    // the FOLLOWING line (a count header is followed by a comment; a
    // mode row's int is followed by its value triple).
    long declaredCount = -1;
    {
        std::size_t probe = 0;
        while (probe < lines.size() && isCommentLine(lines[probe])) ++probe;
        int n = 0;
        if (probe < lines.size() && parseIntLine(lines[probe], n) &&
            n > 0 && n <= 32 && probe + 1 < lines.size() &&
            isCommentLine(lines[probe + 1])) {
            declaredCount = n;
            i = probe + 1;
        }
    }

    std::vector<BrainArchetype> archetypes;
    BrainArchetype current;
    bool haveCurrent = false;
    std::size_t maxGsLines = 0;

    const auto flushCurrent = [&]() {
        if (haveCurrent && !current.modes.empty()) {
            archetypes.push_back(std::move(current));
        } else if (haveCurrent) {
            result.warnings.push_back(
                sourceName + ": archetype '" + current.name +
                "' has no mode rows (dropped)");
        }
        current = BrainArchetype{};
        haveCurrent = false;
    };

    while (i < lines.size()) {
        const std::string& line = lines[i];

        if (isCommentLine(line)) {
            const std::string label = commentLabel(line);

            // Trailer: "# Max Gs" (GENERIC.BRN)
            if (label == "Max Gs") {
                if (i + 1 < lines.size()) {
                    std::istringstream iss(lines[i + 1]);
                    if (iss >> result.max_gs) {
                        i += 2;
                        ++maxGsLines;
                        continue;
                    }
                }
                result.errors.push_back(
                    sourceName + ": 'Max Gs' trailer missing its value");
                break;
            }

            // Section header: a comment line followed by ANOTHER comment
            // line (a mode row's label is followed by its enabled int).
            if (i + 1 < lines.size() && isCommentLine(lines[i + 1])) {
                flushCurrent();
                current.name = label;
                haveCurrent = true;
                ++i;
                continue;
            }

            // Mode row: label line + enabled int + value triple.
            if (i + 2 >= lines.size()) {
                result.errors.push_back(
                    sourceName + ": truncated mode row at '" + label + "'");
                break;
            }
            int enabled = 0;
            double v[3] = {0.0, 0.0, 0.0};
            if (!parseIntLine(lines[i + 1], enabled)) {
                result.errors.push_back(
                    sourceName + ": mode '" + label + "' enabled line '" +
                    lines[i + 1] + "' is not an int");
                break;
            }
            if (!parseDoubles(lines[i + 2], v)) {
                result.errors.push_back(
                    sourceName + ": mode '" + label + "' value line '" +
                    lines[i + 2] + "' is not a 3-double triple");
                break;
            }
            if (!haveCurrent) {
                // Bare rows (GENERIC.BRN): synthesize the default
                // archetype the file was designed to be.
                current.name = "Generic";
                haveCurrent = true;
            }
            BrainModeRow row;
            row.label = label;
            row.enabled = enabled;
            row.priority = v[0];
            row.range_ft = v[1];
            row.angle_deg = v[2];
            row.row = current.modes.size();
            current.modes.push_back(std::move(row));
            i += 3;
            continue;
        }

        // A non-comment line outside a row: only the count header (already
        // consumed). Anything else is structural corruption.
        result.errors.push_back(
            sourceName + ": unexpected non-comment line '" + line + "'");
        break;
    }

    flushCurrent();

    if (result.errors.empty()) {
        if (declaredCount >= 0 &&
            static_cast<long>(archetypes.size()) != declaredCount) {
            result.warnings.push_back(
                sourceName + ": declared " + std::to_string(declaredCount) +
                " archetypes, parsed " + std::to_string(archetypes.size()));
        }
        if (archetypes.empty()) {
            result.errors.push_back(sourceName + ": no archetypes parsed");
        }
    }
    if (result.errors.empty()) {
        result.data.archetypes = std::move(archetypes);
        result.ok = true;
    }
    return result;
}

BrainParseResult loadBrainFile(const std::string& path) {
    BrainParseResult result;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        result.errors.push_back("cannot open " + path);
        return result;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return loadBrainString(ss.str(), path);
}

} // namespace f4::convert
