// f4-world-convert/src/world_json.cpp

#include <f4/world_convert/world_json.hpp>
#include <f4/world_convert/objective_decoder.hpp>
#include <f4/world_convert/unit_decoder.hpp>

#include <cmath>
#include <sstream>
#include <stdexcept>

namespace f4::world_convert {

namespace {

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char ch : s) {
        switch (ch) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", ch);
                    out += buf;
                } else {
                    out += ch;
                }
        }
    }
    return out;
}

std::string base64_encode(const uint8_t* data, std::size_t len) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    for (std::size_t i = 0; i < len; i += 3) {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < len) n |= static_cast<uint32_t>(data[i+1]) << 8;
        if (i + 2 < len) n |= static_cast<uint32_t>(data[i+2]);
        out += alphabet[(n >> 18) & 0x3F];
        out += alphabet[(n >> 12) & 0x3F];
        out += (i + 1 < len) ? alphabet[(n >> 6) & 0x3F] : '=';
        out += (i + 2 < len) ? alphabet[n & 0x3F]        : '=';
    }
    return out;
}

} // namespace

std::string to_world_json(const CamArchive& cam, const WorldJsonOptions& opts) {
    std::ostringstream o;
    o << "{\n";

    // --- Theater + terrain reference (NEW) ---
    // The world JSON is paired with a separate terrain JSON. We record the
    // theater name and the terrain file path so consumers can load both.
    o << "  \"theater\": \"" << json_escape(opts.theater) << "\",\n";
    o << "  \"terrain_file\": \"" << json_escape(opts.terrain_file) << "\",\n";

    // --- Container manifest ---
    o << "  \"archive\": {\n";
    o << "    \"file_size\": " << cam.raw_bytes().size() << ",\n";
    o << "    \"subfiles\": [\n";
    const auto& sfs = cam.subfiles();
    for (std::size_t i = 0; i < sfs.size(); ++i) {
        const auto& sf = sfs[i];
        o << "      {\"name\": \"" << json_escape(sf.name) << "\", "
          << "\"offset\": " << sf.offset << ", "
          << "\"size\": " << sf.size << "}";
        if (i + 1 < sfs.size()) o << ",";
        o << "\n";
    }
    o << "    ]\n";
    o << "  },\n";

    // --- Version (.ver) ---
    const SubFile* ver = cam.find("ver");
    if (ver) {
        o << "  \"version\": " << read_version(ver->data.data(), ver->data.size()) << ",\n";
    }

    // --- Campaign header (.cmp) ---
    const SubFile* cmp = cam.find("cmp");
    if (cmp) {
        CampaignHeader h = decode_cmp(cmp->data.data(), cmp->data.size());
        o << "  \"campaign\": {\n";
        o << "    \"current_time\": " << h.current_time << ",\n";
        o << "    \"te_start_time\": " << h.te_start_time << ",\n";
        o << "    \"te_time_limit\": " << h.te_time_limit << ",\n";
        o << "    \"te_victory_points\": " << h.te_victory_points << ",\n";
        o << "    \"te_type\": " << h.te_type << ",\n";
        o << "    \"te_number_teams\": " << h.te_number_teams << ",\n";
        o << "    \"te_team\": " << h.te_team << ",\n";
        o << "    \"te_flags\": " << h.te_flags << ",\n";
        o << "    \"te_number_aircraft\": [";
        for (std::size_t i = 0; i < h.te_number_aircraft.size(); ++i) {
            if (i) o << ", ";
            o << h.te_number_aircraft[i];
        }
        o << "],\n";
        o << "    \"te_team_pts\": [";
        for (std::size_t i = 0; i < h.te_team_pts.size(); ++i) {
            if (i) o << ", ";
            o << h.te_team_pts[i];
        }
        o << "],\n";
        o << "    \"teams\": [\n";
        for (std::size_t i = 0; i < h.teams.size(); ++i) {
            const auto& t = h.teams[i];
            o << "      {\"slot\": " << i
              << ", \"flags\": " << static_cast<int>(t.flags)
              << ", \"colour\": " << static_cast<int>(t.colour)
              << ", \"name\": \"" << json_escape(t.name) << "\""
              << ", \"motto\": \"" << json_escape(t.motto) << "\"}";
            if (i + 1 < h.teams.size()) o << ",";
            o << "\n";
        }
        o << "    ],\n";
        o << "    \"decoded_bytes\": " << (h.decompressed_size - static_cast<int>(h.remaining_payload.size())) << ",\n";
        o << "    \"undecoded_bytes\": " << h.remaining_payload.size() << "\n";
        o << "  }";

        // --- Objectives (.obj) ---
        const SubFile* obj_sf = cam.find("obj");
        if (obj_sf) {
            try {
                DecodedObjectives objs = decode_obj(obj_sf->data.data(), obj_sf->data.size());
                o << ",\n  \"objectives\": {\n";
                o << "    \"count\": " << objs.count << ",\n";
                o << "    \"decoded\": " << objs.objectives.size() << ",\n";
                o << "    \"bytes_consumed\": " << objs.bytes_consumed << ",\n";
                o << "    \"inner_size\": " << objs.inner_size << ",\n";
                o << "    \"items\": [\n";
                for (std::size_t i = 0; i < objs.objectives.size(); ++i) {
                    const auto& ob = objs.objectives[i];
                    o << "      {\"type\": " << ob.type
                      << ", \"type_name\": \"" << json_escape(objective_type_name(ob.type)) << "\"";
                    // If a class table is available, resolve entity_type to
                    // ObjectiveType (1-39) and emit it. This lets the viewer
                    // pick the right icon without needing the class table.
                    if (opts.class_table && opts.class_table->loaded()) {
                        const uint8_t obj_type =
                            opts.class_table->objective_type_for(ob.entity_type);
                        o << ", \"objective_type\": " << static_cast<int>(obj_type);
                    }
                    o << ", \"x\": " << ob.x
                      << ", \"y\": " << ob.y
                      << ", \"z\": " << (std::isnan(ob.z) ? 0.0f : ob.z)
                      << ", \"owner\": " << static_cast<int>(ob.owner)
                      << ", \"nameid\": " << ob.nameid
                      << ", \"priority\": " << static_cast<int>(ob.priority)
                      << ", \"camp_id\": " << ob.camp_id
                      << ", \"links\": [";
                    // Emit the link data (road/rail network). Each link is
                    // a neighbor VU_ID + the 8 movement costs. The viewer
                    // uses costs[Wheeled] and costs[Rail] to color the link
                    // as a road (brown) or rail (dark gray).
                    for (std::size_t j = 0; j < ob.link_data.size(); ++j) {
                        const auto& lk = ob.link_data[j];
                        if (j) o << ", ";
                        o << "{\"n\": " << lk.neighbor_num
                          << ", \"c\": " << static_cast<int>(lk.neighbor_creator)
                          << ", \"road\": " << (lk.is_road() ? "true" : "false")
                          << ", \"rail\": " << (lk.is_rail() ? "true" : "false")
                          << "}";
                    }
                    o << "]}";
                    if (i + 1 < objs.objectives.size()) o << ",";
                    o << "\n";
                }
                o << "    ]\n";
                o << "  }";
            } catch (const std::exception& e) {
                o << ",\n  \"objectives\": {\"error\": \"" << json_escape(e.what()) << "\"}";
            }
        }

        // --- Units (.uni) ---
        const SubFile* uni_sf = cam.find("uni");
        if (uni_sf) {
            try {
                DecodedUnits units = decode_uni(uni_sf->data.data(), uni_sf->data.size());
                o << ",\n  \"units\": {\n";
                o << "    \"count\": " << units.count << ",\n";
                o << "    \"decoded\": " << units.units.size() << ",\n";
                o << "    \"bytes_consumed\": " << units.bytes_consumed << ",\n";
                o << "    \"inner_size\": " << units.inner_size << ",\n";
                o << "    \"items\": [\n";
                for (std::size_t i = 0; i < units.units.size(); ++i) {
                    const auto& u = units.units[i];
                    o << "      {\"type\": " << u.type
                      << ", \"unit_class\": \"" << unit_class_name(u.unit_class) << "\"";
                    // If a class table is available, resolve entity_type to
                    // unit subtype (STYPE_UNIT_*) — distinguishes armor/infantry/
                    // artillery/supply/engineer battalions, fighter/bomber/transport
                    // squadrons, carrier/destroyer/frigate task forces, etc.
                    if (opts.class_table && opts.class_table->loaded()) {
                        const uint8_t sub =
                            opts.class_table->unit_subtype_for(u.entity_type);
                        o << ", \"unit_subtype\": " << static_cast<int>(sub);
                    }
                    o << ", \"x\": " << u.x
                      << ", \"y\": " << u.y
                      << ", \"z\": " << (std::isnan(u.z) ? 0.0f : u.z)
                      << ", \"owner\": " << static_cast<int>(u.owner)
                      << ", \"camp_id\": " << u.camp_id
                      << ", \"name_id\": " << u.name_id
                      << ", \"dest_x\": " << u.dest_x
                      << ", \"dest_y\": " << u.dest_y
                      << ", \"reinforcement\": " << u.reinforcement
                      << ", \"wp_count\": " << static_cast<int>(u.wp_count)
                      << ", \"supply\": " << static_cast<int>(u.subclass.supply)
                      << ", \"morale\": " << static_cast<int>(u.subclass.morale)
                      << ", \"fatigue\": " << static_cast<int>(u.subclass.fatigue)
                      << ", \"fuel\": " << u.subclass.fuel
                      << ", \"elements\": " << static_cast<int>(u.subclass.elements)
                      << ", \"losses\": " << static_cast<int>(u.losses);
                    // Hierarchy: Battalion.parent_id and Brigade.element_ids.
                    // Used by the viewer to highlight parent/child units.
                    if (u.unit_class == UnitClass::Battalion) {
                        o << ", \"parent_id\": " << u.subclass.parent_id_num;
                    } else if (u.unit_class == UnitClass::Brigade) {
                        o << ", \"element_ids\": [";
                        for (std::size_t j = 0; j < u.subclass.element_ids.size(); j += 2) {
                            if (j) o << ", ";
                            o << u.subclass.element_ids[j];  // num (creator is at j+1)
                        }
                        o << "]";
                    } else if (u.unit_class == UnitClass::Squadron) {
                        // Pilot roster: 48 pilots per squadron.
                        o << ", \"pilots\": [";
                        for (std::size_t j = 0; j < u.subclass.pilots.size(); ++j) {
                            const auto& p = u.subclass.pilots[j];
                            if (j) o << ", ";
                            o << "{\"id\": " << p.pilot_id
                              << ", \"skill\": " << static_cast<int>(p.skill)
                              << ", \"status\": " << static_cast<int>(p.status)
                              << ", \"aa\": " << static_cast<int>(p.aa_kills)
                              << ", \"ag\": " << static_cast<int>(p.ag_kills)
                              << ", \"missions\": " << p.missions_flown
                              << "}";
                        }
                        o << "]";
                    }
                    o << "}";
                    if (i + 1 < units.units.size()) o << ",";
                    o << "\n";
                }
                o << "    ]\n";
                o << "  }";
            } catch (const std::exception& e) {
                o << ",\n  \"units\": {\"error\": \"" << json_escape(e.what()) << "\"}";
            }
        }

        // Raw sub-files not yet decoded — preserved as base64 for future
        // decoders (objective deltas, weather, events, ...). This is the
        // rosetta principle: keep verbatim, decode incrementally.
        o << ",\n  \"raw_subfiles\": {\n";
        const char* preserve_exts[] = {"obd", "tea", "evt", "plt", "pst", "wth"};
        bool first = true;
        for (const char* ext : preserve_exts) {
            const SubFile* sf = cam.find(ext);
            if (!sf) continue;
            if (!first) o << ",\n";
            first = false;
            o << "    \"" << ext << "\": {\"size\": " << sf->size
              << ", \"data_b64\": \"" << base64_encode(sf->data.data(), sf->data.size()) << "\"}";
        }
        o << "\n  }\n";
    } else {
        o << "  \"campaign\": null\n";
    }

    o << "}\n";
    return o.str();
}

} // namespace f4::world_convert
