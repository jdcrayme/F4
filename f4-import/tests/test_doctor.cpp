// f4-import/tests/test_doctor.cpp

#include <f4/import/doctor.hpp>
#include <f4/assets/manifest.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

using namespace f4::import;
using namespace f4::assets;
namespace fs = std::filesystem;

namespace {

fs::path copy_clean_fixture(const std::string& suffix) {
    auto src = fs::path(FIXTURE_DIR) / "clean_data";
    auto dst = fs::temp_directory_path() / "f4_import_doctor_test" / suffix;
    fs::remove_all(dst);
    fs::create_directories(dst);
    std::error_code ec;
    fs::copy(src, dst, fs::copy_options::recursive, ec);
    if (ec) {
        ADD_FAILURE() << "fixture copy failed: " << ec.message();
    }
    return dst;
}

void write_file(const fs::path& p, const std::string& contents) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p);
    f << contents;
}

Manifest load_manifest(const fs::path& data_dir) {
    return read_manifest_file((data_dir / "manifest.json").string());
}

std::size_t count_findings(const DoctorReport& r, const std::string& check_id) {
    std::size_t n = 0;
    for (const auto& f : r.findings) {
        if (f.check_id == check_id) ++n;
    }
    return n;
}

} // namespace

TEST(Doctor, CleanFixtureHasNoErrors) {
    auto data_dir = copy_clean_fixture("clean");
    auto report = run_doctor(data_dir);
    EXPECT_EQ(report.errors, 0u)
        << "clean fixture should produce zero errors:\n"
        << format_report(report);
    fs::remove_all(data_dir);
}

TEST(Doctor, CleanFixturePassesD9) {
    auto data_dir = copy_clean_fixture("clean_d9");
    auto m = load_manifest(data_dir);
    DoctorReport r;
    check_d9_manifest_consistency(data_dir, m, r);
    EXPECT_EQ(r.errors, 0u) << format_report(r);
    fs::remove_all(data_dir);
}

TEST(Doctor, D2DetectsMissingTerrainRef) {
    auto data_dir = copy_clean_fixture("d2_missing");
    write_file(data_dir / "World/save1.world.json",
        R"({"theater":"korea","terrain_file":"@asset:theater:nope","campaign":{"teams":[]}})");
    auto m = load_manifest(data_dir);
    DoctorReport r;
    check_d2_world_json_refs(data_dir, m, r);
    EXPECT_EQ(count_findings(r, "D2"), 1u);
    EXPECT_EQ(r.errors, 1u);
    fs::remove_all(data_dir);
}

TEST(Doctor, D2AcceptsLegacyBareFilename) {
    auto data_dir = copy_clean_fixture("d2_legacy");
    write_file(data_dir / "World/save1.world.json",
        R"({"theater":"korea","terrain_file":"korea.terrain.json","campaign":{"teams":[]}})");
    auto m = load_manifest(data_dir);
    DoctorReport r;
    check_d2_world_json_refs(data_dir, m, r);
    EXPECT_EQ(count_findings(r, "D2"), 0u);
    fs::remove_all(data_dir);
}

TEST(Doctor, D2AcceptsValidAssetRef) {
    auto data_dir = copy_clean_fixture("d2_valid");
    auto m = load_manifest(data_dir);
    DoctorReport r;
    check_d2_world_json_refs(data_dir, m, r);
    EXPECT_EQ(count_findings(r, "D2"), 0u)
        << "valid asset ref should not produce a D2 finding";
    fs::remove_all(data_dir);
}

TEST(Doctor, D4FlagsPresentCapabilityWithNoSources) {
    auto data_dir = copy_clean_fixture("d4_no_sources");
    auto m = load_manifest(data_dir);
    AssetEntry* e = m.find(AssetId{AssetFamily::koreaobj, "00002"});
    ASSERT_NE(e, nullptr);
    e->sources.clear();
    DoctorReport r;
    check_d4_capability_sources(data_dir, m, r);
    EXPECT_EQ(count_findings(r, "D4"), 1u);
    EXPECT_EQ(r.errors, 1u);
    fs::remove_all(data_dir);
}

TEST(Doctor, D4AcceptsUnknownWithoutSources) {
    auto data_dir = copy_clean_fixture("d4_unknown");
    auto m = load_manifest(data_dir);
    AssetEntry* e = m.find(AssetId{AssetFamily::koreaobj, "00002"});
    ASSERT_NE(e, nullptr);
    e->sources.clear();
    Capability* dofs = e->find_capability("dofs");
    ASSERT_NE(dofs, nullptr);
    dofs->status = CapabilityStatus::unknown;
    dofs->count.reset();
    DoctorReport r;
    check_d4_capability_sources(data_dir, m, r);
    EXPECT_EQ(count_findings(r, "D4"), 0u);
    fs::remove_all(data_dir);
}

