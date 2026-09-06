// f4-world-types/src/class_table.cpp
//
// Runtime-safe class table implementation: JSON loader + lookup methods.
// The binary FALCON4.ct decoder (ClassTable::load) + find_class_table()
// live in f4-world-convert (importer-only, links f4-io + f4-install).

#include <f4/world_types/class_table.hpp>

#include <f4/io/read_file.hpp>
#include <f4/json/reader.hpp>

#include <stdexcept>
#include <string>

namespace f4::world_types {

void ClassTable::load_json(const std::filesystem::path& json_path) {
    // Tranche 0a.1: load the class table from the JSON form produced by
    // ct2json. Same ClassTableEntry fields, behavior-preserving vs the
    // binary load() in f4-world-convert.
    auto file_bytes = io::read_file(json_path);
    std::string json_str(file_bytes.begin(), file_bytes.end());

    json::Reader r(json_str);
    r.skip_ws(); r.expect('{');
    std::size_t count = 0;
    while (!r.consume('}')) {
        std::string key = r.read_string();
        r.expect(':');
        if (key == "count") {
            count = static_cast<std::size_t>(r.read_int());
        } else if (key == "entries") {
            r.skip_ws(); r.expect('[');
            entries_.clear();
            entries_.reserve(count);
            while (!r.consume(']')) {
                r.skip_ws(); r.expect('{');
                ClassTableEntry e;
                while (!r.consume('}')) {
                    std::string ek = r.read_string();
                    r.expect(':');
                    if (ek == "entity_type") {
                        r.read_int();  // entity_type = 100 + index; implicit
                    } else if (ek == "domain") {
                        e.domain = static_cast<uint8_t>(r.read_int());
                    } else if (ek == "cls") {
                        e.cls = static_cast<uint8_t>(r.read_int());
                    } else if (ek == "type") {
                        e.type = static_cast<uint8_t>(r.read_int());
                    } else if (ek == "stype") {
                        e.stype = static_cast<uint8_t>(r.read_int());
                    } else if (ek == "vis_type") {
                        r.skip_ws(); r.expect('[');
                        for (int vi = 0; vi < 7; ++vi) {
                            e.vis_type[vi] = static_cast<int16_t>(r.read_int());
                            if (vi < 6) { r.skip_ws(); r.expect(','); }
                        }
                        r.skip_ws(); r.expect(']');
                    } else if (ek == "data_type") {
                        e.data_type = static_cast<uint8_t>(r.read_int());
                    } else if (ek == "data_ptr_index") {
                        e.data_ptr_index = static_cast<uint32_t>(r.read_int());
                    } else {
                        r.skip_value();
                    }
                    (void)r.consume(',');
                }
                entries_.push_back(e);
                r.skip_ws();
                (void)r.consume(',');
            }
        } else {
            r.skip_value();
        }
        (void)r.consume(',');
    }
    if (entries_.empty()) {
        throw std::runtime_error("class_table: JSON had no entries");
    }
}

void ClassTable::load_auto(const std::filesystem::path& path) {
    // Tranche 0a.3: format-aware dispatch. .json -> load_json.
    // The runtime does NOT link the binary .ct decoder; a .ct path here
    // is an error (the caller should convert via ct2json first, or link
    // f4-world-convert directly if binary loading is genuinely needed).
    const auto ext = path.extension().string();
    if (ext == ".json") {
        load_json(path);
    } else {
        throw std::runtime_error(
            "f4::world_types::ClassTable::load_auto: binary .ct loading is "
            "not available in the runtime (links f4-world-types, not "
            "f4-world-convert). Convert the file to JSON via `ct2json` "
            "first, or link f4-world-convert. Path: " + path.string());
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

int16_t ClassTable::vis_type_for(uint16_t entity_type, int slot) const noexcept {
    if (slot < 0 || slot >= 7) return 0;
    const auto* e = lookup(entity_type);
    if (!e) return 0;
    return e->vis_type[slot];
}

} // namespace f4::world_types
