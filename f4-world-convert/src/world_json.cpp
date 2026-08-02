// f4-world-convert/src/world_json.cpp

#include <f4/convert/world_json.hpp>
#include <f4/convert/objective_decoder.hpp>
#include <f4/convert/unit_decoder.hpp>

#include <cmath>
#include <sstream>
#include <stdexcept>

namespace f4::convert {

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

std::string to_world_json(const CamArchive& cam) {
    std::ostringstream o;
    o << "{\n";

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
                o << "    \"items\": [\n";
                for (std::size_t i = 0; i < objs.objectives.size(); ++i) {
                    const auto& ob = objs.objectives[i];
                    o << "      {\"type\": " << ob.type
                      << ", \"type_name\": \"" << json_escape(objective_type_name(ob.type)) << "\""
                      << ", \"x\": " << ob.x
                      << ", \"y\": " << ob.y
                      << ", \"z\": " << (std::isnan(ob.z) ? 0.0f : ob.z)
                      << ", \"owner\": " << static_cast<int>(ob.owner)
                      << ", \"nameid\": " << ob.nameid
                      << ", \"priority\": " << static_cast<int>(ob.priority)
                      << ", \"camp_id\": " << ob.camp_id
                      << "}";
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
                o << "    \"items\": [\n";
                for (std::size_t i = 0; i < units.units.size(); ++i) {
                    const auto& u = units.units[i];
                    o << "      {\"type\": " << u.type
                      << ", \"x\": " << u.x
                      << ", \"y\": " << u.y
                      << ", \"z\": " << (std::isnan(u.z) ? 0.0f : u.z)
                      << ", \"owner\": " << static_cast<int>(u.owner)
                      << ", \"name_id\": " << u.name_id
                      << ", \"dest_x\": " << u.dest_x
                      << ", \"dest_y\": " << u.dest_y
                      << "}";
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

} // namespace f4::convert
