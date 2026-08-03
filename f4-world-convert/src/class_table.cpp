// f4-world-convert/src/class_table.cpp

#include <f4/convert/class_table.hpp>

#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace f4::convert {

namespace {

// On-disk entry size = 81 bytes (VuEntityType with natural alignment = 60,
// + visType[7] = 14 + vehicleDataIndex = 2 + dataType = 1 + dataPtr = 4).
constexpr std::size_t ENTRY_SIZE = 81;
constexpr std::size_t CLASSINFO_OFFSET = 8;  // offset of classInfo_[8] within entry

std::vector<uint8_t> read_file(const std::filesystem::path& path) {
    FILE* fp = std::fopen(path.string().c_str(), "rb");
    if (!fp) throw std::runtime_error("class_table: cannot open " + path.string());
    std::fseek(fp, 0, SEEK_END);
    const long sz = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    std::vector<uint8_t> buf(static_cast<std::size_t>(sz));
    const std::size_t got = std::fread(buf.data(), 1, buf.size(), fp);
    std::fclose(fp);
    if (got != buf.size()) throw std::runtime_error("class_table: short read");
    return buf;
}

} // namespace

void ClassTable::load(const std::filesystem::path& ct_path) {
    auto data = read_file(ct_path);
    if (data.size() < 2) throw std::runtime_error("class_table: file too small");

    // First 2 bytes: short NumEntities
    int16_t num_entities;
    std::memcpy(&num_entities, data.data(), 2);
    if (num_entities <= 0) throw std::runtime_error("class_table: invalid NumEntities");

    // FF-DB Control format: if NumEntities == 0, real count is at end of file.
    // We don't support this format (it's rare); throw with a clear message.
    if (num_entities == 0) {
        throw std::runtime_error("class_table: FF-DB Control format not supported");
    }

    const std::size_t expected = 2 + static_cast<std::size_t>(num_entities) * ENTRY_SIZE;
    if (data.size() < expected) {
        throw std::runtime_error("class_table: file too small for " +
                                 std::to_string(num_entities) + " entries");
    }

    entries_.clear();
    entries_.reserve(static_cast<std::size_t>(num_entities));
    for (int i = 0; i < num_entities; ++i) {
        const std::size_t offset = 2 + static_cast<std::size_t>(i) * ENTRY_SIZE;
        const uint8_t* classinfo = data.data() + offset + CLASSINFO_OFFSET;
        ClassTableEntry e;
        e.domain = classinfo[0];
        e.cls    = classinfo[1];
        e.type   = classinfo[2];
        e.stype  = classinfo[3];
        entries_.push_back(e);
    }
}

const ClassTableEntry* ClassTable::lookup(uint16_t entity_type) const noexcept {
    const int idx = static_cast<int>(entity_type) - VU_LAST_ENTITY_TYPE;
    if (idx < 0 || static_cast<std::size_t>(idx) >= entries_.size()) return nullptr;
    return &entries_[static_cast<std::size_t>(idx)];
}

uint8_t ClassTable::objective_type_for(uint16_t entity_type) const noexcept {
    const auto* e = lookup(entity_type);
    if (!e || e->cls != CLASS_OBJECTIVE) return 0;
    return e->type;
}

uint8_t ClassTable::unit_subtype_for(uint16_t entity_type) const noexcept {
    const auto* e = lookup(entity_type);
    if (!e || e->cls != CLASS_UNIT) return 0;
    return e->stype;
}

std::filesystem::path find_class_table(const std::filesystem::path& reference_file) {
    const std::string filename = "FALCON4.ct";
    auto candidate_in = [&filename](const std::filesystem::path& dir) -> std::filesystem::path {
        if (dir.empty()) return {};
        auto p = dir / filename;
        return std::filesystem::exists(p) ? p : std::filesystem::path{};
    };

    // 1. Next to the reference file (typically the .cam).
    if (!reference_file.empty()) {
        auto p = candidate_in(reference_file.parent_path());
        if (!p.empty()) return p;
        // 2. Up a directory or two.
        auto parent = reference_file.parent_path().parent_path();
        if (!parent.empty()) {
            p = candidate_in(parent);
            if (!p.empty()) return p;
        }
    }

    // 3. CWD-relative — covers the common case where the viewer or CLI is
    // run from the build directory (CMake copies fixtures there) or from
    // the project root (the source-tree fixtures are at known paths).
    const char* cwd_relative[] = {
        "FALCON4.ct",
        "assets/FALCON4.ct",
        "temp/FALCON4.ct",
        "f4-world-convert/tests/fixtures/FALCON4.ct",
        "../f4-world-convert/tests/fixtures/FALCON4.ct",
        "../../f4-world-convert/tests/fixtures/FALCON4.ct",
        "../temp/FALCON4.ct",
        "../../temp/FALCON4.ct",
    };
    for (const char* rel : cwd_relative) {
        if (std::filesystem::exists(rel)) {
            return std::filesystem::path(rel);
        }
    }
    return {};
}

} // namespace f4::convert
