// f4-import/src/vocab.cpp
//
// Family vocabulary table loading + family guessing. The JSON parser
// is f4-json's zero-dep streaming Reader (walk-a-known-schema style).

#include <f4/import/vocab.hpp>

#include <f4/json/reader.hpp>

#include <algorithm>
#include <cstdlib>
#include <stdexcept>

namespace f4::import {

namespace {

/// Parse one "index": { "id": ..., "channel": ... } object. The index
/// key is a JSON string (e.g. "19") — object keys are always strings.
std::pair<int32_t, AnimBinding> read_binding(f4::json::Reader& r) {
    r.skip_ws();
    const int index = std::atoi(r.read_string().c_str());
    r.expect(':');
    r.skip_ws();
    r.expect('{');
    AnimBinding b;
    while (!r.consume('}')) {
        r.skip_ws();
        std::string key = r.read_string();
        r.expect(':');
        if (key == "id") {
            b.id = r.read_string();
        } else if (key == "channel") {
            b.channel = r.read_string();
        } else {
            r.skip_value();
        }
        r.skip_ws();
        (void)r.consume(',');
    }
    return {static_cast<int32_t>(index), std::move(b)};
}

/// Parse a "dof" or "sw" object.
std::map<int32_t, AnimBinding> read_binding_map(f4::json::Reader& r) {
    std::map<int32_t, AnimBinding> out;
    r.skip_ws();
    r.expect('{');
    while (!r.consume('}')) {
        r.skip_ws();
        auto [index, binding] = read_binding(r);
        out.emplace(index, std::move(binding));
        r.skip_ws();
        (void)r.consume(',');
    }
    return out;
}

} // namespace

FamilyTable load_family_table(const std::filesystem::path& json_path) {
    std::FILE* f = std::fopen(json_path.string().c_str(), "rb");
    if (!f) {
        throw std::runtime_error("vocab: cannot open " + json_path.string());
    }
    std::string text;
    char buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    std::fclose(f);

    FamilyTable table;
    f4::json::Reader r(text);
    r.skip_ws();
    r.expect('{');
    while (!r.consume('}')) {
        r.skip_ws();
        std::string key = r.read_string();
        r.expect(':');
        if (key == "f4") {
            r.skip_value();  // schema version block — accepted, unused
        } else if (key == "family") {
            table.family = r.read_string();
        } else if (key == "dof") {
            table.dof = read_binding_map(r);
        } else if (key == "sw") {
            table.sw = read_binding_map(r);
        } else {
            r.skip_value();
        }
        r.skip_ws();
        (void)r.consume(',');
    }
    if (table.family.empty()) {
        throw std::runtime_error("vocab: " + json_path.string() +
                                 " is missing the \"family\" field");
    }
    return table;
}

std::map<std::string, FamilyTable> load_family_tables(
    const std::filesystem::path& family_dir) {
    std::map<std::string, FamilyTable> out;

    // Accept either layout: <dir>/family/*.json (the repo layout) or
    // <dir>/*.json directly.
    std::error_code ec;
    const auto scan = [&](const std::filesystem::path& dir) {
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() != ".json") continue;
            auto table = load_family_table(entry.path());
            out.emplace(table.family, std::move(table));
        }
        ec.clear();
    };

    const std::filesystem::path family_sub = family_dir / "family";
    if (std::filesystem::exists(family_sub, ec)) {
        scan(family_sub);
    } else if (std::filesystem::exists(family_dir, ec)) {
        scan(family_dir);
    }
    return out;
}

std::string guess_family(const std::vector<int>& dof_indices,
                         int effective_dofs, int effective_switches,
                         int n_slots) {
    // FreeFalcon's model families:
    //   helicopter models are driven through the HELI_* space and are
    //     identified by the ROTOR PAIR — dof 2 (HELI_MAIN_ROTOR) and
    //     dof 4 (HELI_TAIL_ROTOR) together;
    //   simple aircraft use the SIMP_* space (≤ ~24 DOFs, few slots);
    //   complex aircraft use the COMP_* space (airbrake stacks, gear
    //     stacks, weapon bays — many DOFs and many slots);
    //   air-defense/ground units use the AIRDEF_* space (radar sweep =
    //     dof 0, elevation/turret DOFs, no weapon-slot racks).
    //
    // Counts alone can't separate a 2-DOF helicopter from a 2-DOF radar
    // — the rotor pair can. Getting this wrong flips which family table
    // applies: a radar classified heli loses its sweep binding, a heli
    // classified airdef loses its rotors.
    //
    // The guess is advisory: it only decides which rename table to
    // apply, and unmapped indices always degrade to unknown.N. A wrong
    // guess costs nothing structural.
    (void)effective_switches;

    const bool has_rotor_pair =
        std::find(dof_indices.begin(), dof_indices.end(), 2) !=
            dof_indices.end() &&
        std::find(dof_indices.begin(), dof_indices.end(), 4) !=
            dof_indices.end();
    const int n = static_cast<int>(dof_indices.size());

    if (has_rotor_pair && n <= 6 && n_slots <= 1) return "heli";
    if (effective_dofs >= 10 && n_slots >= 4) return "complex";
    if (!has_rotor_pair && n <= 8 && n_slots == 0) return "airdef";
    return "simple";
}

} // namespace f4::import
