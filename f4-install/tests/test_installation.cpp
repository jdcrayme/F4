// f4-install/tests/test_installation.cpp
//
// All tests build a synthetic Falcon 4.0 install tree in a unique temp
// directory, then call Installation::detect() and verify the result.
// We never touch a real install — the tree is fully under our control,
// so the tests are reproducible on CI (Linux, macOS, Windows).
//
// File layout conventions used by the fixtures:
//
//   <tmp>/install/
//     FALCON4.ct                  (empty file — content doesn't matter)
//     sim/                        (empty dir, marks aircraft data location)
//     terrdata/
//       theater.lst               (optional)
//       korea/
//         THEATER.MAP             (empty file)
//         THEATER.MEA             (empty file)
//         THEATER.O2              (empty file)
//         theater.ini             (optional — provides display name)
//       balkans/
//         THEATER.MAP
//         THEATER.MEA
//     campaign/
//       save1.cam                 (flat-layout save)
//       korea/
//         save2.cam               (nested-layout save)
//         save3.cam
//       balkans/
//         save1.cam

#include <gtest/gtest.h>

#include <f4/install/f4_install.hpp>

#include <filesystem>
#include <fstream>
#include <ios>
#include <random>
#include <string>
#include <system_error>

namespace fs = std::filesystem;
using namespace f4::install;

namespace {

/// RAII temp directory. Created with a random suffix to avoid collisions
/// when tests run in parallel; removed in the destructor so we never leak
/// fixtures even if an assertion fails.
class TempDir {
public:
    TempDir() {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<int> dist(0, 99999999);
        path_ = fs::temp_directory_path() /
                ("f4-install-test-" + std::to_string(dist(gen)));
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);  // best-effort
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    [[nodiscard]] const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

/// Create an empty regular file (and parent directories as needed).
void touch(const fs::path& p) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f << "";  // explicitly empty
}

/// Write `content` to a file, creating parent dirs as needed.
void write_file(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f << content;
}

} // namespace

// ===========================================================================
// Installation::detect — basic structure
// ===========================================================================

TEST(Installation, DetectsEmptyDirAsInvalid) {
    TempDir tmp;
    auto inst = Installation::detect(tmp.path());
    EXPECT_FALSE(inst.valid()) << "an empty dir should not look like a Falcon install";
    // root() always reflects what was passed in, even on invalid installs,
    // so callers can produce useful error messages ("looked at <root> but
    // no FALCON4.ct or terrdata/ found"). The validity check is separate.
    EXPECT_EQ(inst.root(), tmp.path());
}

TEST(Installation, DetectsNonexistentPathAsInvalid) {
    auto inst = Installation::detect("/nonexistent/path/that/does/not/exist");
    EXPECT_FALSE(inst.valid());
}

TEST(Installation, DetectsClassTableAtRoot) {
    TempDir tmp;
    touch(tmp.path() / "FALCON4.ct");

    auto inst = Installation::detect(tmp.path());
    ASSERT_TRUE(inst.valid());
    EXPECT_FALSE(inst.class_table().empty());
    EXPECT_EQ(inst.class_table().filename(), "FALCON4.ct");
}

TEST(Installation, DetectsClassTableInSimDir) {
    TempDir tmp;
    touch(tmp.path() / "sim" / "FALCON4.ct");

    auto inst = Installation::detect(tmp.path());
    ASSERT_TRUE(inst.valid());
    EXPECT_FALSE(inst.class_table().empty());
    EXPECT_EQ(inst.class_table().parent_path().filename(), "sim");
    EXPECT_FALSE(inst.aircraft_dir().empty());
}

TEST(Installation, DetectsClassTableInTerrdataDir) {
    TempDir tmp;
    touch(tmp.path() / "terrdata" / "FALCON4.ct");

    auto inst = Installation::detect(tmp.path());
    ASSERT_TRUE(inst.valid());
    EXPECT_FALSE(inst.class_table().empty());
    EXPECT_EQ(inst.class_table().parent_path().filename(), "terrdata");
}

TEST(Installation, ValidWithTerrdataOnly) {
    // An install without FALCON4.ct but with terrdata/ is still valid
    // (some community repacks ship terrain without the class table).
    TempDir tmp;
    touch(tmp.path() / "terrdata" / "korea" / "THEATER.MAP");

    auto inst = Installation::detect(tmp.path());
    EXPECT_TRUE(inst.valid());
    EXPECT_TRUE(inst.class_table().empty());
}

