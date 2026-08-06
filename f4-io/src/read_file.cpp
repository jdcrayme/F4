// f4-io/src/read_file.cpp
//
// Implementation of f4::io::read_file. See read_file.hpp for the API
// contract and consolidation history.

#include <f4/io/read_file.hpp>

#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

namespace f4::io {

namespace {

// RAII wrapper for std::FILE*. Closes the handle on scope exit, including
// when the buffer allocation between fopen and fclose throws std::bad_alloc
// (which the previous raw-FILE* implementation leaked).
//
// We deliberately use a C FILE* (instead of std::ifstream) so we can keep
// the exact same error-reporting vocabulary ("cannot open", "ftell failed",
// "short read") that the existing callers — including the unit tests in
// f4-io/tests/test_read_file.cpp — already assert on.
struct FileGuard {
    std::FILE* fp;
    explicit FileGuard(std::FILE* f) noexcept : fp(f) {}
    ~FileGuard() { if (fp) std::fclose(fp); }
    FileGuard(const FileGuard&) = delete;
    FileGuard& operator=(const FileGuard&) = delete;
    FileGuard(FileGuard&& o) noexcept : fp(o.fp) { o.fp = nullptr; }
    FileGuard& operator=(FileGuard&& o) noexcept {
        if (this != &o) { if (fp) std::fclose(fp); fp = o.fp; o.fp = nullptr; }
        return *this;
    }
    explicit operator bool() const noexcept { return fp != nullptr; }
};

} // namespace

std::vector<uint8_t> read_file(const std::filesystem::path& path,
                                const char* label) {
    const std::string prefix = std::string(label) + ": ";

    FileGuard fg(std::fopen(path.string().c_str(), "rb"));
    if (!fg) {
        throw std::runtime_error(prefix + "cannot open " + path.string());
    }
    std::fseek(fg.fp, 0, SEEK_END);
    const long sz = std::ftell(fg.fp);
    std::fseek(fg.fp, 0, SEEK_SET);
    if (sz < 0) {
        throw std::runtime_error(prefix + "ftell failed");
    }

    // If this allocation throws, ~FileGuard() closes fp — no leak.
    std::vector<uint8_t> buf(static_cast<std::size_t>(sz));
    const std::size_t got = std::fread(buf.data(), 1, buf.size(), fg.fp);
    // fp closed by ~FileGuard when fg goes out of scope.
    if (got != buf.size()) {
        throw std::runtime_error(prefix + "short read on " + path.string());
    }
    return buf;
}

} // namespace f4::io
