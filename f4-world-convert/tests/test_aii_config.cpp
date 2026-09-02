// f4-world-convert/tests/test_aii_config.cpp
//
// B.0 — Falcon4.AII reader tests: bubble-key precedence (FreeFalcon
// spellings + FILE_LAYOUT aliases), INI parsing semantics (comments,
// folding, duplicates, sections), documented defaults, loud failures.

#include <f4/world_convert/aii_config.hpp>

#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

namespace {

namespace wc = f4::world_convert;

// Fixture shipped next to this test (F4_AII — see fixtures/Falcon4.AII).
const std::filesystem::path fixture_path() {
    static const std::filesystem::path p =
        std::filesystem::path(FIXTURE_DIR) / "Falcon4.AII";
    return p;
}

// Write a small INI to a temp file and clean it up on destruction.
struct TempIni {
    std::filesystem::path path;
    explicit TempIni(const std::string& text)
        : path(std::filesystem::temp_directory_path() /
               ("f4_aii_test_" +
                std::to_string(std::random_device{}()) + ".ini")) {
        std::ofstream out(path, std::ios::binary);
        out << text;
    }
    ~TempIni() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    TempIni(const TempIni&) = delete;
    TempIni& operator=(const TempIni&) = delete;
};

TEST(AiiConfig, FixtureBubblesFromFreeFalconSpellings) {
    const auto aii = wc::AiiConfig::load(fixture_path());
    EXPECT_DOUBLE_EQ(aii.sim_bubble_size_grid(), 2.5);
    EXPECT_DOUBLE_EQ(aii.ground_bubble_size_grid(), 1.0);
}

TEST(AiiConfig, FixtureKeepsUndocumentedKeysReachable) {
    const auto aii = wc::AiiConfig::load(fixture_path());
    // Typed fields are only the two bubble settings — everything else
    // stays in the raw map (opt-in per key, no struct accretion).
    const auto* task = aii.lookup("ground", "MIN_TASK_GROUND");
    ASSERT_NE(task, nullptr);
    EXPECT_EQ(*task, "30");
    const auto* atc = aii.lookup("Atc", "MaxApproachSpacing");
    ASSERT_NE(atc, nullptr);
    EXPECT_EQ(*atc, "9000");
    // Case-insensitive folding on both section and key.
    EXPECT_NE(aii.lookup("GROUND", "min_task_ground"), nullptr);
    EXPECT_NE(aii.lookup("ATC", "maxapproachspacing"), nullptr);
    // Absent keys are nullptr, not an empty string.
    EXPECT_EQ(aii.lookup("Sim", "NO_SUCH_KEY"), nullptr);
    EXPECT_EQ(aii.lookup("NoSuchSection", "MinBubbleSize"), nullptr);
}

TEST(AiiConfig, FileLayoutSpellingsAreAcceptedAliases) {
    TempIni ini("[Sim]\n"
                "SIM_BUBBLE_SIZE = 3.0\n"
                "GROUND_BUBBLE_SIZE = 0.5\n");
    const auto aii = wc::AiiConfig::load(ini.path);
    EXPECT_DOUBLE_EQ(aii.sim_bubble_size_grid(), 3.0);
    EXPECT_DOUBLE_EQ(aii.ground_bubble_size_grid(), 0.5);
}

TEST(AiiConfig, FreeFalconSpellingWinsOverAlias) {
    // Both spellings present with different values: the FreeFalcon-source
    // name (MinBubbleSize / BubbleRatioToUnitSpan) is primary.
    TempIni ini("[Sim]\n"
                "MinBubbleSize = 2.5\n"
                "SIM_BUBBLE_SIZE = 9.0\n"
                "BubbleRatioToUnitSpan = 1.0\n"
                "GROUND_BUBBLE_SIZE = 9.0\n");
    const auto aii = wc::AiiConfig::load(ini.path);
    EXPECT_DOUBLE_EQ(aii.sim_bubble_size_grid(), 2.5);
    EXPECT_DOUBLE_EQ(aii.ground_bubble_size_grid(), 1.0);
}

TEST(AiiConfig, KeysOutOfSimSectionFoundByScan) {
    // Hand-edited AII files drift keys out of [Sim]; the last-resort scan
    // finds them (deterministic: folded section-name order).
    TempIni ini("[Drifted]\n"
                "SIM_BUBBLE_SIZE = 4.0\n");
    const auto aii = wc::AiiConfig::load(ini.path);
    EXPECT_DOUBLE_EQ(aii.sim_bubble_size_grid(), 4.0);
    // The untouched setting keeps its default.
    EXPECT_DOUBLE_EQ(aii.ground_bubble_size_grid(), 1.0);
}

TEST(AiiConfig, MissingKeysKeepDocumentedDefaults) {
    // A file that tunes OTHER parameters only is valid — the two bubble
    // settings resolve to the documented defaults.
    TempIni ini("[Atc]\n"
                "MaxApproachSpacing = 9000\n");
    const auto aii = wc::AiiConfig::load(ini.path);
    EXPECT_DOUBLE_EQ(aii.sim_bubble_size_grid(), 2.5);
    EXPECT_DOUBLE_EQ(aii.ground_bubble_size_grid(), 1.0);
}

TEST(AiiConfig, EmptyFileIsValidAllDefaults) {
    TempIni ini("");
    const auto aii = wc::AiiConfig::load(ini.path);
    EXPECT_DOUBLE_EQ(aii.sim_bubble_size_grid(), 2.5);
    EXPECT_DOUBLE_EQ(aii.ground_bubble_size_grid(), 1.0);
    EXPECT_TRUE(aii.sections().empty());
}

TEST(AiiConfig, CommentsAndDuplicateKeysLastWins) {
    TempIni ini("; full-line comment\n"
                "[Sim]   ; trailing comment after a section header\n"
                "MinBubbleSize = 1.0 ; inline comment\n"
                "MinBubbleSize = 2.0 ; duplicate — last wins\n"
                "\n"
                "\t\n"); // whitespace-only lines are skipped
    const auto aii = wc::AiiConfig::load(ini.path);
    EXPECT_DOUBLE_EQ(aii.sim_bubble_size_grid(), 2.0);
}

TEST(AiiConfig, MalformedLinesFailLoudlyWithLineNumbers) {
    // A bare word (no '=').
    {
        TempIni ini("[Sim]\nMinBubbleSize = 2.5\nGARBAGE\n");
        EXPECT_THROW(
            {
                try {
                    wc::AiiConfig::load(ini.path);
                } catch (const std::runtime_error& e) {
                    EXPECT_NE(std::string(e.what()).find(":3:"),
                              std::string::npos)
                        << e.what();
                    throw;
                }
            },
            std::runtime_error);
    }
    // A key with no section above it.
    {
        TempIni ini("MinBubbleSize = 2.5\n");
        EXPECT_THROW(
            {
                try {
                    wc::AiiConfig::load(ini.path);
                } catch (const std::runtime_error& e) {
                    EXPECT_NE(std::string(e.what()).find("before any"),
                              std::string::npos)
                        << e.what();
                    throw;
                }
            },
            std::runtime_error);
    }
    // An unterminated section header.
    {
        TempIni ini("[Sim\nMinBubbleSize = 2.5\n");
        EXPECT_THROW(
            {
                try {
                    wc::AiiConfig::load(ini.path);
                } catch (const std::runtime_error& e) {
                    EXPECT_NE(std::string(e.what()).find("unterminated"),
                              std::string::npos)
                        << e.what();
                    throw;
                }
            },
            std::runtime_error);
    }
    // Trailing junk after the section header.
    {
        TempIni ini("[Sim] junk\n");
        EXPECT_THROW(
            {
                try {
                    wc::AiiConfig::load(ini.path);
                } catch (const std::runtime_error& e) {
                    EXPECT_NE(std::string(e.what()).find("trailing junk"),
                              std::string::npos)
                        << e.what();
                    throw;
                }
            },
            std::runtime_error);
    }
    // An empty key.
    {
        TempIni ini("[Sim]\n = 2.5\n");
        EXPECT_THROW(
            {
                try {
                    wc::AiiConfig::load(ini.path);
                } catch (const std::runtime_error& e) {
                    EXPECT_NE(std::string(e.what()).find("empty key"),
                              std::string::npos)
                        << e.what();
                    throw;
                }
            },
            std::runtime_error);
    }
}

TEST(AiiConfig, NonNumericBubbleValueFailsLoudly) {
    // A bubble size we silently defaulted would quietly change deagg
    // geometry — that failure must be loud, never defaulted.
    TempIni ini("[Sim]\nSIM_BUBBLE_SIZE = auto\n");
    EXPECT_THROW(
        {
            try {
                wc::AiiConfig::load(ini.path);
            } catch (const std::runtime_error& e) {
                EXPECT_NE(std::string(e.what()).find("not a number"),
                          std::string::npos)
                    << e.what();
                throw;
            }
        },
        std::runtime_error);
}

TEST(AiiConfig, LoadIfExistsAbsentPathKeepsFallback) {
    // Empty path — the "not configured" case every pre-B.0 caller runs.
    const auto empty = wc::AiiConfig::load_if_exists({});
    EXPECT_DOUBLE_EQ(empty.sim_bubble_size_grid(), 2.5);
    EXPECT_DOUBLE_EQ(empty.ground_bubble_size_grid(), 1.0);

    // Missing file — same.
    const auto missing = wc::AiiConfig::load_if_exists(
        std::filesystem::temp_directory_path() /
        "f4_aii_no_such_file_9x7z.aII");
    EXPECT_DOUBLE_EQ(missing.sim_bubble_size_grid(), 2.5);
    EXPECT_DOUBLE_EQ(missing.ground_bubble_size_grid(), 1.0);

    // A caller-supplied fallback propagates (here: identical defaults —
    // the point is the fallback path returns it untouched).
    const auto custom = wc::AiiConfig::load_if_exists(
        std::filesystem::temp_directory_path() / "f4_aii_no_such_file_9x7z.aII",
        wc::AiiConfig{});
    EXPECT_DOUBLE_EQ(custom.sim_bubble_size_grid(), 2.5);
}

TEST(AiiConfig, LoadIfExistsPresentFileStillParses) {
    TempIni ini("[Sim]\nMinBubbleSize = 3.25\n");
    const auto aii = wc::AiiConfig::load_if_exists(ini.path);
    EXPECT_DOUBLE_EQ(aii.sim_bubble_size_grid(), 3.25);
}

TEST(AiiConfig, MissingFileThrowsFromLoad) {
    // load() (unlike load_if_exists) is loud about a missing file.
    EXPECT_THROW(wc::AiiConfig::load(
                     std::filesystem::temp_directory_path() /
                     "f4_aii_no_such_file_9x7z.aII"),
                 std::runtime_error);
}

} // namespace