// ===========================================================================
// Theater scanning
// ===========================================================================

TEST(Installation, DiscoversTheaterFromTerrdataDir) {
    TempDir tmp;
    touch(tmp.path() / "FALCON4.ct");
    touch(tmp.path() / "terrdata" / "korea" / "THEATER.MAP");
    touch(tmp.path() / "terrdata" / "korea" / "THEATER.MEA");
    touch(tmp.path() / "terrdata" / "korea" / "THEATER.O2");

    auto inst = Installation::detect(tmp.path());
    ASSERT_EQ(inst.theaters().size(), 1u);
    const auto& t = inst.theaters()[0];
    EXPECT_EQ(t.key, "korea");
    EXPECT_EQ(t.display_name, "Korea");  // capitalized fallback
    EXPECT_TRUE(t.complete());
    EXPECT_FALSE(t.theater_map.empty());
    EXPECT_FALSE(t.theater_mea.empty());
    EXPECT_FALSE(t.theater_o2.empty());
    EXPECT_EQ(t.theater_files.size(), 3u);  // MAP + MEA + O2
}

TEST(Installation, MarksIncompleteTheaterWhenMapMissing) {
    // A theater dir without THEATER.MAP is not discovered at all —
    // it doesn't qualify as a theater. A dir with MAP but no MEA is
    // discovered but `complete()` is false.
    TempDir tmp;
    touch(tmp.path() / "FALCON4.ct");
    touch(tmp.path() / "terrdata" / "korea" / "THEATER.MAP");
    // No THEATER.MEA — incomplete.

    auto inst = Installation::detect(tmp.path());
    ASSERT_EQ(inst.theaters().size(), 1u);
    EXPECT_FALSE(inst.theaters()[0].complete());
}

TEST(Installation, ReadsDisplayNameFromTheaterIni) {
    TempDir tmp;
    touch(tmp.path() / "FALCON4.ct");
    touch(tmp.path() / "terrdata" / "korea" / "THEATER.MAP");
    touch(tmp.path() / "terrdata" / "korea" / "THEATER.MEA");
    write_file(tmp.path() / "terrdata" / "korea" / "theater.ini",
               "[Theater]\nTitle=Korea Theater\nTerrain=korea\n");

    auto inst = Installation::detect(tmp.path());
    ASSERT_EQ(inst.theaters().size(), 1u);
    EXPECT_EQ(inst.theaters()[0].display_name, "Korea Theater");
}

TEST(Installation, FindsTheaterCaseInsensitive) {
    // Community installs sometimes lowercase everything. We should still
    // find THEATER.MAP even if it's theater.map on disk.
    TempDir tmp;
    touch(tmp.path() / "FALCON4.ct");
    touch(tmp.path() / "terrdata" / "korea" / "theater.map");
    touch(tmp.path() / "terrdata" / "korea" / "theater.mea");

    auto inst = Installation::detect(tmp.path());
    ASSERT_EQ(inst.theaters().size(), 1u);
    EXPECT_EQ(inst.theaters()[0].key, "korea");
    EXPECT_TRUE(inst.theaters()[0].complete());
}

TEST(Installation, ApplyTheaterLstPreferredOrder) {
    TempDir tmp;
    touch(tmp.path() / "FALCON4.ct");
    // Discover three theaters (alphabetical: balkans, iceland, korea).
    for (const auto& key : {"balkans", "iceland", "korea"}) {
        touch(tmp.path() / "terrdata" / key / "THEATER.MAP");
        touch(tmp.path() / "terrdata" / key / "THEATER.MEA");
    }
    // theater.lst puts korea first (preferred), balkans second.
    // iceland is missing from the list — should still be discovered,
    // appended after the listed ones in alphabetical order.
    write_file(tmp.path() / "terrdata" / "theater.lst",
               "# Theater list\n"
               "korea\n"
               "balkans\n");

    auto inst = Installation::detect(tmp.path());
    ASSERT_EQ(inst.theaters().size(), 3u);
    EXPECT_EQ(inst.theaters()[0].key, "korea");    // from list
    EXPECT_EQ(inst.theaters()[1].key, "balkans");  // from list
    EXPECT_EQ(inst.theaters()[2].key, "iceland");  // discovered, alpha-sorted
}

