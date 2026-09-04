// f4-world-convert/src/campaign_json.cpp
//
// from_world_json_campaign — parse the "campaign" JSON block (emitted by
// to_world_json) back into a CampaignHeader. The inverse of the campaign
// block in world_json.cpp's to_world_json().
//
// Walks the JSON with f4::json::Reader, reading every field encode_cmp
// needs. Fields absent from the JSON default to 0/empty — the encoder
// writes those as zero. This makes the parser robust to older JSON files
// that predate the te_number_f16s / camp_map_b64 / squadrons /
// remaining_payload_b64 additions.

#include <f4/world_convert/campaign_json.hpp>
#include <f4/json/reader.hpp>

#include <stdexcept>
#include <string>
#include <vector>

namespace f4::world_convert {

namespace {

using f4::json::Reader;

// base64 decoder — the inverse of world_json.cpp's base64_encode.
// (Duplicated from cam_writer.cpp's anonymous namespace; both are internal
// to f4-world-convert. A shared f4-convert base64 utility is a future
// cleanup, not required for this tranche.)
std::vector<uint8_t> base64_decode(const std::string& s) {
    static constexpr int8_t kInvalid = -1;
    auto val = [](char c) -> int8_t {
        if (c >= 'A' && c <= 'Z') return static_cast<int8_t>(c - 'A');
        if (c >= 'a' && c <= 'z') return static_cast<int8_t>(c - 'a' + 26);
        if (c >= '0' && c <= '9') return static_cast<int8_t>(c - '0' + 52);
        if (c == '+') return 62;
        if (c == '/') return 63;
        return kInvalid;
    };
    std::vector<uint8_t> out;
    out.reserve((s.size() / 4) * 3);
    int buf = 0, bits = 0;
    for (char c : s) {
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        if (c == '=') break;
        const int8_t v = val(c);
        if (v < 0) throw std::runtime_error("campaign_json: invalid base64");
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((buf >> bits) & 0xFF));
        }
    }
    return out;
}

// Read a JSON array of integers into a vector<int32_t>, resizing to `count`
// (padding with 0 if the JSON array is shorter — robustness for older files).
void read_i32_array(Reader& r, std::vector<int32_t>& out, std::size_t count) {
    out.assign(count, 0);
    r.expect('[');
    if (r.consume(']')) return;
    std::size_t i = 0;
    for (;;) {
        if (i < count) out[i] = static_cast<int32_t>(r.read_int());
        else r.skip_value();   // JSON array longer than `count` — ignore extras
        ++i;
        if (r.consume(']')) break;
        r.expect(',');
    }
}

// Read the teams array. Each entry has: slot, flags, colour, name, motto
// (the .cmp fields) plus optional .tea enrichment (cteam, stance, etc.)
// which we skip — encode_cmp only needs the .cmp team fields.
void read_teams(Reader& r, CampaignHeader& h) {
    h.teams.assign(8, TeamEntry{});
    r.expect('[');
    if (r.consume(']')) return;
    for (;;) {
        r.expect('{');
        if (r.consume('}')) goto next;
        {
            std::size_t slot = 0;
            TeamEntry t;
            bool have_slot = false;
            for (;;) {
                std::string key = r.read_string();
                r.expect(':');
                if (key == "slot") {
                    slot = static_cast<std::size_t>(r.read_int());
                    have_slot = true;
                } else if (key == "flags") {
                    t.flags = static_cast<uint8_t>(r.read_int());
                } else if (key == "colour") {
                    t.colour = static_cast<uint8_t>(r.read_int());
                } else if (key == "name") {
                    t.name = r.read_string();
                } else if (key == "motto") {
                    t.motto = r.read_string();
                } else {
                    r.skip_value();
                }
                if (r.consume('}')) break;
                r.expect(',');
            }
            if (have_slot && slot < h.teams.size()) {
                h.teams[slot] = t;
            }
        }
    next:
        if (r.consume(']')) break;
        r.expect(',');
    }
}

// Read the events array (standard or priority).
void read_events(Reader& r, std::vector<CampaignEvent>& out) {
    out.clear();
    r.expect('[');
    if (r.consume(']')) return;
    for (;;) {
        r.expect('{');
        if (r.consume('}')) goto next;
        {
            CampaignEvent e;
            for (;;) {
                std::string key = r.read_string();
                r.expect(':');
                if (key == "x") e.x = static_cast<int16_t>(r.read_int());
                else if (key == "y") e.y = static_cast<int16_t>(r.read_int());
                else if (key == "time") e.time = static_cast<int32_t>(r.read_int());
                else if (key == "flags") e.flags = static_cast<uint8_t>(r.read_int());
                else if (key == "team") e.team = static_cast<uint8_t>(r.read_int());
                else if (key == "text") e.text = r.read_string();
                else r.skip_value();
                if (r.consume('}')) break;
                r.expect(',');
            }
            out.push_back(std::move(e));
        }
    next:
        if (r.consume(']')) break;
        r.expect(',');
    }
}

