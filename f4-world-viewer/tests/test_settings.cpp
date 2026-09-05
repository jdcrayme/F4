// f4-world-viewer/tests/test_settings.cpp
//
// Unit tests for the settings module — JSON parse/emit round-trip and
// the platform-specific path resolution. We test the JSON round-trip
// in detail (escape handling, missing fields, malformed input) and
// just touch the file I/O (load/save) since the on-disk location is
// platform-dependent.

#include <gtest/gtest.h>

#include <f4/viewer/settings.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

using namespace f4::viewer;

// Compatibility shim for MSVC++: setenv() and unsetenv() are POSIX, not
// Windows. We implement them only on Windows — on POSIX (Linux/macOS) the
// standard library already provides them and redeclaring them with a
// different return type causes a "ambiguating new declaration" compile error.
//
// unsetenv: _putenv_s(name, nullptr) is documented as "delete the variable"
// but the UCRT debug build asserts `value != nullptr` before reaching that
// code path (putenv.cpp:235 "Assertion failed: value != nullptr"). The
// bulletproof approach is the Win32 SetEnvironmentVariableA, which actually
// removes the variable when passed nullptr for the value.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef NOGDI
#define NOGDI
#endif
#ifndef NOUSER
#define NOUSER
#endif
#include <windows.h>

inline int setenv(const char* name, const char* value, int overwrite) {
    if (!overwrite) {
        // Check if variable exists (optional logic depending on desired behavior)
        if (getenv(name)) return 0; // Or return error if overwrite is false
    }
    // SetEnvironmentVariableA returns BOOL (1 on success, 0 on failure).
    // Match POSIX setenv's return convention: 0 on success, -1 on failure.
    return SetEnvironmentVariableA(name, value) ? 0 : -1;
}

inline int unsetenv(const char* name) {
    // Passing nullptr as the value removes the variable from the environment
    // block. This is the documented Win32 behavior and avoids the UCRT debug
    // assertion that _putenv_s(name, nullptr) triggers.
    return SetEnvironmentVariableA(name, nullptr) ? 0 : -1;
}
#endif

// ===========================================================================
// JSON round-trip — the workhorse tests
// ===========================================================================

TEST(SettingsJson, RoundTripEmpty) {
    ViewerSettings s;
    auto json = settings_to_json(s);
    auto s2 = settings_from_json(json);
    EXPECT_EQ(s, s2);
}

TEST(SettingsJson, RoundTripAllFieldsPopulated) {
    ViewerSettings s;
    s.install_path = "/path/to/falcon4";
    s.last_theater_key = "korea";
    s.last_campaign_stem = "save1";
    s.last_world_json = "/tmp/save1.world.json";
    s.last_terrain_json = "/tmp/korea.terrain.json";
    auto json = settings_to_json(s);
    auto s2 = settings_from_json(json);
    EXPECT_EQ(s, s2);
}

TEST(SettingsJson, HandlesSpacesInPaths) {
    ViewerSettings s;
    s.install_path = "/path/with spaces/Falcon 4.0";
    s.last_theater_key = "korea";
    s.last_campaign_stem = "save 1";
    auto json = settings_to_json(s);
    auto s2 = settings_from_json(json);
    EXPECT_EQ(s, s2);
}

TEST(SettingsJson, HandlesBackslashesInWindowsPaths) {
    ViewerSettings s;
    s.install_path = R"(C:\Program Files\Falcon 4.0)";
    s.last_world_json = R"(C:\Users\Bob\save1.world.json)";
    auto json = settings_to_json(s);
    auto s2 = settings_from_json(json);
    EXPECT_EQ(s, s2);
}

TEST(SettingsJson, HandlesUnicodeInPaths) {
    // Non-ASCII UTF-8 in paths — common on non-English Windows installs.
    ViewerSettings s;
    s.install_path = "/home/bob/Falcon 4.0 \xE6\x88\x98\xE6\x9C\xBA";  // "战机" = "fighter"
    auto json = settings_to_json(s);
    auto s2 = settings_from_json(json);
    EXPECT_EQ(s, s2);
}

TEST(SettingsJson, HandlesQuotesInPaths) {
    // Unlikely on real filesystems but the parser must not break.
    ViewerSettings s;
    s.last_campaign_stem = R"(save"1)";
    auto json = settings_to_json(s);
    auto s2 = settings_from_json(json);
    EXPECT_EQ(s, s2);
}

// ===========================================================================
// Missing fields — the parser must tolerate partial files
// ===========================================================================

TEST(SettingsJson, ParsesEmptyInstallPath) {
    auto json = R"({
        "install_path": "",
        "last_theater_key": "korea"
    })";
    auto s = settings_from_json(json);
    EXPECT_TRUE(s.install_path.empty());
    EXPECT_EQ(s.last_theater_key, "korea");
}

