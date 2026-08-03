// f4-world-viewer/src/settings.cpp

#include <f4/viewer/settings.hpp>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace f4::viewer {

namespace {

/// Escape a string for embedding in JSON. Handles backslash, quote, and
/// the common control chars. We don't bother with Unicode escapes —
/// paths on disk are byte sequences and we'll round-trip them as UTF-8.
std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                // Pass through printable ASCII and all bytes >= 0x80
                // (UTF-8 continuation bytes — preserves paths with
                // non-ASCII characters without breaking JSON validity).
                if (static_cast<unsigned char>(c) < 0x20) {
                    // Other control chars: \u00XX escape.
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
                break;
        }
    }
    return out;
}

/// Unescape a JSON string value (the content between the quotes).
/// Returns false on malformed escape sequence — caller treats as parse
/// failure for the whole field.
bool json_unescape(std::string& s) {
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\') continue;
        if (i + 1 >= s.size()) return false;
        char esc = s[i + 1];
        std::size_t consume = 2;
        switch (esc) {
            case '\\': s[i] = '\\'; break;
            case '"':  s[i] = '"';  break;
            case '/':  s[i] = '/';  break;  // optional JSON escape
            case 'b':  s[i] = '\b'; break;
            case 'f':  s[i] = '\f'; break;
            case 'n':  s[i] = '\n'; break;
            case 'r':  s[i] = '\r'; break;
            case 't':  s[i] = '\t'; break;
            case 'u': {
                // \uXXXX — we only handle ASCII range (XXXX <= 0x7F).
                // Higher code points would need surrogate pair handling;
                // paths in practice don't contain \u escapes.
                if (i + 5 >= s.size()) return false;
                char buf[5] = {s[i+2], s[i+3], s[i+4], s[i+5], 0};
                char* end = nullptr;
                long code = std::strtol(buf, &end, 16);
                if (end != buf + 4) return false;
                s[i] = static_cast<char>(code);
                consume = 6;
                break;
            }
            default: return false;
        }
        s.erase(i + 1, consume - 1);
    }
    return true;
}

/// Extract the string value of "key" from a flat JSON object. Returns
/// empty string if not found or malformed. Only handles top-level
/// string fields (no nested objects/arrays) — sufficient for our
/// settings schema.
std::string extract_string_field(const std::string& json,
                                  const std::string& key) {
    // Build the search pattern: "key" :
    std::string needle = "\"" + key + "\"";
    std::size_t pos = 0;
    while ((pos = json.find(needle, pos)) != std::string::npos) {
        pos += needle.size();
        // Skip whitespace.
        while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
        if (pos >= json.size() || json[pos] != ':') continue;
        ++pos;
        while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
        if (pos >= json.size() || json[pos] != '"') continue;
        ++pos;  // skip opening quote
        // Find the closing quote (handling escaped quotes).
        std::string value;
        while (pos < json.size() && json[pos] != '"') {
            if (json[pos] == '\\' && pos + 1 < json.size()) {
                value += json[pos];
                value += json[pos + 1];
                pos += 2;
            } else {
                value += json[pos];
                ++pos;
            }
        }
        if (pos >= json.size()) return {};  // unterminated string
        // Successfully extracted a value — unescape and return.
        if (!json_unescape(value)) return {};
        return value;
    }
    return {};
}

} // namespace

std::filesystem::path settings_dir() {
    namespace fs = std::filesystem;
#if defined(_WIN32)
    const char* appdata = std::getenv("APPDATA");
    if (appdata && *appdata) {
        return fs::path(appdata) / "F4Viewer";
    }
    // Fallback to user profile if APPDATA isn't set.
    const char* userprofile = std::getenv("USERPROFILE");
    if (userprofile && *userprofile) {
        return fs::path(userprofile) / "AppData" / "Roaming" / "F4Viewer";
    }
    return fs::current_path() / "F4Viewer";
#elif defined(__APPLE__)
    const char* home = std::getenv("HOME");
    if (home && *home) {
        return fs::path(home) / "Library" / "Application Support" / "f4-viewer";
    }
    return fs::current_path() / "f4-viewer";
#else
    // Linux / BSD / other Unix: respect XDG_CONFIG_HOME.
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        return fs::path(xdg) / "f4-viewer";
    }
    const char* home = std::getenv("HOME");
    if (home && *home) {
        return fs::path(home) / ".config" / "f4-viewer";
    }
    return fs::current_path() / "f4-viewer";
#endif
}

std::filesystem::path settings_file_path() {
    return settings_dir() / "settings.json";
}

std::string settings_to_json(const ViewerSettings& s) {
    std::ostringstream ss;
    ss << "{\n";
    ss << "  \"install_path\": \"" << json_escape(s.install_path.string()) << "\",\n";
    ss << "  \"last_theater_key\": \"" << json_escape(s.last_theater_key) << "\",\n";
    ss << "  \"last_campaign_stem\": \"" << json_escape(s.last_campaign_stem) << "\",\n";
    ss << "  \"last_world_json\": \"" << json_escape(s.last_world_json.string()) << "\",\n";
    ss << "  \"last_terrain_json\": \"" << json_escape(s.last_terrain_json.string()) << "\"\n";
    ss << "}\n";
    return ss.str();
}

ViewerSettings settings_from_json(const std::string& json) {
    ViewerSettings s;
    s.install_path        = extract_string_field(json, "install_path");
    s.last_theater_key    = extract_string_field(json, "last_theater_key");
    s.last_campaign_stem  = extract_string_field(json, "last_campaign_stem");
    s.last_world_json     = extract_string_field(json, "last_world_json");
    s.last_terrain_json   = extract_string_field(json, "last_terrain_json");
    return s;
}

ViewerSettings load_settings() {
    try {
        const auto path = settings_file_path();
        if (!std::filesystem::exists(path)) return {};
        std::ifstream f(path);
        if (!f) return {};
        std::stringstream buf;
        buf << f.rdbuf();
        return settings_from_json(buf.str());
    } catch (const std::exception&) {
        // Corrupted or unreadable — return defaults. The viewer must
        // never fail to start because of a settings file.
        return {};
    }
}

bool save_settings(const ViewerSettings& s) {
    try {
        const auto dir = settings_dir();
        std::filesystem::create_directories(dir);
        const auto path = settings_file_path();
        std::ofstream f(path);
        if (!f) return false;
        f << settings_to_json(s);
        return f.good();
    } catch (const std::exception&) {
        return false;
    }
}

} // namespace f4::viewer