// Read the squadrons array (SquadronUIInfo records).
void read_squadrons(Reader& r, std::vector<SquadronUIInfo>& out) {
    out.clear();
    r.expect('[');
    if (r.consume(']')) return;
    for (;;) {
        r.expect('{');
        if (r.consume('}')) goto next;
        {
            SquadronUIInfo s;
            for (;;) {
                std::string key = r.read_string();
                r.expect(':');
                if (key == "x") s.x = static_cast<float>(r.read_number());
                else if (key == "y") s.y = static_cast<float>(r.read_number());
                else if (key == "id_num") s.id_num = static_cast<uint32_t>(r.read_int());
                else if (key == "id_creator") s.id_creator = static_cast<uint32_t>(r.read_int());
                else if (key == "d_index") s.d_index = static_cast<int16_t>(r.read_int());
                else if (key == "name_id") s.name_id = static_cast<int16_t>(r.read_int());
                else if (key == "airbase_icon") s.airbase_icon = static_cast<int16_t>(r.read_int());
                else if (key == "squadron_patch") s.squadron_patch = static_cast<int16_t>(r.read_int());
                else if (key == "specialty") s.specialty = static_cast<uint8_t>(r.read_int());
                else if (key == "current_strength") s.current_strength = static_cast<uint8_t>(r.read_int());
                else if (key == "country") s.country = static_cast<uint8_t>(r.read_int());
                else if (key == "airbase_name") s.airbase_name = r.read_string();
                else r.skip_value();
                if (r.consume('}')) break;
                r.expect(',');
            }
            out.push_back(std::move(s));
        }
    next:
        if (r.consume(']')) break;
        r.expect(',');
    }
}

} // namespace

int read_world_json_version(const std::string& world_json) {
    Reader r(world_json);
    r.skip_ws();
    r.expect('{');
    if (r.consume('}')) return 63;
    for (;;) {
        std::string key = r.read_string();
        r.expect(':');
        if (key == "version") {
            return static_cast<int>(r.read_int());
        }
        r.skip_value();
        if (r.consume('}')) break;
        r.expect(',');
    }
    return 63;
}