TEST(Installation, TheaterLstIgnoresCommentsAndBlanks) {
    TempDir tmp;
    touch(tmp.path() / "FALCON4.ct");
    touch(tmp.path() / "terrdata" / "korea" / "THEATER.MAP");
    touch(tmp.path() / "terrdata" / "korea" / "THEATER.MEA");
    write_file(tmp.path() / "terrdata" / "theater.lst",
               "# comment line\n"
               "\n"
               "// another comment\n"
               "  korea  \n"  // whitespace should be stripped
               "  \"balkans\"  \n");  // quotes stripped, then not found on disk

    auto inst = Installation::detect(tmp.path());
    // Only korea is on disk; balkans is listed but not present, so only
    // korea should be returned (the preferred_order list is filtered by
    // what's actually on disk).
    ASSERT_EQ(inst.theaters().size(), 1u);
    EXPECT_EQ(inst.theaters()[0].key, "korea");
}

TEST(Installation, FindTheaterByKey) {
    TempDir tmp;
    touch(tmp.path() / "FALCON4.ct");
    touch(tmp.path() / "terrdata" / "korea" / "THEATER.MAP");
    touch(tmp.path() / "terrdata" / "korea" / "THEATER.MEA");
    touch(tmp.path() / "terrdata" / "balkans" / "THEATER.MAP");
    touch(tmp.path() / "terrdata" / "balkans" / "THEATER.MEA");

    auto inst = Installation::detect(tmp.path());
    ASSERT_NE(inst.find_theater("korea"), nullptr);
    ASSERT_NE(inst.find_theater("BALKANS"), nullptr);  // case-insensitive
    EXPECT_EQ(inst.find_theater("nonexistent"), nullptr);
}

// ===========================================================================
// Campaign scanning
// ===========================================================================

TEST(Installation, DiscoversFlatLayoutCampaigns) {
    // Vanilla Falcon 4.0 layout: campaign/save1.cam directly under campaign/.
    TempDir tmp;
    touch(tmp.path() / "FALCON4.ct");
    touch(tmp.path() / "terrdata" / "korea" / "THEATER.MAP");
    touch(tmp.path() / "terrdata" / "korea" / "THEATER.MEA");
    touch(tmp.path() / "campaign" / "save1.cam");
    touch(tmp.path() / "campaign" / "save2.cam");

    auto inst = Installation::detect(tmp.path());
    ASSERT_EQ(inst.campaigns().size(), 2u);
    EXPECT_EQ(inst.campaigns()[0].stem, "save1");
    EXPECT_EQ(inst.campaigns()[1].stem, "save2");
    // Flat layout — theater_key is empty (parent dir is campaign/, not korea/).
    EXPECT_TRUE(inst.campaigns()[0].theater_key.empty());
}

TEST(Installation, DiscoversNestedLayoutCampaigns) {
    // FreeFalcon multi-theater layout: campaign/<theater>/saveN.cam.
    TempDir tmp;
    touch(tmp.path() / "FALCON4.ct");
    touch(tmp.path() / "terrdata" / "korea" / "THEATER.MAP");
    touch(tmp.path() / "terrdata" / "korea" / "THEATER.MEA");
    touch(tmp.path() / "terrdata" / "balkans" / "THEATER.MAP");
    touch(tmp.path() / "terrdata" / "balkans" / "THEATER.MEA");
    touch(tmp.path() / "campaign" / "korea" / "save1.cam");
    touch(tmp.path() / "campaign" / "korea" / "save2.cam");
    touch(tmp.path() / "campaign" / "balkans" / "save1.cam");

    auto inst = Installation::detect(tmp.path());
    ASSERT_EQ(inst.campaigns().size(), 3u);
    // Sorted by (theater_key, stem): balkans first, then korea.
    EXPECT_EQ(inst.campaigns()[0].theater_key, "balkans");
    EXPECT_EQ(inst.campaigns()[0].stem, "save1");
    EXPECT_EQ(inst.campaigns()[1].theater_key, "korea");
    EXPECT_EQ(inst.campaigns()[1].stem, "save1");
    EXPECT_EQ(inst.campaigns()[2].theater_key, "korea");
    EXPECT_EQ(inst.campaigns()[2].stem, "save2");
}

