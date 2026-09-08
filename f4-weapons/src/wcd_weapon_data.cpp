// f4-weapons/src/wcd_weapon_data.cpp
//
// The Falcon4.WCD JSON reader + the built-in overlay. Streaming
// f4-json Reader, same discipline as f4-world-types' class_table loader
// (unknown keys are skipped, not rejected — the export schema may grow).

#include <f4/weapons/wcd_weapon_data.hpp>

#include <f4/io/read_file.hpp>
#include <f4/json/reader.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>

namespace f4::weapons {

namespace {

// Case-insensitive, whitespace-trimmed name comparison. WCD names come out
// of fixed-width char arrays; the decode trims, but an export from another
// producer may not have.
[[nodiscard]] std::string normalized(std::string_view s) {
    std::string out;
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    out.reserve(e - b);
    for (std::size_t i = b; i < e; ++i) {
        out.push_back(static_cast<char>(std::toupper(
            static_cast<unsigned char>(s[i]))));
    }
    return out;
}

[[nodiscard]] WcdWeaponRecord parse_entry(json::Reader& r) {
    WcdWeaponRecord e;
    r.skip_ws(); r.expect('{');
    while (!r.consume('}')) {
        std::string key = r.read_string();
        r.expect(':');
        if (key == "index")                        e.index = static_cast<int>(r.read_int());
        else if (key == "strength")                e.strength = static_cast<int>(r.read_int());
        else if (key == "damage_type")             e.damage_type = static_cast<int>(r.read_int());
        else if (key == "range_km")                e.range_km = static_cast<int>(r.read_int());
        else if (key == "flags")                   e.flags = static_cast<int>(r.read_int());
        else if (key == "name")                    e.name = r.read_string();
        else if (key == "hit_chance") {
            r.skip_ws(); r.expect('[');
            for (int m = 0; m < 8; ++m) {
                e.hit_chance[static_cast<std::size_t>(m)] =
                    static_cast<std::uint8_t>(r.read_int());
                if (m < 7) { r.skip_ws(); r.expect(','); }
            }
            r.skip_ws(); r.expect(']');
        }
        else if (key == "fire_rate")               e.fire_rate = static_cast<int>(r.read_int());
        else if (key == "rarity")                  e.rarity = static_cast<int>(r.read_int());
        else if (key == "guidance_flags")          e.guidance_flags = static_cast<int>(r.read_int());
        else if (key == "collective")              e.collective = static_cast<int>(r.read_int());
        else if (key == "simweap_index")           e.simweap_index = static_cast<int>(r.read_int());
        else if (key == "weight")                  e.weight = static_cast<int>(r.read_int());
        else if (key == "drag_index")              e.drag_index = static_cast<int>(r.read_int());
        else if (key == "blast_radius")            e.blast_radius = static_cast<int>(r.read_int());
        else if (key == "radar_type")              e.radar_type = static_cast<int>(r.read_int());
        else if (key == "sim_data_idx")            e.sim_data_idx = static_cast<int>(r.read_int());
        else if (key == "max_alt")                 e.max_alt = static_cast<int>(r.read_int());
        else                                       r.skip_value();
        (void)r.consume(',');
    }
    return e;
}

} // namespace

const WcdWeaponRecord* WcdWeaponData::find_by_name(
    std::string_view name) const noexcept {
    const std::string want = normalized(name);
    if (want.empty()) return nullptr;
    for (const auto& e : entries) {
        if (normalized(e.name) == want) return &e;
    }
    return nullptr;
}

WcdLoadResult load_wcd_weapon_json_string(std::string_view json_text) {
    WcdLoadResult out;
    try {
        const std::string owned(json_text);  // Reader owns-position API takes a string
        json::Reader r(owned);
        r.skip_ws(); r.expect('{');
        bool saw_format = false;
        while (!r.consume('}')) {
            std::string key = r.read_string();
            r.expect(':');
            if (key == "format") {
                const auto fmt = r.read_string();
                if (fmt != "f4-weapon-class-table") {
                    out.errors.push_back(
                        "wcd_weapon_data: not a f4-weapon-class-table "
                        "document (format='" + fmt + "')");
                    return out;
                }
                saw_format = true;
            } else if (key == "version") {
                const auto v = r.read_int();
                if (v != 1) {
                    out.warnings.push_back(
                        "wcd_weapon_data: unexpected version " +
                        std::to_string(v) + " (expected 1) — parsing anyway");
                }
            } else if (key == "source") {
                out.data.source = r.read_string();
            } else if (key == "source_fingerprint") {
                out.data.source_fingerprint = r.read_string();
            } else if (key == "count") {
                (void)r.read_int();  // advisory; entries is the truth
            } else if (key == "entries") {
                r.skip_ws(); r.expect('[');
                while (!r.consume(']')) {
                    out.data.entries.push_back(parse_entry(r));
                    r.skip_ws();
                    (void)r.consume(',');
                }
            } else {
                r.skip_value();
            }
            (void)r.consume(',');
        }
        if (!saw_format) {
            out.errors.push_back("wcd_weapon_data: missing 'format' key");
            return out;
        }
        if (out.data.entries.empty()) {
            out.warnings.push_back("wcd_weapon_data: zero entries");
        }
        out.ok = true;
    } catch (const std::exception& e) {
        out.errors.push_back(std::string("wcd_weapon_data: parse failed: ") +
                             e.what());
    }
    return out;
}

WcdLoadResult load_wcd_weapon_json(const std::filesystem::path& path) {
    WcdLoadResult out;
    try {
        auto bytes = f4::io::read_file(path, "wcd_weapon_data");
        std::string text(bytes.begin(), bytes.end());
        return load_wcd_weapon_json_string(text);
    } catch (const std::exception& e) {
        out.errors.push_back(std::string("wcd_weapon_data: cannot read '") +
                             path.string() + "': " + e.what());
        return out;
    }
}

std::size_t overlay_wcd_weapon_data(
    WeaponClassTable& base,
    const WcdWeaponData& wcd,
    const std::vector<std::pair<std::string, std::string>>& engine_to_wcd_name,
    std::vector<std::string>* warnings) {

    // km -> ft. The sim is imperial end to end; WCD ranges are km.
    constexpr double kKmToFt = 3280.83989501312;

    std::size_t updated = 0;
    for (const auto& [engine_name, wcd_name] : engine_to_wcd_name) {
        const std::uint32_t handle = base.find_by_name(engine_name);
        if (handle == kInvalidWeapon) {
            if (warnings) {
                warnings->push_back(
                    "overlay_wcd_weapon_data: engine name '" + engine_name +
                    "' not in table — left untouched");
            }
            continue;
        }
        const auto* rec = wcd.find_by_name(wcd_name);
        if (rec == nullptr) {
            if (warnings) {
                warnings->push_back(
                    "overlay_wcd_weapon_data: WCD name '" + wcd_name +
                    "' (for engine '" + engine_name + "') not in export — "
                    "left untouched");
            }
            continue;
        }
        auto* rec_mut = base.get_mut(handle);
        if (rec_mut == nullptr) continue;  // unreachable; defensive
        rec_mut->max_range_ft = static_cast<double>(rec->range_km) * kKmToFt;
        rec_mut->launch_mass_lb = static_cast<double>(rec->weight);
        rec_mut->warhead_power_lb = static_cast<double>(rec->strength);
        rec_mut->lethal_radius_ft = static_cast<double>(rec->blast_radius);
        ++updated;
    }
    return updated;
}

} // namespace f4::weapons
