// f4-world/src/theater_tables.cpp
//
// CAMP-SCALE-1 — the runtime reader for the converted theater tables.
// See include/f4/world/theater_tables.hpp for the contract.

#include <f4/world/theater_tables.hpp>

#include <f4/json/reader.hpp>

#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace f4::world {

namespace {

std::string slurp_file(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        throw std::runtime_error(
            "theater_tables: cannot open " + path.string());
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string to_lower_ascii(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(
            static_cast<unsigned char>(c)));
    }
    return s;
}

// --- array helpers (positional; the emitter writes flat arrays) -----------

void read_byte_array(f4::json::Reader& r, std::vector<uint8_t>& out) {
    r.skip_ws();
    r.expect('[');
    if (r.consume(']')) return;
    for (;;) {
        out.push_back(static_cast<uint8_t>(r.read_int()));
        if (r.consume(']')) break;
        r.expect(',');
    }
}

void read_short_array(f4::json::Reader& r, std::vector<int16_t>& out) {
    r.skip_ws();
    r.expect('[');
    if (r.consume(']')) return;
    for (;;) {
        out.push_back(static_cast<int16_t>(r.read_int()));
        if (r.consume(']')) break;
        r.expect(',');
    }
}

void read_int_array(f4::json::Reader& r, std::vector<int32_t>& out) {
    r.skip_ws();
    r.expect('[');
    if (r.consume(']')) return;
    for (;;) {
        out.push_back(static_cast<int32_t>(r.read_int()));
        if (r.consume(']')) break;
        r.expect(',');
    }
}

// --- row parsers (the emitter's exact field names) -------------------------

TheaterUnitRow parse_unit_row(f4::json::Reader& r) {
    TheaterUnitRow u;
    r.skip_ws();
    r.expect('{');
    if (r.consume('}')) return u;
    for (;;) {
        const std::string k = r.read_string();
        r.expect(':');
        if      (k == "index")            u.index          = static_cast<int16_t>(r.read_int());
        else if (k == "name")             u.name           = r.read_string();
        else if (k == "flags")            u.flags          = static_cast<uint32_t>(r.read_int());
        else if (k == "movement_type")    u.movement_type  = static_cast<int32_t>(r.read_int());
        else if (k == "movement_type_name") (void)r.skip_value();
        else if (k == "movement_speed")   u.movement_speed = static_cast<int16_t>(r.read_int());
        else if (k == "max_range")        u.max_range      = static_cast<int16_t>(r.read_int());
        else if (k == "fuel")             u.fuel           = static_cast<int32_t>(r.read_int());
        else if (k == "rate")             u.rate           = static_cast<int16_t>(r.read_int());
        else if (k == "pt_data_index")    u.pt_data_index  = static_cast<int16_t>(r.read_int());
        else if (k == "num_elements")     read_int_array(r, u.num_elements);
        else if (k == "vehicle_type")     read_short_array(r, u.vehicle_type);
        else if (k == "scores")           read_byte_array(r, u.scores);
        else if (k == "role")             u.role           = static_cast<uint8_t>(r.read_int());
        else if (k == "hit_chance")       read_byte_array(r, u.hit_chance);
        else if (k == "strength")         read_byte_array(r, u.strength);
        else if (k == "range")            read_byte_array(r, u.range);
        else if (k == "detection")        read_byte_array(r, u.detection);
        else if (k == "damage_mod")       read_byte_array(r, u.damage_mod);
        else if (k == "radar_vehicle")    u.radar_vehicle  = static_cast<uint8_t>(r.read_int());
        else if (k == "special_index")    u.special_index  = static_cast<int16_t>(r.read_int());
        else if (k == "icon_index")       u.icon_index     = static_cast<int16_t>(r.read_int());
        else                              r.skip_value();
        if (r.consume('}')) break;
        r.expect(',');
    }
    return u;
}