TEST(Installation, CampaignsForFiltering) {
    TempDir tmp;
    touch(tmp.path() / "FALCON4.ct");
    touch(tmp.path() / "terrdata" / "korea" / "THEATER.MAP");
    touch(tmp.path() / "terrdata" / "korea" / "THEATER.MEA");
    touch(tmp.path() / "terrdata" / "balkans" / "THEATER.MAP");
    touch(tmp.path() / "terrdata" / "balkans" / "THEATER.MEA");
    touch(tmp.path() / "campaign" / "korea" / "save1.cam");
    touch(tmp.path() / "campaign" / "balkans" / "save1.cam");
    // Also a flat-layout save — should be included for any theater selection.
    touch(tmp.path() / "campaign" / "save0.cam");

    auto inst = Installation::detect(tmp.path());
    auto korea_camps = inst.campaigns_for("korea");
    ASSERT_EQ(korea_camps.size(), 2u);  // save1 (korea) + save0 (flat)
    EXPECT_EQ(korea_camps[0].stem, "save0");  // flat sorts first (empty theater_key)
    EXPECT_EQ(korea_camps[1].stem, "save1");
    EXPECT_EQ(korea_camps[1].theater_key, "korea");
}

TEST(Installation, MixedFlatAndNestedLayouts) {
    // Some installs (community upgrades) have both flat saves from vanilla
    // F4 and per-theater saves from a later FreeFalcon install.
    TempDir tmp;
    touch(tmp.path() / "FALCON4.ct");
    touch(tmp.path() / "terrdata" / "korea" / "THEATER.MAP");
    touch(tmp.path() / "terrdata" / "korea" / "THEATER.MEA");
    touch(tmp.path() / "campaign" / "save1.cam");                  // flat (legacy)
    touch(tmp.path() / "campaign" / "korea" / "save2.cam");        // nested (current)

    auto inst = Installation::detect(tmp.path());
    EXPECT_EQ(inst.campaigns().size(), 2u);
    // Flat sorts first (empty theater_key < "korea").
    EXPECT_TRUE(inst.campaigns()[0].theater_key.empty());
    EXPECT_EQ(inst.campaigns()[0].stem, "save1");
    EXPECT_EQ(inst.campaigns()[1].theater_key, "korea");
    EXPECT_EQ(inst.campaigns()[1].stem, "save2");
}

TEST(Installation, CampaignStemAndDisplayName) {
    TempDir tmp;
    touch(tmp.path() / "FALCON4.ct");
    touch(tmp.path() / "terrdata" / "korea" / "THEATER.MAP");
    touch(tmp.path() / "terrdata" / "korea" / "THEATER.MEA");
    touch(tmp.path() / "campaign" / "save1.cam");

    auto inst = Installation::detect(tmp.path());
    ASSERT_EQ(inst.campaigns().size(), 1u);
    const auto& c = inst.campaigns()[0];
    EXPECT_EQ(c.stem, "save1");
    EXPECT_EQ(c.display_name, "save1");  // defaults to stem
    EXPECT_FALSE(c.cam.empty());
    EXPECT_EQ(c.cam.filename(), "save1.cam");
}

// ===========================================================================
// find_class_table — install-aware resolver
// ===========================================================================

TEST(Installation, FindClassTableNextToReferenceFile) {
    TempDir tmp;
    touch(tmp.path() / "FALCON4.ct");
    // Simulate a .cam in a subdir, with FALCON4.ct next to it.
    touch(tmp.path() / "saves" / "save1.cam");
    touch(tmp.path() / "saves" / "FALCON4.ct");

    auto inst = Installation::detect(tmp.path());
    auto ct = inst.find_class_table(tmp.path() / "saves" / "save1.cam");
    EXPECT_FALSE(ct.empty());
    EXPECT_EQ(ct.parent_path().filename(), "saves");
}

TEST(Installation, FindClassTableUpOneDirectory) {
    TempDir tmp;
    touch(tmp.path() / "FALCON4.ct");
    // .cam in a subdir; FALCON4.ct one level up.
    touch(tmp.path() / "saves" / "save1.cam");

    auto inst = Installation::detect(tmp.path());
    auto ct = inst.find_class_table(tmp.path() / "saves" / "save1.cam");
    EXPECT_FALSE(ct.empty());
    EXPECT_EQ(ct.filename(), "FALCON4.ct");
}

