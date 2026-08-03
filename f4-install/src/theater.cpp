// f4-install/src/theater.cpp

#include <f4/install/theater.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace f4::install {

namespace {

/// Trim leading/trailing whitespace from a string view.
std::string trim(std::string s) {
    const auto first = std::find_if_not(s.begin(), s.end(),
                                         [](unsigned char c) { return std::isspace(c); });
    if (first == s.end()) return {};
    const auto last = std::find_if_not(s.rbegin(), s.rend(),
                                        [](unsigned char c) { return std::isspace(c); }).base();
    return {first, last};
}

/// Lowercase ASCII — for case-insensitive directory/file name matching.
/// (We can't use std::filesystem::path's native comparison because it's
/// case-sensitive on POSIX and case-insensitive on Windows by default;
/// for cross-platform consistency we lowercase explicitly.)
std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

/// Strip surrounding quotes/brackets if present ("korea", 'korea', [korea]).
std::string strip_quotes(std::string s) {
    if (s.size() >= 2) {
        const char a = s.front();
        const char b = s.back();
        if ((a == '"' && b == '"') ||
            (a == '\'' && b == '\'') ||
            (a == '[' && b == ']') ||
            (a == '(' && b == ')')) {
            return s.substr(1, s.size() - 2);
        }
    }
    return s;
}

/// Case-insensitive equality for two paths' filename strings.
bool iequals(const std::string& a, const std::string& b) {
    return to_lower(a) == to_lower(b);
}

} // namespace

std::vector<std::string> parse_theater_lst_string(const std::string& content) {
    std::vector<std::string> out;
    std::istringstream ss(content);
    std::string line;
    while (std::getline(ss, line)) {
        std::string t = trim(line);
        if (t.empty()) continue;
        // Skip comments.
        if (t.starts_with('#') || t.starts_with("//") || t.starts_with(';')) continue;
        // Strip trailing inline comments (// or # or ;).
        for (const std::string& prefix : {"//", "#", ";"}) {
            const auto pos = t.find(prefix);
            if (pos != std::string::npos) t = trim(t.substr(0, pos));
        }
        t = strip_quotes(t);
        if (t.empty()) continue;
        out.push_back(to_lower(t));
    }
    return out;
}

std::vector<std::string> parse_theater_lst(const std::filesystem::path& lst_path) {
    std::ifstream f(lst_path);
    if (!f) return {};
    std::stringstream buf;
    buf << f.rdbuf();
    return parse_theater_lst_string(buf.str());
}

std::string read_theater_title(const std::filesystem::path& ini_path) {
    std::ifstream f(ini_path);
    if (!f) return {};

    bool in_theater_section = false;
    std::string line;
    while (std::getline(f, line)) {
        std::string t = trim(line);
        if (t.empty()) continue;

        // Section header?
        if (t.front() == '[') {
            const auto end = t.find(']');
            if (end != std::string::npos) {
                std::string section = to_lower(trim(t.substr(1, end - 1)));
                in_theater_section = (section == "theater");
            }
            continue;
        }

        if (!in_theater_section) continue;

        // key=value
        const auto eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string key = to_lower(trim(t.substr(0, eq)));
        std::string val = trim(t.substr(eq + 1));
        if (key == "title") {
            val = strip_quotes(val);
            return val;
        }
    }
    return {};
}

namespace {

/// Capitalize the first letter of `key` for use as a default display_name.
/// "korea" → "Korea", "balkans" → "Balkans".
std::string capitalize_first(std::string s) {
    if (!s.empty()) s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    return s;
}

/// Probe for a file in `dir` whose name matches `want` case-insensitively.
/// Returns the matched path (preserving the original on-disk casing) or
/// an empty path if not found.
std::filesystem::path find_case_insensitive(const std::filesystem::path& dir,
                                              const std::string& want) {
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return {};
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) continue;
        if (iequals(entry.path().filename().string(), want)) {
            return entry.path();
        }
    }
    return {};
}

/// Collect every THEATER.* file in `dir` (case-insensitive prefix match).
std::vector<std::filesystem::path> collect_theater_files(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return out;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) continue;
        const std::string name = entry.path().filename().string();
        const std::string stem_lower = to_lower(name);
        // The canonical theater files all start with "THEATER." (uppercase
        // in the shipping data). Match case-insensitively so community
        // installs that lowercase everything still work.
        if (stem_lower.starts_with("theater.")) {
            out.push_back(entry.path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace


/// Look for a subdirectory named `name` case-insensitively in `dir`.
std::filesystem::path find_subdir(const std::filesystem::path& dir, const std::string& name) {
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return {};
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_directory()) continue;
        if (iequals(entry.path().filename().string(), name)) {
            return entry.path();
        }
    }
    return {};
}

std::vector<Theater> scan_theaters(const std::filesystem::path& terrdata_dir,
                                    const std::vector<std::string>& preferred_order) {
    std::vector<Theater> out;
    std::error_code ec;
    if (!std::filesystem::exists(terrdata_dir, ec)) return out;

    // First pass: discover all candidate theater directories (subdirs of
    // terrdata/ that contain a THEATER.MAP). We collect them by key so
    // we can apply preferred ordering afterwards.
    std::unordered_map<std::string, Theater> by_key;
    for (const auto& entry : std::filesystem::directory_iterator(terrdata_dir, ec)) {
        if (!entry.is_directory()) continue;
        const std::string dir_name = entry.path().filename().string();
        const std::string key = to_lower(dir_name);
        
        // Probe for THEATER.MAP — required for a directory to qualify.
        auto terrain_path = find_subdir(entry, "terrain");
        if (terrain_path.empty())
            terrain_path = entry;

        auto map_path = find_case_insensitive(terrain_path, "THEATER.MAP");
        if (map_path.empty()) continue;  // not a theater

        Theater t;
        t.key = key;
        t.dir = terrain_path;
        t.theater_map = map_path;
        t.theater_mea = find_case_insensitive(terrain_path, "THEATER.MEA");
        t.theater_o2  = find_case_insensitive(terrain_path, "THEATER.O2");
        t.theater_ini = find_case_insensitive(terrain_path, "theater.ini");
        t.theater_files = collect_theater_files(terrain_path);

        // Display name: theater.ini [Theater].Title if present, else a
        // simple capitalize-first fallback.
        if (!t.theater_ini.empty()) {
            std::string title = read_theater_title(t.theater_ini);
            t.display_name = title.empty() ? capitalize_first(key) : title;
        } else {
            t.display_name = capitalize_first(key);
        }

        by_key[key] = std::move(t);
    }

    // Apply preferred ordering: listed keys first (in listed order, if
    // present on disk), then any remaining theaters alphabetically.
    out.reserve(by_key.size());
    for (const auto& key : preferred_order) {
        auto it = by_key.find(to_lower(key));
        if (it != by_key.end()) {
            out.push_back(std::move(it->second));
            by_key.erase(it);
        }
    }
    // Remaining (not in preferred_order): sort by key for stable output.
    std::vector<std::pair<std::string, Theater>> rest(by_key.begin(), by_key.end());
    std::sort(rest.begin(), rest.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto& [k, t] : rest) {
        out.push_back(std::move(t));
    }
    return out;
}

} // namespace f4::install
