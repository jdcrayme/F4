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

// Windows system()/_pclose return the raw exit code; POSIX shells wrap
// it in a wait status that WEXITSTATUS unwraps.
int exit_code_of(int rc) {
#ifdef _WIN32
    return rc;
#else
    return WEXITSTATUS(rc);
#endif
}

struct CmdResult {
    int exit_code = 0;
    std::string out;
    std::string err;
};

CmdResult run_cmd(const std::vector<std::string>& argv) {
    // Double quotes are the one quoting style both POSIX sh and Windows
    // cmd.exe honor, and captures go to the OS temp dir (/tmp is not a
    // Windows path). The program itself must be native-styled — cmd.exe
    // cannot exec a quoted forward-slash path ("filename syntax is
    // incorrect").
    const auto out_log = fs::temp_directory_path() / "f4i_out.log";
    const auto err_log = fs::temp_directory_path() / "f4i_err.log";
    std::string cmd;
    cmd += '"';
    cmd += fs::path(argv[0]).make_preferred().string();
    cmd += "\" ";
    for (size_t i = 1; i < argv.size(); ++i) {
        cmd += '"';
        cmd += argv[i];
        cmd += "\" ";
    }
    cmd += "> \"" + out_log.string() + "\" 2> \"" + err_log.string() + "\"";
#ifdef _WIN32
    // cmd /c strips the FIRST and LAST quote of the line whenever the
    // line holds more than two quotes — which mangles the quoted
    // program path. Wrapping the whole command in one extra pair makes
    // the outer strip land on the wrapper (the standard workaround).
    cmd = "\"" + cmd + "\"";
#endif
    const int rc = std::system(cmd.c_str());
    CmdResult r;
    r.exit_code = exit_code_of(rc);
    {
        std::FILE* f = std::fopen(out_log.string().c_str(), "rb");
        if (f) { char buf[4096]; size_t n; while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) r.out.append(buf, n); std::fclose(f); }
    }
    {
        std::FILE* f = std::fopen(err_log.string().c_str(), "rb");
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