TheaterVehicleRow parse_vehicle_row(f4::json::Reader& r) {
    TheaterVehicleRow v;
    r.skip_ws();
    r.expect('{');
    if (r.consume('}')) return v;
    for (;;) {
        const std::string k = r.read_string();
        r.expect(':');
        if      (k == "index")            v.index           = static_cast<int16_t>(r.read_int());
        else if (k == "name")             v.name            = r.read_string();
        else if (k == "nctr")             v.nctr            = r.read_string();
        else if (k == "hit_points")       v.hit_points      = static_cast<int16_t>(r.read_int());
        else if (k == "flags")            v.flags           = static_cast<uint32_t>(r.read_int());
        else if (k == "rcs_factor")       v.rcs_factor      = static_cast<float>(r.read_number());
        else if (k == "max_wt")           v.max_wt          = static_cast<int32_t>(r.read_int());
        else if (k == "empty_wt")         v.empty_wt        = static_cast<int32_t>(r.read_int());
        else if (k == "fuel_wt")          v.fuel_wt         = static_cast<int32_t>(r.read_int());
        else if (k == "fuel_econ")        v.fuel_econ       = static_cast<int16_t>(r.read_int());
        else if (k == "engine_sound")     v.engine_sound    = static_cast<int16_t>(r.read_int());
        else if (k == "high_alt")         v.high_alt        = static_cast<int16_t>(r.read_int());
        else if (k == "low_alt")          v.low_alt         = static_cast<int16_t>(r.read_int());
        else if (k == "cruise_alt")       v.cruise_alt      = static_cast<int16_t>(r.read_int());
        else if (k == "max_speed")        v.max_speed       = static_cast<int16_t>(r.read_int());
        else if (k == "radar_type")       v.radar_type      = static_cast<int16_t>(r.read_int());
        else if (k == "number_of_pilots") v.number_of_pilots = static_cast<int16_t>(r.read_int());
        else if (k == "rack_flags")       v.rack_flags      = static_cast<uint16_t>(r.read_int());
        else if (k == "visible_flags")    v.visible_flags   = static_cast<uint16_t>(r.read_int());
        else if (k == "callsign_index")   v.callsign_index  = static_cast<uint8_t>(r.read_int());
        else if (k == "callsign_slots")   v.callsign_slots  = static_cast<uint8_t>(r.read_int());
        else if (k == "hit_chance")       read_byte_array(r, v.hit_chance);
        else if (k == "strength")         read_byte_array(r, v.strength);
        else if (k == "range")            read_byte_array(r, v.range);
        else if (k == "detection")        read_byte_array(r, v.detection);
        else if (k == "weapon")           read_short_array(r, v.weapon);
        else if (k == "weapons")          read_byte_array(r, v.weapons);
        else if (k == "damage_mod")       read_byte_array(r, v.damage_mod);
        else                              r.skip_value();
        if (r.consume('}')) break;
        r.expect(',');
    }
    return v;
}

TheaterWeaponRow parse_weapon_row(f4::json::Reader& r) {
    TheaterWeaponRow w;
    r.skip_ws();
    r.expect('{');
    if (r.consume('}')) return w;
    for (;;) {
        const std::string k = r.read_string();
        r.expect(':');
        if      (k == "index")            w.index          = static_cast<int16_t>(r.read_int());
        else if (k == "name")             w.name           = r.read_string();
        else if (k == "strength")         w.strength       = static_cast<uint16_t>(r.read_int());
        else if (k == "damage_type")      w.damage_type    = static_cast<int32_t>(r.read_int());
        else if (k == "range_km")         w.range_km       = static_cast<int16_t>(r.read_int());
        else if (k == "flags")            w.flags          = static_cast<uint16_t>(r.read_int());
        else if (k == "fire_rate")        w.fire_rate      = static_cast<uint8_t>(r.read_int());
        else if (k == "rarity")           w.rarity         = static_cast<uint8_t>(r.read_int());
        else if (k == "guidance_flags")   w.guidance_flags = static_cast<uint16_t>(r.read_int());
        else if (k == "collective")       w.collective     = static_cast<uint8_t>(r.read_int());
        else if (k == "simweap_index")    w.simweap_index  = static_cast<int16_t>(r.read_int());
        else if (k == "weight")           w.weight         = static_cast<uint16_t>(r.read_int());
        else if (k == "drag_index")       w.drag_index     = static_cast<int16_t>(r.read_int());
        else if (k == "blast_radius")     w.blast_radius   = static_cast<uint16_t>(r.read_int());
        else if (k == "radar_type")       w.radar_type     = static_cast<int16_t>(r.read_int());
        else if (k == "sim_data_idx")     w.sim_data_idx   = static_cast<int16_t>(r.read_int());
        else if (k == "max_alt")          w.max_alt        = static_cast<int8_t>(r.read_int());
        else if (k == "hit_chance") {
            std::vector<uint8_t> hc;
            read_byte_array(r, hc);
        }
        else                              r.skip_value();
        if (r.consume('}')) break;
        r.expect(',');
    }
    return w;
}

} // namespace