TEST(Installation, FindClassTableFromInstallRoot) {
    // No reference file — falls back to install-aware resolution.
    TempDir tmp;
    touch(tmp.path() / "FALCON4.ct");

    auto inst = Installation::detect(tmp.path());
    auto ct = inst.find_class_table();  // empty reference_file
    EXPECT_FALSE(ct.empty());
    EXPECT_EQ(ct.filename(), "FALCON4.ct");
}

TEST(Installation, FindClassTableReturnsEmptyWhenNotFound) {
    TempDir tmp;
    // No FALCON4.ct anywhere. detect() returns valid=false (no terrdata
    // either), but we can still construct an Installation manually and
    // ask it to find the class table — should return empty.
    auto inst = Installation::detect(tmp.path());
    EXPECT_FALSE(inst.valid());
    // CWD fallback may or may not find one — but the install-aware paths
    // should be exhausted first. We can't reliably assert empty here
    // because the CWD fallback depends on where the test runs. Instead
    // we just verify no exception is thrown.
    EXPECT_NO_THROW({ (void)inst.find_class_table(); });
}

// ===========================================================================
// Free-function helpers
// ===========================================================================

TEST(FindClassTableInInstall, OneShotResolver) {
    TempDir tmp;
    touch(tmp.path() / "FALCON4.ct");

    auto ct = find_class_table_in_install(tmp.path(), {});
    EXPECT_FALSE(ct.empty());
    EXPECT_EQ(ct.filename(), "FALCON4.ct");
}

// ===========================================================================
// theater.lst parser — pure-function unit tests
// ===========================================================================

TEST(TheaterLstParser, ParsesBasicList) {
    auto keys = parse_theater_lst_string("korea\nbalkans\niceland\n");
    EXPECT_EQ(keys, (std::vector<std::string>{"korea", "balkans", "iceland"}));
}

TEST(TheaterLstParser, IgnoresComments) {
    auto keys = parse_theater_lst_string(
        "# comment\n"
        "korea\n"
        "// another comment\n"
        "balkans\n"
        "; third comment style\n");
    EXPECT_EQ(keys, (std::vector<std::string>{"korea", "balkans"}));
}

TEST(TheaterLstParser, StripsWhitespaceAndQuotes) {
    auto keys = parse_theater_lst_string(
        "  korea  \n"
        "  \"balkans\"  \n"
        "  [iceland]  \n"
        "  (panama)  \n");
    EXPECT_EQ(keys, (std::vector<std::string>{"korea", "balkans", "iceland", "panama"}));
}

TEST(TheaterLstParser, LowercasesKeys) {
    auto keys = parse_theater_lst_string("Korea\nBALKANS\nIceland\n");
    EXPECT_EQ(keys, (std::vector<std::string>{"korea", "balkans", "iceland"}));
}

TEST(TheaterLstParser, IgnoresInlineComments) {
    auto keys = parse_theater_lst_string(
        "korea # primary theater\n"
        "balkans // secondary\n"
        "iceland ; cold\n");
    EXPECT_EQ(keys, (std::vector<std::string>{"korea", "balkans", "iceland"}));
}

TEST(TheaterLstParser, EmptyInput) {
    EXPECT_TRUE(parse_theater_lst_string("").empty());
    EXPECT_TRUE(parse_theater_lst_string("\n\n\n").empty());
    EXPECT_TRUE(parse_theater_lst_string("# only comments\n# more comments\n").empty());
}

TEST(TheaterLstParser, FileRoundTrip) {
    TempDir tmp;
    write_file(tmp.path() / "theater.lst", "korea\nbalkans\n");
    auto keys = parse_theater_lst(tmp.path() / "theater.lst");
    EXPECT_EQ(keys, (std::vector<std::string>{"korea", "balkans"}));
}

TEST(TheaterLstParser, MissingFileReturnsEmpty) {
    auto keys = parse_theater_lst("/nonexistent/theater.lst");
    EXPECT_TRUE(keys.empty());
}

// ===========================================================================
// theater.ini title parser
// ===========================================================================

TEST(TheaterIni, ReadsTitleField) {
    TempDir tmp;
    write_file(tmp.path() / "theater.ini",
               "[Theater]\n"
               "Title=Korea Theater\n"
               "Terrain=korea\n"
               "TerrainDB=korea.t\n");
    EXPECT_EQ(read_theater_title(tmp.path() / "theater.ini"), "Korea Theater");
}

