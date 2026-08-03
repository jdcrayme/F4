// f4-world-viewer/src/settings.cpp
//
// Persisted viewer settings, read/written as a tiny JSON document.
// Refactored to use f4-json's Reader/Writer instead of the hand-rolled
// json_escape / json_unescape / extract_string_field helpers that lived
// here previously. The on-disk format is unchanged, so existing settings
// files from previous viewer versions load without migration.

#include <f4/viewer/settings.hpp>

#include <f4/json/f4_json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace f4::viewer {

namespace {

using f4::json::Reader;
using f4::json::Writer;

// Walk a flat JSON object and populate the settings struct. Unknown keys
// are skipped (forward-compat: if a future viewer adds a "theme" field,
// older viewers ignore it instead of failing). Returns false on any parse
// error — the caller treats the whole file as unreadable and falls back
// to defaults.
bool parse_settings_object(const std::string& json, ViewerSettings& s) {
    try {
        Reader r(json);
        r.skip_ws();
        r.expect('{');
        if (r.consume('}')) return true;  // empty object
        for (;;) {
            std::string key = r.read_string();
            r.expect(':');
            if      (key == "install_path")        s.install_path       = r.read_string();
            else if (key == "last_theater_key")    s.last_theater_key   = r.read_string();
            else if (key == "last_campaign_stem")  s.last_campaign_stem = r.read_string();
            else if (key == "last_world_json")     s.last_world_json    = r.read_string();
            else if (key == "last_terrain_json")   s.last_terrain_json  = r.read_string();
            else                                    r.skip_value();
            if (r.consume('}')) break;
            r.expect(',');
        }
        return true;
    } catch (const std::exception&) {
        return false;
    }
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
    Writer w;
    w.raw("{\n");
    w.raw("  \"install_path\": ");       w.string(s.install_path.string());        w.raw(",\n");
    w.raw("  \"last_theater_key\": ");   w.string(s.last_theater_key);             w.raw(",\n");
    w.raw("  \"last_campaign_stem\": "); w.string(s.last_campaign_stem);           w.raw(",\n");
    w.raw("  \"last_world_json\": ");    w.string(s.last_world_json.string());     w.raw(",\n");
    w.raw("  \"last_terrain_json\": ");  w.string(s.last_terrain_json.string());   w.raw("\n");
    w.raw("}\n");
    return w.str();
}

ViewerSettings settings_from_json(const std::string& json) {
    ViewerSettings s;
    parse_settings_object(json, s);
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
        ViewerSettings s;
        if (parse_settings_object(buf.str(), s)) return s;
        return {};
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