TheaterTables TheaterTables::parse(const std::string& json) {
    TheaterTables t;
    f4::json::Reader r(json);

    r.skip_ws();
    r.expect('{');
    if (r.consume('}')) return t;
    for (;;) {
        const std::string k = r.read_string();
        r.expect(':');
        if (k == "format") {
            const std::string fmt = r.read_string();
            if (fmt != "f4.theater.tables/1") {
                throw std::runtime_error(
                    "theater_tables: unsupported format \"" + fmt + "\"");
            }
        } else if (k == "counts") {
            r.skip_value();  // informational; the arrays carry their own truth
        } else if (k == "units") {
            r.skip_ws();
            r.expect('[');
            if (!r.consume(']')) {
                for (;;) {
                    t.units.push_back(parse_unit_row(r));
                    if (r.consume(']')) break;
                    r.expect(',');
                }
            }
        } else if (k == "vehicles") {
            r.skip_ws();
            r.expect('[');
            if (!r.consume(']')) {
                for (;;) {
                    t.vehicles.push_back(parse_vehicle_row(r));
                    if (r.consume(']')) break;
                    r.expect(',');
                }
            }
        } else if (k == "weapons") {
            r.skip_ws();
            r.expect('[');
            if (!r.consume(']')) {
                for (;;) {
                    t.weapons.push_back(parse_weapon_row(r));
                    if (r.consume(']')) break;
                    r.expect(',');
                }
            }
        } else {
            r.skip_value();  // additive exporter fields never break the reader
        }
        if (r.consume('}')) break;
        r.expect(',');
    }
    return t;
}

TheaterTables TheaterTables::load(const std::filesystem::path& path) {
    return parse(slurp_file(path));
}

std::optional<CountermeasureCounts>
resolve_countermeasures(const TheaterTables& tables,
                        const f4::world_types::ClassTable& ct,
                        std::uint16_t vehicle_entity_type) noexcept {
    // Class-table row: the vehicle's own entry must point into the VCD.
    uint8_t data_type = 0;
    uint32_t data_ptr = 0;
    if (!ct.data_ptr_for(vehicle_entity_type, data_type, data_ptr)) {
        return std::nullopt;
    }
    if (data_type != static_cast<uint8_t>(f4::world_types::DTYPE_VEHICLE)) {
        return std::nullopt;
    }
    const auto* v = tables.vehicle_at(data_ptr);
    if (v == nullptr) return std::nullopt;

    CountermeasureCounts out{};
    bool found = false;
    const std::size_t hardpoints = v->weapon.size();
    for (std::size_t i = 0; i < hardpoints; ++i) {
        const int16_t wid = v->weapon[i];
        if (wid < 0) continue;
        const auto* w = tables.weapon_at(static_cast<std::size_t>(wid));
        if (w == nullptr) continue;
        const std::string name = to_lower_ascii(w->name);
        const auto shots = i < v->weapons.size()
            ? static_cast<int>(v->weapons[i]) : 0;
        if (shots <= 0) continue;
        if (name == "chaff") {
            out.chaff_rounds += shots;
            found = true;
        } else if (name == "flare") {
            out.flare_rounds += shots;
            found = true;
        }
    }
    if (!found) return std::nullopt;
    return out;
}

} // namespace f4::world