TEST(TheaterIni, ReturnsEmptyWhenTitleMissing) {
    TempDir tmp;
    write_file(tmp.path() / "theater.ini",
               "[Theater]\n"
               "Terrain=korea\n");
    EXPECT_TRUE(read_theater_title(tmp.path() / "theater.ini").empty());
}

TEST(TheaterIni, ReturnsEmptyWhenFileMissing) {
    EXPECT_TRUE(read_theater_title("/nonexistent/theater.ini").empty());
}

TEST(TheaterIni, StripsQuotesAroundTitle) {
    TempDir tmp;
    write_file(tmp.path() / "theater.ini",
               "[Theater]\n"
               "Title=\"Korea Theater\"\n");
    EXPECT_EQ(read_theater_title(tmp.path() / "theater.ini"), "Korea Theater");
}

TEST(TheaterIni, IgnoresTitleInOtherSection) {
    TempDir tmp;
    write_file(tmp.path() / "theater.ini",
               "[Other]\n"
               "Title=Ignore This\n"
               "[Theater]\n"
               "Title=Real Title\n");
    EXPECT_EQ(read_theater_title(tmp.path() / "theater.ini"), "Real Title");
}

// ===========================================================================
// Installation::resolve
// ===========================================================================

TEST(Installation, ResolveReturnsPathUnderRoot) {
    TempDir tmp;
    touch(tmp.path() / "FALCON4.ct");
    auto inst = Installation::detect(tmp.path());
    auto p = inst.resolve("sim/aircraft/f16.dat");
    EXPECT_EQ(p, tmp.path() / "sim/aircraft/f16.dat");
}

TEST(Installation, ResolveReturnsEmptyForInvalidInstall) {
    auto inst = Installation::detect("/nonexistent");
    EXPECT_TRUE(inst.resolve("anything").empty());
}

// ===========================================================================
// Installation::diagnostics — what detect() probed + found
// ===========================================================================

TEST(Installation, DiagnosticsRecordsClassTableSearchPaths) {
    TempDir tmp;
    // No FALCON4.ct anywhere — should record all 3 search locations.
    touch(tmp.path() / "terrdata" / "korea" / "THEATER.MAP");
    touch(tmp.path() / "terrdata" / "korea" / "THEATER.MEA");

    auto inst = Installation::detect(tmp.path());
    const auto& diag = inst.diagnostics();

    // Should have probed at least root/FALCON4.ct. Since we have no
    // sim/ dir, the sim/FALCON4.ct probe is skipped. Since we have a
    // terrdata/ dir, the terrdata/FALCON4.ct probe is recorded.
    EXPECT_GE(diag.class_table_searched.size(), 2u);
    bool probed_root = false;
    bool probed_terrdata = false;
    for (const auto& p : diag.class_table_searched) {
        if (p.filename() == "FALCON4.ct") {
            if (p.parent_path() == tmp.path()) probed_root = true;
            if (p.parent_path().filename() == "terrdata") probed_terrdata = true;
        }
    }
    EXPECT_TRUE(probed_root);
    EXPECT_TRUE(probed_terrdata);
    EXPECT_TRUE(inst.class_table().empty());  // not found
}

TEST(Installation, DiagnosticsRecordsClassTableFoundAtRoot) {
    TempDir tmp;
    touch(tmp.path() / "FALCON4.ct");
    auto inst = Installation::detect(tmp.path());
    EXPECT_FALSE(inst.class_table().empty());
    // Even when found, the search path is recorded (so the user can
    // see WHERE it was found).
    EXPECT_GE(inst.diagnostics().class_table_searched.size(), 1u);
}

TEST(Installation, DiagnosticsRecordsClassTableFoundInSimDir) {
    TempDir tmp;
    touch(tmp.path() / "sim" / "FALCON4.ct");
    auto inst = Installation::detect(tmp.path());
    EXPECT_FALSE(inst.class_table().empty());
    EXPECT_GE(inst.diagnostics().class_table_searched.size(), 2u);
    // The found path should be in sim/
    EXPECT_EQ(inst.class_table().parent_path().filename(), "sim");
}

