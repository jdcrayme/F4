// f4-import/tests/test_f4import_cli.cpp
//
// Smoke tests for the `f4import` binary — spawn the CLI as a subprocess
// against the bundled clean fixture and assert exit codes.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace fs = std::filesystem;

namespace {

std::string f4import_path() { return F4IMPORT_PATH; }

struct CmdResult {
    int exit_code = 0;
    std::string out;
    std::string err;
};

CmdResult run_cmd(const std::vector<std::string>& argv) {
    std::string cmd;
    for (const auto& a : argv) {
        cmd += "'";
        cmd += a;
        cmd += "' ";
    }
    cmd += "> /tmp/f4i_out.log 2> /tmp/f4i_err.log";
    int rc = std::system(cmd.c_str());
    CmdResult r;
    r.exit_code = WEXITSTATUS(rc);
    {
        std::FILE* f = std::fopen("/tmp/f4i_out.log", "rb");
        if (f) { char buf[4096]; size_t n; while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) r.out.append(buf, n); std::fclose(f); }
    }
    {
        std::FILE* f = std::fopen("/tmp/f4i_err.log", "rb");
        if (f) { char buf[4096]; size_t n; while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) r.err.append(buf, n); std::fclose(f); }
    }
    return r;
}

} // namespace

TEST(F4ImportCLI, HelpExitsZero) {
    auto r = run_cmd({f4import_path(), "--help"});
    EXPECT_EQ(r.exit_code, 0);
    std::string combined = r.out + r.err;
    EXPECT_NE(combined.find("f4import"), std::string::npos);
}

TEST(F4ImportCLI, NoArgsExitsUsageError) {
    auto r = run_cmd({f4import_path()});
    EXPECT_EQ(r.exit_code, 3);
}

TEST(F4ImportCLI, UnknownSubcommandExitsUsageError) {
    auto r = run_cmd({f4import_path(), "frob"});
    EXPECT_EQ(r.exit_code, 3);
}

TEST(F4ImportCLI, CheckOnCleanFixtureExitsZero) {
    std::string data = std::string(FIXTURE_DIR) + "clean_data";
    auto r = run_cmd({f4import_path(), "check", "--data", data});
    EXPECT_EQ(r.exit_code, 0) << r.err;
    EXPECT_NE(r.out.find("OK"), std::string::npos);
}

TEST(F4ImportCLI, CheckOnMissingDirExitsOne) {
    auto r = run_cmd({f4import_path(), "check", "--data", "/no/such/dir"});
    EXPECT_EQ(r.exit_code, 1);
}

TEST(F4ImportCLI, CheckOnEmptyDirExitsOne) {
    auto empty = fs::temp_directory_path() / "f4i_empty_dir";
    fs::remove_all(empty);
    fs::create_directories(empty);
    auto r = run_cmd({f4import_path(), "check", "--data", empty.string()});
    EXPECT_EQ(r.exit_code, 1) << "empty Data/ is missing (manifest has no assets)";
    fs::remove_all(empty);
}

TEST(F4ImportCLI, DoctorOnCleanFixtureExitsZeroOrWarningOnly) {
    std::string data = std::string(FIXTURE_DIR) + "clean_data";
    auto r = run_cmd({f4import_path(), "doctor", "--data", data});
    EXPECT_TRUE(r.exit_code == 0 || r.exit_code == 2)
        << "exit_code=" << r.exit_code << " out=" << r.out << " err=" << r.err;
}

TEST(F4ImportCLI, DoctorOnMissingManifestExitsOne) {
    auto empty = fs::temp_directory_path() / "f4i_no_manifest";
    fs::remove_all(empty);
    fs::create_directories(empty);
    auto r = run_cmd({f4import_path(), "doctor", "--data", empty.string()});
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_NE(r.out.find("D9"), std::string::npos);
    fs::remove_all(empty);
}

TEST(F4ImportCLI, DoctorOnMissingDataDirExitsOne) {
    auto r = run_cmd({f4import_path(), "doctor", "--data", "/no/such/dir"});
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_NE(r.out.find("D9"), std::string::npos);
}

TEST(F4ImportCLI, CheckRequiresDataDir) {
    auto r = run_cmd({f4import_path(), "check"});
    EXPECT_EQ(r.exit_code, 3);
}

TEST(F4ImportCLI, DoctorRequiresDataDir) {
    auto r = run_cmd({f4import_path(), "doctor"});
    EXPECT_EQ(r.exit_code, 3);
}
