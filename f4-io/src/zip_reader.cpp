// f4-io/src/zip_reader.cpp
//
// ZipReader implementation. See zip_reader.hpp for the design.

#include <f4/io/zip_reader.hpp>

#include <f4/io/read_file.hpp>

#include <cctype>
#include <cstring>
#include <stdexcept>

namespace f4::io {

namespace {

constexpr uint32_t SIG_LOCAL      = 0x04034b50;  // "PK\x03\x04"
constexpr uint32_t SIG_CENTRAL    = 0x02014b50;  // "PK\x01\x02"
constexpr uint32_t SIG_EOCD       = 0x06054b50;  // "PK\x05\x06"
constexpr uint16_t METHOD_STORED  = 0;
constexpr uint64_t MAX_EOCD_SCAN  = 65536 + 22;  // comment field limit + record

std::string to_lower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

void ZipReader::load(const std::filesystem::path& zip_path) {
    entries_.clear();
    data_.clear();
    data_ = read_file(zip_path, "zip_reader");

    // --- Locate the End Of Central Directory record -------------------
    const std::size_t n = data_.size();
    if (n < 22) throw std::runtime_error("zip_reader: file too small");
    const std::size_t scan_start =
        n > MAX_EOCD_SCAN ? n - MAX_EOCD_SCAN : 0;
    std::size_t eocd = n;  // n == "not found"
    for (std::size_t i = n - 22; ; --i) {
        uint32_t sig;
        std::memcpy(&sig, data_.data() + i, 4);
        if (sig == SIG_EOCD) { eocd = i; break; }
        if (i == scan_start) break;
    }
    if (eocd >= n) throw std::runtime_error("zip_reader: no EOCD record");

    uint16_t count, cd_size;
    uint32_t cd_offset;
    std::memcpy(&count, data_.data() + eocd + 10, 2);
    std::memcpy(&cd_size, data_.data() + eocd + 12, 2);
    std::memcpy(&cd_offset, data_.data() + eocd + 16, 4);
    if (static_cast<std::size_t>(cd_offset) + cd_size > n)
        throw std::runtime_error("zip_reader: central directory out of range");

    // --- Walk the central directory ------------------------------------
    std::size_t p = cd_offset;
    for (uint16_t e = 0; e < count; ++e) {
        if (p + 46 > n) throw std::runtime_error("zip_reader: truncated central dir");
        uint32_t sig;
        std::memcpy(&sig, data_.data() + p, 4);
        if (sig != SIG_CENTRAL)
            throw std::runtime_error("zip_reader: bad central dir signature");

        uint16_t method, nlen, elen, clen;
        uint32_t csize, usize, lho;
        std::memcpy(&method, data_.data() + p + 10, 2);
        std::memcpy(&csize, data_.data() + p + 20, 4);
        std::memcpy(&usize, data_.data() + p + 24, 4);
        std::memcpy(&nlen,  data_.data() + p + 28, 2);
        std::memcpy(&elen,  data_.data() + p + 30, 2);
        std::memcpy(&clen,  data_.data() + p + 32, 2);
        std::memcpy(&lho,   data_.data() + p + 42, 4);

        const std::string name(data_.data() + p + 46,
                               data_.data() + p + 46 + nlen);

        // Resolve the entry's data offset through its local header.
        const std::size_t lh = static_cast<std::size_t>(lho);
        if (lh + 30 > n) throw std::runtime_error("zip_reader: local header OOB");
        uint32_t lsig;
        std::memcpy(&lsig, data_.data() + lh, 4);
        if (lsig != SIG_LOCAL)
            throw std::runtime_error("zip_reader: bad local header signature");
        uint16_t lnlen, lelen;
        std::memcpy(&lnlen, data_.data() + lh + 26, 2);
        std::memcpy(&lelen, data_.data() + lh + 28, 2);

        Entry entry;
        entry.size = usize > 0 ? usize : csize;   // stored: both equal
        entry.data_offset = lh + 30 + lnlen + lelen;
        if (method != METHOD_STORED) {
            // Index the name so has() is truthful, but mark it so read()
            // reports a clear error instead of slicing garbage.
            entry.data_offset = UINT64_MAX;
            entry.size = 0;
        }
        entries_[to_lower(name)] = entry;

        p += 46 + static_cast<std::size_t>(nlen) + elen + clen;
    }
}

bool ZipReader::has(const std::string& name) const {
    return entries_.find(to_lower(name)) != entries_.end();
}

std::vector<uint8_t> ZipReader::read(const std::string& name) const {
    const auto it = entries_.find(to_lower(name));
    if (it == entries_.end())
        throw std::runtime_error("zip_reader: no entry named " + name);
    const Entry& e = it->second;
    if (e.data_offset == UINT64_MAX)
        throw std::runtime_error("zip_reader: entry " + name +
                                 " is compressed (only STORED supported)");
    if (e.data_offset + e.size > data_.size())
        throw std::runtime_error("zip_reader: entry data out of range: " + name);
    return std::vector<uint8_t>(data_.begin() + static_cast<std::ptrdiff_t>(e.data_offset),
                                data_.begin() + static_cast<std::ptrdiff_t>(e.data_offset + e.size));
}

} // namespace f4::io