CampaignHeader from_world_json_campaign(const std::string& world_json, int camp_version) {
    CampaignHeader h;
    // Version-gated fields: set the flags the encoder checks.
    // (The encoder reads camp_version, not h.* — we pass it through.)

    Reader r(world_json);
    r.skip_ws();
    r.expect('{');

    bool found = false;
    if (r.consume('}')) goto done;
    for (;;) {
        std::string key = r.read_string();
        r.expect(':');
        if (key == "campaign") {
            found = true;
            // The campaign block is an object. Parse every field.
            r.expect('{');
            if (r.consume('}')) goto done;
            for (;;) {
                std::string ck = r.read_string();
                r.expect(':');
                if (ck == "current_time") h.current_time = static_cast<int32_t>(r.read_int());
                else if (ck == "te_start_time") h.te_start_time = static_cast<int32_t>(r.read_int());
                else if (ck == "te_time_limit") h.te_time_limit = static_cast<int32_t>(r.read_int());
                else if (ck == "te_victory_points") h.te_victory_points = static_cast<int32_t>(r.read_int());
                else if (ck == "te_type") h.te_type = static_cast<int32_t>(r.read_int());
                else if (ck == "te_number_teams") h.te_number_teams = static_cast<int32_t>(r.read_int());
                else if (ck == "te_team") h.te_team = static_cast<int32_t>(r.read_int());
                else if (ck == "te_flags") h.te_flags = static_cast<int32_t>(r.read_int());
                else if (ck == "te_number_aircraft") read_i32_array(r, h.te_number_aircraft, 8);
                else if (ck == "te_number_f16s") read_i32_array(r, h.te_number_f16s, 8);
                else if (ck == "te_team_pts") read_i32_array(r, h.te_team_pts, 8);
                else if (ck == "teams") read_teams(r, h);
                else if (ck == "last_major_event") h.last_major_event = static_cast<int32_t>(r.read_int());
                else if (ck == "last_resupply") h.last_resupply = static_cast<int32_t>(r.read_int());
                else if (ck == "last_repair") h.last_repair = static_cast<int32_t>(r.read_int());
                else if (ck == "last_reinforcement") h.last_reinforcement = static_cast<int32_t>(r.read_int());
                else if (ck == "time_stamp") h.time_stamp = static_cast<int16_t>(r.read_int());
                else if (ck == "group") h.group = static_cast<int16_t>(r.read_int());
                else if (ck == "ground_ratio") h.ground_ratio = static_cast<int16_t>(r.read_int());
                else if (ck == "air_ratio") h.air_ratio = static_cast<int16_t>(r.read_int());
                else if (ck == "air_defense_ratio") h.air_defense_ratio = static_cast<int16_t>(r.read_int());
                else if (ck == "naval_ratio") h.naval_ratio = static_cast<int16_t>(r.read_int());
                else if (ck == "brief") h.brief = static_cast<int16_t>(r.read_int());
                else if (ck == "theater_size_x") h.theater_size_x = static_cast<int16_t>(r.read_int());
                else if (ck == "theater_size_y") h.theater_size_y = static_cast<int16_t>(r.read_int());
                else if (ck == "current_day") h.current_day = static_cast<uint8_t>(r.read_int());
                else if (ck == "active_teams") h.active_teams = static_cast<uint8_t>(r.read_int());
                else if (ck == "day_zero") h.day_zero = static_cast<uint8_t>(r.read_int());
                else if (ck == "endgame_result") h.endgame_result = static_cast<uint8_t>(r.read_int());
                else if (ck == "situation") h.situation = static_cast<uint8_t>(r.read_int());
                else if (ck == "enemy_air_exp") h.enemy_air_exp = static_cast<uint8_t>(r.read_int());
                else if (ck == "enemy_ad_exp") h.enemy_ad_exp = static_cast<uint8_t>(r.read_int());
                else if (ck == "bullseye_name") h.bullseye_name = static_cast<uint8_t>(r.read_int());
                else if (ck == "bullseye_x") h.bullseye_x = static_cast<int16_t>(r.read_int());
                else if (ck == "bullseye_y") h.bullseye_y = static_cast<int16_t>(r.read_int());
                else if (ck == "theater_name") h.theater_name = r.read_string();
                else if (ck == "scenario") h.scenario = r.read_string();
                else if (ck == "save_file") h.save_file = r.read_string();
                else if (ck == "ui_name") h.ui_name = r.read_string();
                else if (ck == "player_squadron_id") h.player_squadron_num = static_cast<uint32_t>(r.read_int());
                else if (ck == "player_squadron_creator") h.player_squadron_creator = static_cast<uint32_t>(r.read_int());
                else if (ck == "tempo") h.tempo = static_cast<uint8_t>(r.read_int());
                else if (ck == "creator_ip") h.creator_ip = static_cast<int32_t>(r.read_int());
                else if (ck == "creation_time") h.creation_time = static_cast<int32_t>(r.read_int());
                else if (ck == "creation_rand") h.creation_rand = static_cast<int32_t>(r.read_int());
                else if (ck == "camp_map_size") h.camp_map_size = static_cast<int16_t>(r.read_int());
                else if (ck == "camp_map_b64") h.camp_map = base64_decode(r.read_string());
                else if (ck == "last_index_num") h.last_index_num = static_cast<int16_t>(r.read_int());
                else if (ck == "num_avail_squadrons") h.num_avail_squadrons = static_cast<int16_t>(r.read_int());
                else if (ck == "squadrons") read_squadrons(r, h.squadrons);
                else if (ck == "remaining_payload_b64") h.remaining_payload = base64_decode(r.read_string());
                else if (ck == "events") read_events(r, h.standard_events);
                else if (ck == "priority_events") read_events(r, h.priority_events);
                else r.skip_value();
                if (r.consume('}')) break;
                r.expect(',');
            }
        } else {
            r.skip_value();
        }
        if (r.consume('}')) break;
        r.expect(',');
    }
done:
    if (!found) {
        throw std::runtime_error(
            "from_world_json_campaign: no \"campaign\" block in world JSON");
    }
    // Ensure the 8-element arrays are full (read_i32_array handles this,
    // but a JSON that omits the field entirely leaves them empty — the
    // encoder writes 0 for each missing element, which is correct).
    if (h.te_number_aircraft.size() < 8) h.te_number_aircraft.resize(8, 0);
    if (h.te_number_f16s.size() < 8) h.te_number_f16s.resize(8, 0);
    if (h.te_team_pts.size() < 8) h.te_team_pts.resize(8, 0);
    if (h.teams.size() < 8) h.teams.resize(8, TeamEntry{});
    return h;
}

} // namespace f4::world_convert
