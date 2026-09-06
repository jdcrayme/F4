// f4-world-convert/src/aii_config.cpp
//
// Implementation of the Falcon4.AII reader — see aii_config.hpp for the
// format contract and the bubble-key precedence chain.

#include <f4/world_types/aii_config.hpp>

#include <f4/io/read_file.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <utility>

namespace f4::world_types {

namespace {

// Windows INI folding: sections and keys match case-insensitively
// (GetPrivateProfileString semantics — real AII files mix spellings).
std::string fold(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        out.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

// Trim ASCII whitespace both ends.
std::string_view trim(std::string_view s) {
    const auto is_ws = [](const unsigned char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
               c == '\f' || c == '\v';
    };
    while (!s.empty() && is_ws(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && is_ws(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return s;
}

// Parse a bubble-size value: a plain number (strtod, whole string).
// Throws with a loud, actionable message otherwise — a bubble size we
// silently defaulted would quietly change deagg geometry.
double parse_bubble_number(std::string_view raw, std::string_view which,
                           const std::filesystem::path& path) {
    const std::string text(trim(raw));
    if (text.empty()) {
        throw std::runtime_error(std::string("aii: ") + path.string() +
                                 ": " + std::string(which) +
                                 " has an empty value");
    }
    char* end = nullptr;
    const double v = std::strtod(text.c_str(), &end);
    if (end != text.c_str() + text.size() || std::isnan(v)) {
        throw std::runtime_error(std::string("aii: ") + path.string() +
                                 ": " + std::string(which) +
                                 " is not a number: '" + text + "'");
    }
    return v;
}

} // namespace

AiiConfig AiiConfig::load(const std::filesystem::path& path) {
    // read_file throws with the path on open/short-read failure; re-prefix
    // through the house "aii:" diagnostic by wrapping.
    std::vector<uint8_t> bytes;
    try {
        bytes = f4::io::read_file(path, "aii");
    } catch (const std::exception& e) {
        // read_file's message already carries the path; keep it whole.
        throw std::runtime_error(std::string("aii: ") + e.what());
    }

    AiiConfig cfg;
    std::string current; // folded current-section name; empty = before any [section]

    // An empty file yields an empty view — avoid string_view(nullptr, 0).
    std::string_view text(bytes.empty()
                              ? std::string_view("")
                              : std::string_view(
                                    reinterpret_cast<const char*>(bytes.data()),
                                    bytes.size()));
    std::size_t line_no = 0;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const auto nl = text.find('\n', pos);
        std::string_view line = text.substr(
            pos, nl == std::string_view::npos ? std::string_view::npos
                                              : nl - pos);
        ++line_no;
        pos = (nl == std::string_view::npos) ? text.size() + 1 : nl + 1;

        // Strip a ';' comment, then trim.
        if (const auto semi = line.find(';'); semi != std::string_view::npos) {
            line = line.substr(0, semi);
        }
        line = trim(line);
        if (line.empty()) continue;

        if (line.front() == '[') {
            // Section header: "[name]". A trailing junk after ']' is an error —
            // it almost certainly means a multi-branch line got mangled.
            const auto close = line.find(']');
            if (close == std::string_view::npos) {
                throw std::runtime_error("aii: " + path.string() + ":" +
                                         std::to_string(line_no) +
                                         ": unterminated section header");
            }
            if (const auto rest = trim(line.substr(close + 1));
                !rest.empty()) {
                throw std::runtime_error("aii: " + path.string() + ":" +
                                         std::to_string(line_no) +
                                         ": trailing junk after section header");
            }
            current = fold(line.substr(1, close - 1));
            cfg.sections_[current]; // create even if it ends up keyless
            continue;
        }

        // Key = value. A line with no '=' is a parse error.
        const auto eq = line.find('=');
        if (eq == std::string_view::npos) {
            throw std::runtime_error("aii: " + path.string() + ":" +
                                     std::to_string(line_no) +
                                     ": expected 'key = value' (no '=')");
        }
        const auto key = trim(line.substr(0, eq));
        const auto value = trim(line.substr(eq + 1));
        if (key.empty()) {
            throw std::runtime_error("aii: " + path.string() + ":" +
                                     std::to_string(line_no) +
                                     ": empty key before '='");
        }
        if (current.empty()) {
            throw std::runtime_error("aii: " + path.string() + ":" +
                                     std::to_string(line_no) +
                                     ": key '" + std::string(key) +
                                     "' before any [section]");
        }
        // Last value wins (GetPrivateProfileString semantics). Keys fold
        // at store time — lookup() folds its arguments, so both sides of
        // the map agree on the folded spelling.
        cfg.sections_[current][fold(key)] = std::string(value);
    }

    // The bubble settings — precedence per the header doc. Primary
    // spellings are the FreeFalcon-source names (NEXT_PHASE_PLAN.md B.0);
    // aliases are the FALCON4_FILE_LAYOUT.md §4.1 names.
    if (auto [v, found] =
            cfg.resolve_bubble_value_("minbubblesize", "sim_bubble_size");
        found) {
        cfg.sim_bubble_size_grid_ =
            parse_bubble_number(v, "SIM_BUBBLE_SIZE/MinBubbleSize", path);
    }
    if (auto [v, found] = cfg.resolve_bubble_value_(
            "bubbleratiotounitspan", "ground_bubble_size");
        found) {
        cfg.ground_bubble_size_grid_ = parse_bubble_number(
            v, "GROUND_BUBBLE_SIZE/BubbleRatioToUnitSpan", path);
    }

    return cfg;
}

AiiConfig AiiConfig::documented_defaults() {
    return AiiConfig{}; // fields already carry the documented defaults
}

AiiConfig AiiConfig::load_if_exists(const std::filesystem::path& path,
                                    const AiiConfig& fallback) {
    // Empty path = not configured. A path that names a missing file is
    // the same thing — defaults preserved (the FreeFalcon install ships
    // the AII; test fixtures and hand-built scenarios may not).
    if (path.empty() || !std::filesystem::exists(path)) {
        return fallback;
    }
    return load(path);
}

const std::string* AiiConfig::lookup(std::string_view section,
                                     std::string_view key) const {
    const auto sit = sections_.find(fold(section));
    if (sit == sections_.end()) return nullptr;
    const auto kit = sit->second.find(fold(key));
    if (kit == sit->second.end()) return nullptr;
    return &kit->second;
}

std::pair<std::string, bool> AiiConfig::resolve_bubble_value_(
    std::string_view primary, std::string_view alias) const {
    // 1. The documented [Sim] section (folded "sim").
    if (const auto* v = lookup("Sim", primary)) return {*v, true};
    if (const auto* v = lookup("Sim", alias)) return {*v, true};
    // 2. Last-resort scan: any section carrying one of the spellings.
    //    std::map order = deterministic (folded section name ascending);
    //    within a section the primary spelling wins over the alias.
    for (const auto& [name, keys] : sections_) {
        (void)name;
        if (const auto kit = keys.find(std::string(primary));
            kit != keys.end()) {
            return {kit->second, true};
        }
        if (const auto kit = keys.find(std::string(alias));
            kit != keys.end()) {
            return {kit->second, true};
        }
    }
    return {"", false};
}

} // namespace f4::world_types