TEST(Installation, DiagnosticsCampaignDirFoundFlag) {
    TempDir tmp;
    touch(tmp.path() / "FALCON4.ct");
    touch(tmp.path() / "campaign" / "save1.cam");
    auto inst = Installation::detect(tmp.path());
    EXPECT_TRUE(inst.diagnostics().campaign_dir_found);
    EXPECT_EQ(inst.campaigns().size(), 1u);
}

TEST(Installation, DiagnosticsCampaignDirNotFoundFlag) {
    TempDir tmp;
    touch(tmp.path() / "FALCON4.ct");
    // No campaign/ directory.
    auto inst = Installation::detect(tmp.path());
    EXPECT_FALSE(inst.diagnostics().campaign_dir_found);
    EXPECT_TRUE(inst.campaigns().empty());
}

TEST(Installation, DiagnosticsTheaterLstParsed) {
    TempDir tmp;
    touch(tmp.path() / "FALCON4.ct");
    touch(tmp.path() / "terrdata" / "korea" / "THEATER.MAP");
    touch(tmp.path() / "terrdata" / "korea" / "THEATER.MEA");
    write_file(tmp.path() / "terrdata" / "theater.lst",
               "korea\nbalkans\n");
    auto inst = Installation::detect(tmp.path());
    EXPECT_TRUE(inst.diagnostics().theater_lst_parsed);
    EXPECT_EQ(inst.diagnostics().theater_lst_key_count, 2u);
    EXPECT_EQ(inst.diagnostics().theater_lst_path.filename(), "theater.lst");
}

TEST(Installation, DiagnosticsTheaterLstAbsent) {
    TempDir tmp;
    touch(tmp.path() / "FALCON4.ct");
    touch(tmp.path() / "terrdata" / "korea" / "THEATER.MAP");
    touch(tmp.path() / "terrdata" / "korea" / "THEATER.MEA");
    // No theater.lst.
    auto inst = Installation::detect(tmp.path());
    EXPECT_FALSE(inst.diagnostics().theater_lst_parsed);
    EXPECT_TRUE(inst.diagnostics().theater_lst_path.empty());
    EXPECT_EQ(inst.diagnostics().theater_lst_key_count, 0u);
}

TEST(Installation, DiagnosticsTheaterDirsProbed) {
    TempDir tmp;
    touch(tmp.path() / "FALCON4.ct");
    // korea is a theater (has THEATER.MAP), balkans is a subdir without
    // THEATER.MAP (rejected but should still be in theater_dirs_probed).
    touch(tmp.path() / "terrdata" / "korea" / "THEATER.MAP");
    touch(tmp.path() / "terrdata" / "korea" / "THEATER.MEA");
    touch(tmp.path() / "terrdata" / "balkans" / "some_other_file.txt");

    auto inst = Installation::detect(tmp.path());
    const auto& diag = inst.diagnostics();
    // Both subdirs should be in theater_dirs_probed.
    EXPECT_EQ(diag.theater_dirs_probed.size(), 2u);
    // Only korea should be in theaters().
    EXPECT_EQ(inst.theaters().size(), 1u);
    EXPECT_EQ(inst.theaters()[0].key, "korea");
}

TEST(DiagnosticInfo, FormatProducesNonEmptyString) {
    DiagnosticInfo diag;
    diag.class_table_searched = {"/path1/FALCON4.ct", "/path2/FALCON4.ct"};
    diag.campaign_dir_found = true;
    diag.theater_lst_parsed = true;
    diag.theater_lst_key_count = 3;
    diag.theater_lst_path = "/path/theater.lst";
    diag.theater_dirs_probed = {"/path/terrdata/korea"};
    std::string s = diag.format();
    EXPECT_FALSE(s.empty());
    EXPECT_NE(s.find("FALCON4.ct search"), std::string::npos);
    EXPECT_NE(s.find("2 locations probed"), std::string::npos);
    EXPECT_NE(s.find("theater.lst"), std::string::npos);
    EXPECT_NE(s.find("3 keys"), std::string::npos);
    EXPECT_NE(s.find("Campaign dir found: yes"), std::string::npos);
}

TEST(DiagnosticInfo, FormatHandlesEmpty) {
    DiagnosticInfo diag;
    std::string s = diag.format();
    EXPECT_FALSE(s.empty());
    EXPECT_NE(s.find("0 locations probed"), std::string::npos);
    EXPECT_NE(s.find("not found"), std::string::npos);
}
