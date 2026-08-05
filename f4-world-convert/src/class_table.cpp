// f4-world-convert/src/class_table.cpp

#include <f4/world_convert/class_table.hpp>

#include <f4/install/installation.hpp>
#include <f4/io/read_file.hpp>

#include <cstring>
#include <stdexcept>

namespace f4::world_convert {

namespace {

// On-disk entry size = 81 bytes (VuEntityType with natural alignment = 60,
// + visType[7] = 14 + vehicleDataIndex = 2 + dataType = 1 + dataPtr = 4).
constexpr std::size_t ENTRY_SIZE = 81;
constexpr std::size_t CLASSINFO_OFFSET = 8;  // offset of classInfo_[8] within entry
constexpr std::size_t DATATYPE_OFFSET   = 76; // offset of dataType within entry
constexpr std::size_t DATAPTR_OFFSET    = 77; // offset of dataPtr (4 bytes, LE)

// Thin wrapper around f4::io::read_file that preserves the historical
// "class_table:" diagnostic prefix. The shared helper adds the path to
// the short-read message (a minor diagnostic improvement; the original
// class_table short-read message omitted the path).
std::vector<uint8_t> read_file(const std::filesystem::path& path) {
    return f4::io::read_file(path, "class_table");
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
        const uint8_t* datatype_p = data.data() + offset + DATATYPE_OFFSET;
        const uint8_t* dataptr_p  = data.data() + offset + DATAPTR_OFFSET;
        ClassTableEntry e;
        e.domain = classinfo[0];
        e.cls    = classinfo[1];
        e.type   = classinfo[2];
        e.stype  = classinfo[3];
        e.data_type = datatype_p[0];
        // dataPtr is a 32-bit value on disk (FF was 32-bit; the on-disk format
        // stores it as a 4-byte LE integer regardless of host pointer size).
        uint32_t data_ptr;
        std::memcpy(&data_ptr, dataptr_p, 4);
        e.data_ptr_index = data_ptr;
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

bool ClassTable::data_ptr_for(uint16_t entity_type,
                              uint8_t& out_data_type,
                              uint32_t& out_data_ptr_index) const noexcept {
    const auto* e = lookup(entity_type);
    if (!e || e->data_type == DTYPE_NOTHING) return false;
    out_data_type = e->data_type;
    out_data_ptr_index = e->data_ptr_index;
    return true;
}

std::filesystem::path find_class_table(const std::filesystem::path& reference_file) {
    // Delegates to f4::install::Installation. The Installation object is
    // constructed fresh each call (cheap — detect() is just a directory
    // walk), so this remains a stateless free function.
    //
    // Search order (preserves the pre-f4-install behavior exactly):
    //   1. Same directory as `reference_file` (typically the .cam).
    //   2. Up one or two directories from `reference_file`.
    //   3. CWD-relative well-known paths.
    //
    // The f4-install version adds case-insensitive matching (catches
    // "falcon4.ct" on Linux when the install ships "FALCON4.ct"). The
    // legacy CWD fallback is preserved verbatim via
    // f4::install::find_class_table_cwd_fallback(), so existing call
    // sites (cam2json CLI run from the build dir, viewer run from
    // source tree) continue to find the bundled fixture FALCON4.ct
    // without an install configured.
    //
    // Callers that want install-aware resolution should call
    // f4::install::Installation::find_class_table() directly, passing
    // an Installation detected from the user's install path. This free
    // function uses an empty Installation, so the install-aware step is
    // skipped and behavior is unchanged.
    f4::install::Installation inst;  // empty install — no root set
    return inst.find_class_table(reference_file);
}

} // namespace f4::world_convert