TEST(SettingsJson, MissingFieldsDefaultToEmpty) {
    auto json = R"({
        "install_path": "/path/to/falcon4"
    })";
    auto s = settings_from_json(json);
    EXPECT_EQ(s.install_path, "/path/to/falcon4");
    EXPECT_TRUE(s.last_theater_key.empty());
    EXPECT_TRUE(s.last_campaign_stem.empty());
    EXPECT_TRUE(s.last_world_json.empty());
    EXPECT_TRUE(s.last_terrain_json.empty());
}

TEST(SettingsJson, MalformedJsonReturnsDefaults) {
    // No quotes around value, unbalanced braces, etc. — the parser must
    // return defaults, not throw.
    auto s1 = settings_from_json("");
    auto s2 = settings_from_json("{");
    auto s3 = settings_from_json("not json at all");
    auto s4 = settings_from_json(R"({"install_path": /no/quotes})");
    EXPECT_TRUE(s1.install_path.empty());
    EXPECT_TRUE(s2.install_path.empty());
    EXPECT_TRUE(s3.install_path.empty());
    EXPECT_TRUE(s4.install_path.empty());
}

// ===========================================================================
// File I/O — load_settings + save_settings
// ===========================================================================

namespace {

/// RAII temp settings file. Overrides the platform-specific settings
/// directory env var (XDG_CONFIG_HOME on Linux, APPDATA on Windows) so
/// each test gets an isolated, throwaway settings dir.
///
/// On Windows, settings_dir() resolves to %APPDATA%/F4Viewer. Without
/// overriding APPDATA, tests would hit the real user settings dir and
/// see stale state from previous runs — causing the "file missing"
/// and "dir missing" tests to fail non-deterministically.
class TempSettings {
public:
    TempSettings() {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<int> dist(0, 99999999);
        temp_dir_ = (std::filesystem::temp_directory_path() /
                     ("f4-settings-test-" + std::to_string(dist(gen)))).string();
        std::filesystem::create_directories(temp_dir_);

#if defined(_WIN32)
        // Windows: settings_dir() reads APPDATA.
        if (const char* cur = std::getenv("APPDATA")) {
            old_env_ = cur;
            had_env_ = true;
        } else {
            had_env_ = false;
        }
        _putenv_s("APPDATA", temp_dir_.c_str());
#else
        // Linux/BSD: settings_dir() reads XDG_CONFIG_HOME.
        if (const char* cur = std::getenv("XDG_CONFIG_HOME")) {
            old_env_ = cur;
            had_env_ = true;
        } else {
            had_env_ = false;
        }
        setenv("XDG_CONFIG_HOME", temp_dir_.c_str(), 1);
#endif
    }
    ~TempSettings() {
#if defined(_WIN32)
        if (had_env_) {
            _putenv_s("APPDATA", old_env_.c_str());
        } else {
            _putenv_s("APPDATA", "");
        }
#else
        if (had_env_) {
            setenv("XDG_CONFIG_HOME", old_env_.c_str(), 1);
        } else {
            unsetenv("XDG_CONFIG_HOME");
        }
#endif
        std::error_code ec;
        std::filesystem::remove_all(temp_dir_, ec);
    }
    TempSettings(const TempSettings&) = delete;
    TempSettings& operator=(const TempSettings&) = delete;

private:
    std::string old_env_;
    bool had_env_ = false;
    std::string temp_dir_;
};

} // namespace

TEST(SettingsIO, SaveAndLoadRoundTrip) {
    TempSettings tmp;  // sets XDG_CONFIG_HOME for the duration

    ViewerSettings s;
    s.install_path = "/test/falcon4";
    s.last_theater_key = "balkans";
    s.last_campaign_stem = "save3";

    ASSERT_TRUE(save_settings(s));

    auto loaded = load_settings();
    EXPECT_EQ(loaded, s);
}

TEST(SettingsIO, LoadReturnsDefaultsWhenFileMissing) {
    TempSettings tmp;
    // Don't save — load should return defaults.
    auto loaded = load_settings();
    EXPECT_TRUE(loaded.install_path.empty());
    EXPECT_TRUE(loaded.last_theater_key.empty());
}

TEST(SettingsIO, LoadReturnsDefaultsOnCorruptFile) {
    TempSettings tmp;
    // Write garbage to the settings file.
    std::filesystem::create_directories(settings_dir());
    std::ofstream f(settings_file_path());
    f << "this is not json {{{";
    f.close();

    auto loaded = load_settings();
    EXPECT_TRUE(loaded.install_path.empty());
}

TEST(SettingsIO, SaveCreatesSettingsDirIfMissing) {
    TempSettings tmp;
    // Verify the dir doesn't exist initially.
    EXPECT_FALSE(std::filesystem::exists(settings_dir()));

    ViewerSettings s;
    s.install_path = "/test/falcon4";
    ASSERT_TRUE(save_settings(s));

    EXPECT_TRUE(std::filesystem::exists(settings_dir()));
    EXPECT_TRUE(std::filesystem::exists(settings_file_path()));
}

TEST(SettingsIO, SettingsFilePathIsUnderSettingsDir) {
    TempSettings tmp;
    auto path = settings_file_path();
    EXPECT_EQ(path.parent_path(), settings_dir());
    EXPECT_EQ(path.filename(), "settings.json");
}