TEST(Doctor, D8DetectsMissingFileOnDisk) {
    auto data_dir = copy_clean_fixture("d8_missing_file");
    fs::remove(data_dir / "Models/koreaobj/00002.gltf");
    auto m = load_manifest(data_dir);
    DoctorReport r;
    check_d8_id_and_files(data_dir, m, r);
    EXPECT_GE(count_findings(r, "D8"), 1u);
    fs::remove_all(data_dir);
}

TEST(Doctor, D8DetectsUnlistedFileOnDisk) {
    auto data_dir = copy_clean_fixture("d8_unlisted");
    write_file(data_dir / "Models/koreaobj/00999.gltf", "{}");
    auto m = load_manifest(data_dir);
    DoctorReport r;
    check_d8_id_and_files(data_dir, m, r);
    bool found_unlisted = false;
    for (const auto& f : r.findings) {
        if (f.check_id == "D8" && f.message.find("unlisted") != std::string::npos) {
            found_unlisted = true;
            break;
        }
    }
    EXPECT_TRUE(found_unlisted) << "expected an unlisted-file finding";
    fs::remove_all(data_dir);
}

TEST(Doctor, D8IgnoresNonAssetFiles) {
    auto data_dir = copy_clean_fixture("d8_readme");
    write_file(data_dir / "Models/README.md", "this is not an asset");
    auto m = load_manifest(data_dir);
    DoctorReport r;
    check_d8_id_and_files(data_dir, m, r);
    for (const auto& f : r.findings) {
        if (f.check_id == "D8" && f.message.find("README") != std::string::npos) {
            FAIL() << "non-asset file should not be flagged: " << f.message;
        }
    }
    fs::remove_all(data_dir);
}

TEST(Doctor, D9DetectsMissingManifest) {
    auto data_dir = fs::temp_directory_path() / "f4_import_doctor_test" / "d9_no_manifest";
    fs::remove_all(data_dir);
    fs::create_directories(data_dir);
    auto report = run_doctor(data_dir);
    EXPECT_EQ(count_findings(report, "D9"), 1u);
    EXPECT_EQ(report.errors, 1u);
    fs::remove_all(data_dir);
}

TEST(Doctor, D9DetectsCorruptManifest) {
    auto data_dir = fs::temp_directory_path() / "f4_import_doctor_test" / "d9_corrupt";
    fs::remove_all(data_dir);
    fs::create_directories(data_dir);
    write_file(data_dir / "manifest.json", R"({ not valid json })");
    auto report = run_doctor(data_dir);
    EXPECT_EQ(count_findings(report, "D9"), 1u);
    EXPECT_EQ(report.errors, 1u);
    fs::remove_all(data_dir);
}

TEST(Doctor, FormatReportIncludesFindingsAndSummary) {
    DoctorReport r;
    r.findings.push_back({"D2", Severity::error, "theater:nope",
                          "world references unknown theater"});
    r.errors = 1;
    std::string s = format_report(r);
    EXPECT_NE(s.find("ERROR D2"), std::string::npos);
    EXPECT_NE(s.find("theater:nope"), std::string::npos);
    EXPECT_NE(s.find("1 errors"), std::string::npos);
}

TEST(Doctor, RunDoctorReturnsSameAsAggregatedChecks) {
    auto data_dir = copy_clean_fixture("aggregate");
    auto m = load_manifest(data_dir);
    auto full = run_doctor(data_dir, m);
    DoctorReport sum;
    check_d1_visual_bindings(data_dir, m, sum);
    check_d2_world_json_refs(data_dir, m, sum);
    check_d3_class_table_bindings(data_dir, m, sum);
    check_d4_capability_sources(data_dir, m, sum);
    check_d5_node_tags(data_dir, m, sum);
    check_d6_vocab(data_dir, m, sum);
    check_d7_orphans(data_dir, m, sum);
    check_d8_id_and_files(data_dir, m, sum);
    check_d9_manifest_consistency(data_dir, m, sum);
    EXPECT_EQ(sum.findings.size(), full.findings.size());
    EXPECT_EQ(sum.errors, full.errors);
    EXPECT_EQ(sum.warnings, full.warnings);
    EXPECT_EQ(sum.infos, full.infos);
    fs::remove_all(data_dir);
}
