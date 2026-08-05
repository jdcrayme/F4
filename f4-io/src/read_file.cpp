// f4-io/src/read_file.cpp
//
// Implementation of f4::io::read_file. See read_file.hpp for the API
// contract and consolidation history.

#include <f4/io/read_file.hpp>

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace f4::io {

std::vector<uint8_t> read_file(const std::filesystem::path& path,
                                const char* label) {
    const std::string prefix = std::string(label) + ": ";

    FILE* fp = std::fopen(path.string().c_str(), "rb");
    if (!fp) {
        throw std::runtime_error(prefix + "cannot open " + path.string());
    }
    std::fseek(fp, 0, SEEK_END);
    const long sz = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    if (sz < 0) {
        std::fclose(fp);
        throw std::runtime_error(prefix + "ftell failed");
    }
    std::vector<uint8_t> buf(static_cast<std::size_t>(sz));
    const std::size_t got = std::fread(buf.data(), 1, buf.size(), fp);
    std::fclose(fp);
    if (got != buf.size()) {
        throw std::runtime_error(prefix + "short read on " + path.string());
    }
    return buf;
}

} // namespace f4::io
