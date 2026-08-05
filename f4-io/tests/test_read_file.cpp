// test_read_file.cpp — unit tests for f4::io::read_file.
//
// Coverage:
//   * read_file throws std::runtime_error on a missing file.
//   * read_file returns the correct bytes for an existing small file.
//   * read_file handles an empty file (returns an empty vector).
//   * read_file's `label` argument appears in the error message.
//   * read_file preserves binary content (no text-mode translation).

#include <gtest/gtest.h>

#include <f4/io/read_file.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// RAII helper: create a temp file with the given contents and return its
// path. The file is removed when the helper is destroyed.
class TempFile {
public:
    explicit TempFile(const std::vector<uint8_t>& contents) {
        // Use the process CWD + a unique-ish name. CWD under ctest is the
        // build directory, which is writable.
        path_ = std::filesystem::temp_directory_path() /
                ("f4_io_test_" + std::to_string(counter_++) + ".bin");
        std::ofstream out(path_, std::ios::binary);
        out.write(reinterpret_cast<const char*>(contents.data()),
                  static_cast<std::streamsize>(contents.size()));
        out.close();
    }
    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    const std::filesystem::path& path() const { return path_; }

private:
    static int counter_;
    std::filesystem::path path_;
};

int TempFile::counter_ = 0;

} // namespace

TEST(ReadFile, ThrowsOnMissingFile) {
    const auto missing = std::filesystem::temp_directory_path() /
                         "f4_io_definitely_does_not_exist_xyz.bin";
    std::error_code ec;
    std::filesystem::remove(missing, ec);
    ASSERT_FALSE(std::filesystem::exists(missing));
    EXPECT_THROW(f4::io::read_file(missing), std::runtime_error);
}

TEST(ReadFile, ReturnsCorrectBytesForSmallFile) {
    const std::vector<uint8_t> expected = {0x01, 0x02, 0x03, 0xFF, 0x00, 0xAB};
    TempFile tmp(expected);
    const auto got = f4::io::read_file(tmp.path());
    EXPECT_EQ(got, expected);
}

TEST(ReadFile, EmptyFileReturnsEmptyVector) {
    const std::vector<uint8_t> empty;
    TempFile tmp(empty);
    const auto got = f4::io::read_file(tmp.path());
    EXPECT_TRUE(got.empty());
}

TEST(ReadFile, DefaultLabelAppearsInErrorMessage) {
    const auto missing = std::filesystem::temp_directory_path() /
                         "f4_io_no_such_file_label_test.bin";
    std::error_code ec;
    std::filesystem::remove(missing, ec);
    try {
        f4::io::read_file(missing);
        FAIL() << "expected std::runtime_error";
    } catch (const std::runtime_error& e) {
        const std::string msg = e.what();
        EXPECT_NE(msg.find("read_file:"), std::string::npos)
            << "default label should appear in error message: " << msg;
    }
}

TEST(ReadFile, CustomLabelAppearsInErrorMessage) {
    const auto missing = std::filesystem::temp_directory_path() /
                         "f4_io_no_such_file_custom_label.bin";
    std::error_code ec;
    std::filesystem::remove(missing, ec);
    try {
        f4::io::read_file(missing, "theater_data");
        FAIL() << "expected std::runtime_error";
    } catch (const std::runtime_error& e) {
        const std::string msg = e.what();
        EXPECT_NE(msg.find("theater_data:"), std::string::npos)
            << "custom label should appear in error message: " << msg;
    }
}

TEST(ReadFile, PreservesBinaryContent) {
    // All 256 byte values — catches any accidental text-mode translation
    // (e.g. \r\n <-> \n) that a future refactor might introduce.
    std::vector<uint8_t> expected(256);
    for (int i = 0; i < 256; ++i) expected[i] = static_cast<uint8_t>(i);
    TempFile tmp(expected);
    const auto got = f4::io::read_file(tmp.path());
    EXPECT_EQ(got.size(), expected.size());
    EXPECT_EQ(got, expected);
}

TEST(ReadFile, LargeFileRoundTrips) {
    // A few KB — bigger than a single read() call might return on some
    // systems, exercising the short-read check.
    std::vector<uint8_t> expected(8192);
    for (std::size_t i = 0; i < expected.size(); ++i) {
        expected[i] = static_cast<uint8_t>(i * 7 + 1);
    }
    TempFile tmp(expected);
    const auto got = f4::io::read_file(tmp.path());
    EXPECT_EQ(got, expected);
}
