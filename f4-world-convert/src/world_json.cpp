// f4-world-convert/src/world_json.cpp

#include <f4/world_convert/world_json.hpp>
#include <f4/world_convert/objective_decoder.hpp>
#include <f4/world_convert/unit_decoder.hpp>
#include <f4/world_convert/team_decoder.hpp>
#include <f4/json/writer.hpp>
#include <f4/io/read_file.hpp>

#include <cmath>
#include <optional>
#include <sstream>
#include <stdexcept>

namespace f4::world_convert {

namespace {

// Use the shared JSON string escaper from f4-json instead of a local copy.
// The shared version handles the same escapes (\" \\ \n \r \t \u00xx) plus
// \b and \f, which the local copy missed. For world_json's inputs (theater
// names, team names, file paths, objective names) the difference is moot —
// none of these contain backspace or formfeed characters — but using the
// shared function eliminates the duplication and ensures we stay compliant
// with the JSON spec.
using f4::json::escape_string;

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

std::filesystem::path find_base_objectives(const std::filesystem::path& cam_path) {
    namespace fs = std::filesystem;
    // 1. Any .obj next to the .cam (case-insensitive extension match).
    if (!cam_path.empty()) {
        fs::path dir = cam_path.parent_path();
        std::error_code ec;
        if (!dir.empty() && fs::is_directory(dir, ec)) {
            for (const auto& entry : fs::directory_iterator(dir, ec)) {
                if (!entry.is_regular_file()) continue;
                const auto ext = entry.path().extension().string();
                if (ext.size() == 4 &&
                    std::tolower(static_cast<unsigned char>(ext[1])) == 'o' &&
                    std::tolower(static_cast<unsigned char>(ext[2])) == 'b' &&
                    std::tolower(static_cast<unsigned char>(ext[3])) == 'j') {
                    return entry.path();
                }
            }
        }
    }
    // 2. CWD-relative well-known paths (mirrors find_class_table_cwd_
    //    fallback). save1.obj is the stock korea scenario's base
    //    objective list extracted from the bundled save1.cam fixture.
    const char* candidates[] = {
        "save1.obj",
        "assets/save1.obj",
        "temp/save1.obj",
        "f4-world-convert/tests/fixtures/save1.obj",
        "../f4-world-convert/tests/fixtures/save1.obj",
        "../../f4-world-convert/tests/fixtures/save1.obj",
        "../temp/save1.obj",
        "../../temp/save1.obj",
    };
    for (const char* rel : candidates) {
        if (fs::exists(rel)) return fs::path(rel);
    }
    return {};
}

std::string to_world_json(const CamArchive& cam, const WorldJsonOptions& opts) {
    std::ostringstream o;
    o << "{\n";

    // --- Theater + terrain reference (NEW) ---
    // The world JSON is paired with a separate terrain JSON. We record the
    // theater name and the terrain file path so consumers can load both.
    o << "  \"theater\": \"" << escape_string(opts.theater) << "\",\n";
    o << "  \"terrain_file\": \"" << escape_string(opts.terrain_file) << "\",\n";

    // --- Container manifest ---
    o << "  \"archive\": {\n";
    o << "    \"file_size\": " << cam.raw_bytes().size() << ",\n";
    o << "    \"subfiles\": [\n";
    const auto& sfs = cam.subfiles();
    for (std::size_t i = 0; i < sfs.size(); ++i) {
        const auto& sf = sfs[i];
        o << "      {\"name\": \"" << escape_string(sf.name) << "\", "
          << "\"offset\": " << sf.offset << ", "
          << "\"size\": " << sf.size << "}";
        if (i + 1 < sfs.size()) o << ",";
        o << "\n";
    }
    o << "    ]\n";
    o << "  },\n";

    // --- Version (.ver) --- read first: every sub-file decoder is
    // version-parameterized (gCampDataVersion gates several on-disk
    // layouts — pos_.z_ at v70+, ushort current_wp/wp_count at v71+,
    // squadron stores[220] at v69+, flight v>65 fields, ...).
    int camp_version = 63;   // the save1.cam fixture default
    const SubFile* ver = cam.find("ver");
    if (ver) {
        camp_version = read_version(ver->data.data(), ver->data.size());
        o << "  \"version\": " << camp_version << ",\n";
    }

    // --- Campaign header (.cmp) ---
    const SubFile* cmp = cam.find("cmp");
    if (cmp) {
        CampaignHeader h = decode_cmp(cmp->data.data(), cmp->data.size(),
                                      camp_version);
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
        // Decode .tea for team enrichment (stance, experience, member countries).
        // The .tea decoder scans for all TeamClass blocks; if it fails or
        // finds fewer teams than .cmp, we just omit the enrichment fields.
        DecodedTeams tea_teams;
        const SubFile* tea_sf = cam.find("tea");
        if (tea_sf) {
            try {
                tea_teams = decode_tea(tea_sf->data.data(), tea_sf->data.size(),
                                       camp_version);
            } catch (...) {
                // .tea parse failed — proceed with .cmp-only team data
            }
        }
        for (std::size_t i = 0; i < h.teams.size(); ++i) {
            const auto& t = h.teams[i];
            o << "      {\"slot\": " << i
              << ", \"flags\": " << static_cast<int>(t.flags)
              << ", \"colour\": " << static_cast<int>(t.colour)
              << ", \"name\": \"" << escape_string(t.name) << "\""
              << ", \"motto\": \"" << escape_string(t.motto) << "\"";
            // Enrich with .tea data if available for this team slot.
            // The .tea decoder's TeamRecord.who field is the team index (0..7),
            // matching the .cmp team slot.
            for (const auto& tt : tea_teams.teams) {
                if (tt.who == i) {
                    o << ", \"cteam\": " << static_cast<int>(tt.cteam)
                      << ", \"team_flags\": " << tt.flags
                      << ", \"member\": [";
                    for (std::size_t j = 0; j < tt.member.size(); ++j) {
                        if (j) o << ", ";
                        o << static_cast<int>(tt.member[j]);
                    }
                    o << "]"
                      << ", \"stance\": [";
                    for (std::size_t j = 0; j < tt.stance.size(); ++j) {
                        if (j) o << ", ";
                        o << tt.stance[j];
                    }
                    o << "]"
                      << ", \"first_colonel\": " << tt.first_colonel
                      << ", \"first_commander\": " << tt.first_commander
                      << ", \"first_wingman\": " << tt.first_wingman
                      << ", \"last_wingman\": " << tt.last_wingman
                      << ", \"air_experience\": " << static_cast<int>(tt.air_experience)
                      << ", \"air_defense_experience\": " << static_cast<int>(tt.air_defense_experience)
                      << ", \"ground_experience\": " << static_cast<int>(tt.ground_experience)
                      << ", \"naval_experience\": " << static_cast<int>(tt.naval_experience)
                      << ", \"initiative\": " << tt.initiative
                      << ", \"supply_avail\": " << tt.supply_avail
                      << ", \"fuel_avail\": " << tt.fuel_avail
                      << ", \"replacements_avail\": " << tt.replacements_avail
                      << ", \"player_rating\": " << tt.player_rating
                      << ", \"team_flag\": " << static_cast<int>(tt.team_flag)
                      << ", \"team_color2\": " << static_cast<int>(tt.team_color)
                      << ", \"equipment\": " << static_cast<int>(tt.equipment)
                      << ", \"tea_name\": \"" << escape_string(tt.name) << "\""
                      << ", \"reinforcement\": " << tt.reinforcement
                      << ", \"current_stats\": {"
                      << "\"aircraft\":" << tt.current_aircraft
                      << ", \"air_defense_vehs\":" << tt.current_air_defense_vehs
                      << ", \"ground_vehs\":" << tt.current_ground_vehs
                      << ", \"ships\":" << tt.current_ships
                      << ", \"supply\":" << tt.current_supply
                      << ", \"fuel\":" << tt.current_fuel
                      << ", \"airbases\":" << tt.current_airbases
                      << ", \"supply_level\":" << static_cast<int>(tt.current_supply_level)
                      << ", \"fuel_level\":" << static_cast<int>(tt.current_fuel_level)
                      << "}"
                      // Team's Air Tasking Manager: airbase schedule list +
                      // pending mission requests (the ATO worklist).
                      << ", \"atm_airbases\": [";
                    for (std::size_t ai = 0; ai < tt.atm.airbases.size(); ++ai) {
                        if (ai) o << ", ";
                        o << tt.atm.airbases[ai].id_num;
                    }
                    o << "]"
                      << ", \"atm_requests\": [";
                    for (std::size_t ri = 0; ri < tt.atm.requests.size(); ++ri) {
                        const auto& mr = tt.atm.requests[ri];
                        if (ri) o << ", ";
                        o << "{\"mission\": " << static_cast<int>(mr.mission)
                          << ", \"who\": " << static_cast<int>(mr.who)
                          << ", \"vs\": " << static_cast<int>(mr.vs)
                          << ", \"tot\": " << mr.tot
                          << ", \"priority\": " << mr.priority
                          << ", \"action_type\": " << static_cast<int>(mr.action_type)
                          << ", \"context\": " << static_cast<int>(mr.context)
                          << ", \"aircraft\": " << static_cast<int>(mr.aircraft)
                          << ", \"target_num\": " << mr.target_id_num
                          << ", \"requester_num\": " << mr.requester_id_num
                          << "}";
                    }
                    o << "]";
                    break;
                }
            }
            o << "}";
            if (i + 1 < h.teams.size()) o << ",";
            o << "\n";
        }
        o << "    ],\n";
        o << "    \"decoded_bytes\": " << h.bytes_consumed << ",\n";
        o << "    \"undecoded_bytes\": " << h.remaining_payload.size() << ",\n";
        // ---- Extension fields (CampaignClass::Decode, cmpclass.cpp:1404+) ----
        // Timers, ratios, theater size, day/progress state, bullseye,
        // names, player squadron. These turn the header into a real
        // campaign-state snapshot.
        o << "    \"last_major_event\": " << h.last_major_event << ",\n";
        o << "    \"last_resupply\": " << h.last_resupply << ",\n";
        o << "    \"last_repair\": " << h.last_repair << ",\n";
        o << "    \"last_reinforcement\": " << h.last_reinforcement << ",\n";
        o << "    \"time_stamp\": " << h.time_stamp << ",\n";
        o << "    \"group\": " << h.group << ",\n";
        o << "    \"ground_ratio\": " << h.ground_ratio << ",\n";
        o << "    \"air_ratio\": " << h.air_ratio << ",\n";
        o << "    \"air_defense_ratio\": " << h.air_defense_ratio << ",\n";
        o << "    \"naval_ratio\": " << h.naval_ratio << ",\n";
        o << "    \"brief\": " << h.brief << ",\n";
        o << "    \"theater_size_x\": " << h.theater_size_x << ",\n";
        o << "    \"theater_size_y\": " << h.theater_size_y << ",\n";
        o << "    \"current_day\": " << static_cast<int>(h.current_day) << ",\n";
        o << "    \"active_teams\": " << static_cast<int>(h.active_teams) << ",\n";
        o << "    \"day_zero\": " << static_cast<int>(h.day_zero) << ",\n";
        o << "    \"endgame_result\": " << static_cast<int>(h.endgame_result) << ",\n";
        o << "    \"situation\": " << static_cast<int>(h.situation) << ",\n";
        o << "    \"enemy_air_exp\": " << static_cast<int>(h.enemy_air_exp) << ",\n";
        o << "    \"enemy_ad_exp\": " << static_cast<int>(h.enemy_ad_exp) << ",\n";
        o << "    \"bullseye_name\": " << static_cast<int>(h.bullseye_name) << ",\n";
        o << "    \"bullseye_x\": " << h.bullseye_x << ",\n";
        o << "    \"bullseye_y\": " << h.bullseye_y << ",\n";
        o << "    \"theater_name\": \"" << escape_string(h.theater_name) << "\",\n";
        o << "    \"scenario\": \"" << escape_string(h.scenario) << "\",\n";
        o << "    \"save_file\": \"" << escape_string(h.save_file) << "\",\n";
        o << "    \"ui_name\": \"" << escape_string(h.ui_name) << "\",\n";
        o << "    \"player_squadron_id\": " << h.player_squadron_num << ",\n";
        o << "    \"player_squadron_creator\": " << h.player_squadron_creator << ",\n";
        o << "    \"tempo\": " << static_cast<int>(h.tempo) << ",\n";
        o << "    \"creator_ip\": " << h.creator_ip << ",\n";
        o << "    \"creation_time\": " << h.creation_time << ",\n";
        o << "    \"creation_rand\": " << h.creation_rand << ",\n";
        o << "    \"camp_map_size\": " << h.camp_map_size << ",\n";
        o << "    \"num_avail_squadrons\": " << h.num_avail_squadrons << ",\n";
        // UI event queues — the campaign news feed (mid-campaign saves
        // carry the last ~10 events: kills, captures, missions).
        o << "    \"events\": [";
        for (std::size_t ei = 0; ei < h.standard_events.size(); ++ei) {
            const auto& e = h.standard_events[ei];
            if (ei) o << ", ";
            o << "{\"x\": " << e.x << ", \"y\": " << e.y
              << ", \"time\": " << e.time
              << ", \"flags\": " << static_cast<int>(e.flags)
              << ", \"team\": " << static_cast<int>(e.team)
              << ", \"text\": \"" << escape_string(e.text) << "\"}";
        }
        o << "],\n";
        o << "    \"priority_events\": [";
        for (std::size_t ei = 0; ei < h.priority_events.size(); ++ei) {
            const auto& e = h.priority_events[ei];
            if (ei) o << ", ";
            o << "{\"x\": " << e.x << ", \"y\": " << e.y
              << ", \"time\": " << e.time
              << ", \"flags\": " << static_cast<int>(e.flags)
              << ", \"team\": " << static_cast<int>(e.team)
              << ", \"text\": \"" << escape_string(e.text) << "\"}";
        }
        o << "]\n";
        o << "  }";

        // --- Objectives (.obj, or base .obj + .obd deltas) ---
        // A CAMP_SAVE_FULL .cam (e.g. save1.cam) embeds its objective
        // list as an "obj" sub-file. A normal in-campaign save (e.g.
        // TestCamp.cam) carries only "obd" deltas — the base list lives
        // in the scenario's .obj (opts.base_objectives_path). Mirror
        // FreeFalcon's LoadBaseObjectives + LoadObjectiveDeltas: decode
        // the base list, then apply the deltas on top. The base file's
        // gCampDataVersion can differ from the save's (FreeFalcon
        // re-reads the scenario's version for base objectives) — hence
        // the separate base_objectives_version option.
        const SubFile* obj_sf = cam.find("obj");
        const SubFile* obd_sf = cam.find("obd");
        std::optional<DecodedObjectives> objs;
        std::string objectives_source = "embedded";
        if (obj_sf) {
            try {
                objs = decode_obj(obj_sf->data.data(), obj_sf->data.size(),
                                  camp_version);
            } catch (const std::exception&) {
                // Fall through to the base-objectives path below.
            }
        }
        // No explicit base-objectives path: auto-discover one (any .obj
        // next to the .cam, else the bundled save1.obj fixture). This
        // keeps the viewer's Import .cam flow and a bare `cam2json
        // TestCamp.cam` working for normal saves without extra flags.
        std::filesystem::path base_obj_path = opts.base_objectives_path;
        if (!objs && base_obj_path.empty()) {
            base_obj_path = find_base_objectives(cam.path());
        }
        if (!objs && base_obj_path.empty()) {
            // FINAL fallback (self-contained): the bundled save1.cam
            // FIXTURE embeds the stock korea scenario's objective list as
            // its "obj" sub-file — the same bytes the extracted save1.obj
            // fixture carried (it was extracted from exactly here). This
            // makes obd-only saves decode with full objectives on a
            // pristine clone, with no out-of-repo files and no binaries
            // that a patch application can drop.
            const char* cam_candidates[] = {
                "save1.cam",
                "f4-world-convert/tests/fixtures/save1.cam",
                "../f4-world-convert/tests/fixtures/save1.cam",
                "../../f4-world-convert/tests/fixtures/save1.cam",
            };
            for (const char* rel : cam_candidates) {
                std::error_code ec;
                if (!std::filesystem::is_regular_file(rel, ec)) continue;
                try {
                    CamArchive fixture;
                    fixture.load(rel);
                    if (const SubFile* emb = fixture.find("obj")) {
                        objs = decode_obj(emb->data.data(),
                                          emb->data.size(),
                                          opts.base_objectives_version);
                        objectives_source = "embedded-fixture";
                        break;
                    }
                } catch (const std::exception&) {
                    // Unreadable — try the next candidate.
                }
            }
        }
        if (!objs && !base_obj_path.empty()) {
            try {
                auto base_data = f4::io::read_file(
                    base_obj_path, "objectives");
                objs = decode_obj(base_data.data(), base_data.size(),
                                  opts.base_objectives_version);
                objectives_source = "auto";
            } catch (const std::exception&) {
                // No usable base objectives — the world JSON carries no
                // objectives section (units still decode).
            }
        }
        std::size_t deltas_applied = 0;
        if (objs && obd_sf) {
            try {
                DecodedObjectiveDeltas deltas = decode_obd(
                    obd_sf->data.data(), obd_sf->data.size(), camp_version);
                for (const auto& d : deltas.deltas) {
                    for (auto& ob : objs->objectives) {
                        if (ob.id_num == d.id_num &&
                            ob.id_creator == d.id_creator) {
                            ob.last_repair = d.last_repair;
                            ob.owner = d.owner;
                            ob.supply = d.supply;
                            ob.fuel = d.fuel;
                            ob.losses = d.losses;
                            if (!d.fstatus.empty()) ob.fstatus = d.fstatus;
                            ++deltas_applied;
                            break;
                        }
                    }
                }
            } catch (const std::exception&) {
                // Delta decode failed — emit the base objectives as-is.
            }
        }
        if (objs) {
            try {
                const DecodedObjectives& obj_ref = *objs;
                o << ",\n  \"objectives\": {\n";
                o << "    \"count\": " << obj_ref.count << ",\n";
                o << "    \"decoded\": " << obj_ref.objectives.size() << ",\n";
                o << "    \"bytes_consumed\": " << obj_ref.bytes_consumed << ",\n";
                o << "    \"inner_size\": " << obj_ref.inner_size << ",\n";
                o << "    \"source\": \"" << objectives_source << "\",\n";
                o << "    \"deltas_applied\": " << deltas_applied << ",\n";
                o << "    \"items\": [\n";
                for (std::size_t i = 0; i < obj_ref.objectives.size(); ++i) {
                    const auto& ob = obj_ref.objectives[i];
                    // Resolve objective_type via the class table when available
                    // (1-39). Falls back to 0 = "unknown" if no class table.
                    uint8_t obj_type = 0;
                    if (opts.class_table && opts.class_table->loaded()) {
                        obj_type = opts.class_table->objective_type_for(ob.entity_type);
                    }
                    // type_name: derived from the resolved objective_type (NOT
                    // from the raw entity_type, which is a class-table index
                    // like 1776 — passing it to objective_type_name would
                    // produce "Objective#1776" instead of "Airbase").
                    o << "      {\"type\": " << ob.type
                      << ", \"id_num\": " << ob.id_num
                      << ", \"id_creator\": " << ob.id_creator
                      << ", \"type_name\": \"" << escape_string(objective_type_name(static_cast<int16_t>(obj_type))) << "\""
                      << ", \"objective_type\": " << static_cast<int>(obj_type);
                    // Theater-db enrichment (Falcon4.OCD): emit the objective's
                    // class name (e.g. "Airbase A-3", "Bridge B-12") and the
                    // # of features (buildings, runways, etc.). For airbases,
                    // also walk the PtHeader chain to emit ground layout.
                    if (opts.theater_db && opts.theater_db->objectives.loaded()
                        && obj_type > 0) {
                        const auto* ocd = opts.theater_db->objectives.at(
                            static_cast<std::size_t>(obj_type) - 1);
                        // Note: ObjDataTable is indexed by (ObjectiveType - 1)
                        // in FreeFalcon — entry 0 is "Airbase" (type 1), etc.
                        // (See entity.cpp:234-235: builds NumObjectiveTypes from
                        // the largest classInfo_[VU_TYPE] value seen.)
                        if (ocd) {
                            o << ", \"class_name\": \"" << escape_string(ocd->name) << "\""
                              << ", \"features_count\": " << static_cast<int>(ocd->features)
                              << ", \"radar_feature\": " << static_cast<int>(ocd->radar_feature)
                              << ", \"deag_distance\": " << ocd->deag_distance
                              << ", \"pt_data_index\": " << ocd->pt_data_index;
                            // OCD.Detection[8] — per-movement-type electronic
                            // detection ranges. Phase 1 fix A.7: previously
                            // decoded by theater_data.cpp but never emitted.
                            o << ", \"detection\": [";
                            for (int di = 0; di < TD_MOVEMENT_TYPES; ++di) {
                                if (di) o << ", ";
                                o << static_cast<int>(ocd->detection[di]);
                            }
                            o << "]";
                            // Per-objective feature placements (Falcon4.FED +
                            // FCD). Walks `ocd->first_feature .. first_feature +
                            // features - 1` in the FED table. Each FED record's
                            // `index` field is an entity_type; we match it
                            // against FCD records' `index` field (both are
                            // entity_types referring to the same feature class)
                            // to find the feature's name + hit points.
                            // The class-table data_ptr_for() lookup is NOT
                            // reliable here because the fixture's class table
                            // may not have DTYPE_FEATURE entries for every
                            // entity_type referenced by FED records — the
                            // FCD.index ↔ FED.index match is more direct and
                            // works against any class-table state.
                            if (ocd->features > 0 && ocd->first_feature > 0
                                && opts.theater_db->feature_entries.loaded()
                                && opts.theater_db->features.loaded()) {
                                o << ", \"features\": [";
                                bool first_feat = true;
                                for (int fi = 0; fi < ocd->features; ++fi) {
                                    const auto* fed = opts.theater_db->feature_entries.at(
                                        static_cast<std::size_t>(ocd->first_feature) + fi);
                                    if (!fed) break;
                                    if (!first_feat) o << ", ";
                                    first_feat = false;
                                    o << "{\"index\": " << fed->index
                                      << ", \"flags\": " << fed->flags
                                      << ", \"value\": " << static_cast<int>(fed->value)
                                      << ", \"offset_x\": " << fed->offset_x
                                      << ", \"offset_y\": " << fed->offset_y
                                      << ", \"offset_z\": " << fed->offset_z
                                      << ", \"facing\": " << fed->facing;
                                    // Resolve FED.index → FCD by matching the
                                    // entity_type (both records' `index` fields
                                    // are entity_types referring to the same
                                    // feature class). Linear scan is fine — FCD
                                    // is typically a few hundred entries.
                                    for (std::size_t fci = 0;
                                         fci < opts.theater_db->features.size();
                                         ++fci) {
                                        const auto* fcd =
                                            opts.theater_db->features.at(fci);
                                        if (fcd &&
                                            fcd->index == fed->index) {
                                            o << ", \"name\": \"" << escape_string(fcd->name) << "\""
                                              << ", \"hit_points\": " << fcd->hit_points
                                              << ", \"repair_time\": " << fcd->repair_time
                                              << ", \"priority\": " << static_cast<int>(fcd->priority)
                                              << ", \"feat_flags\": " << fcd->flags
                                              << ", \"radar_type\": " << fcd->radar_type;
                                            break;
                                        }
                                    }
                                    o << "}";
                                }
                                o << "]";
                            }
                        }
                    }
                    o << ", \"x\": " << ob.x
                      << ", \"y\": " << ob.y
                      << ", \"z\": " << (std::isnan(ob.z) ? 0.0f : ob.z)
                      << ", \"owner\": " << static_cast<int>(ob.owner)
                      << ", \"nameid\": " << ob.nameid
                      << ", \"priority\": " << static_cast<int>(ob.priority)
                      << ", \"camp_id\": " << ob.camp_id
                      << ", \"obj_flags\": " << ob.obj_flags
                      << ", \"supply\": " << static_cast<int>(ob.supply)
                      << ", \"fuel\": " << static_cast<int>(ob.fuel)
                      << ", \"losses\": " << static_cast<int>(ob.losses)
                      << ", \"last_repair\": " << ob.last_repair
                      << ", \"first_owner\": " << static_cast<int>(ob.first_owner)
                      << ", \"parent_id\": " << ob.parent_id_num
                      << ", \"fstatus\": [";
                    for (std::size_t fi = 0; fi < ob.fstatus.size(); ++fi) {
                        if (fi) o << ", ";
                        o << static_cast<int>(ob.fstatus[fi]);
                    }
                    o << "]";
                    if (ob.has_radar) {
                        o << ", \"has_radar\": true, \"detect_ratio\": [";
                        for (int ri = 0; ri < 8; ++ri) {
                            if (ri) o << ", ";
                            o << ob.detect_ratio[ri];
                        }
                        o << "]";
                        // Phase 3: look up the radar's actual range from
                        // Falcon4.RCD via OCD.radar_feature → FED.index →
                        // FCD.radar_type → RCD.range_km. Previously the
                        // viewer used a fabricated 32-grid-unit constant
                        // for ALL radar objectives regardless of type.
                        if (opts.theater_db && opts.theater_db->radars.loaded()
                            && opts.theater_db->features.loaded()
                            && opts.theater_db->feature_entries.loaded()
                            && obj_type > 0) {
                            const auto* ocd_r = opts.theater_db->objectives.at(
                                static_cast<std::size_t>(obj_type) - 1);
                            if (ocd_r && ocd_r->radar_feature != 255
                                && ocd_r->radar_feature > 0
                                && ocd_r->first_feature > 0) {
                                // radar_feature is the feature index within
                                // the objective's feature list; look up the
                                // corresponding FED entry.
                                const auto* fed_r = opts.theater_db->feature_entries.at(
                                    static_cast<std::size_t>(ocd_r->first_feature)
                                    + ocd_r->radar_feature - 1);
                                if (fed_r) {
                                    // Find FCD by matching index (= entity_type).
                                    int16_t radar_type_idx = -1;
                                    for (std::size_t fci = 0;
                                         fci < opts.theater_db->features.size();
                                         ++fci) {
                                        const auto* fcd_r =
                                            opts.theater_db->features.at(fci);
                                        if (fcd_r && fcd_r->index == fed_r->index) {
                                            radar_type_idx = fcd_r->radar_type;
                                            break;
                                        }
                                    }
                                    // Look up RCD by radar_type_idx.
                                    if (radar_type_idx >= 0) {
                                        const auto* rcd = opts.theater_db->radars.at(
                                            static_cast<std::size_t>(radar_type_idx));
                                        if (rcd) {
                                            o << ", \"radar_range_km\": " << rcd->range_km
                                              << ", \"radar_name\": \"" << escape_string(rcd->name) << "\""
                                              << ", \"radar_type_idx\": " << static_cast<int>(radar_type_idx);
                                        }
                                    }
                                }
                            }
                        }
                    } else {
                        o << ", \"has_radar\": false";
                    }
                    // Airbase ground layout (Falcon4.PHD/PD): walk the
                    // PtHeader chain starting at pt_data_index, emitting
                    // each list (runway/taxiway/parking) as a sub-array
                    // of points. Only emitted when theater_db is loaded
                    // AND we have an OCD entry with a non-zero pt_data_index.
                    if (opts.theater_db && opts.theater_db->objectives.loaded()
                        && opts.theater_db->pt_headers.loaded()
                        && opts.theater_db->pt_data.loaded()
                        && obj_type > 0) {
                        const auto* ocd = opts.theater_db->objectives.at(
                            static_cast<std::size_t>(obj_type) - 1);
                        if (ocd && ocd->pt_data_index > 0) {
                            o << ", \"ground_layout\": [";
                            // Walk the nextHeader chain. Hard cap at 64 hops
                            // to defend against cyclic/buggy data.
                            int16_t hdr_idx = ocd->pt_data_index;
                            bool first_hdr = true;
                            for (int hop = 0; hop < 64 && hdr_idx > 0; ++hop) {
                                const auto* hdr = opts.theater_db->pt_headers.at(
                                    static_cast<std::size_t>(hdr_idx));
                                if (!hdr) break;
                                if (!first_hdr) o << ", ";
                                first_hdr = false;
                                o << "{\"type\": " << static_cast<int>(hdr->type)
                                  << ", \"type_name\": \"" << escape_string(point_list_type_name(hdr->type)) << "\""
                                  << ", \"count\": " << static_cast<int>(hdr->count)
                                  << ", \"runway_num\": " << static_cast<int>(hdr->runway_num)
                                  << ", \"heading_deg\": " << hdr->data
                                  << ", \"sin_h\": " << hdr->sin_heading
                                  << ", \"cos_h\": " << hdr->cos_heading
                                  << ", \"points\": [";
                                for (int pi = 0; pi < hdr->count; ++pi) {
                                    const auto* pt = opts.theater_db->pt_data.at(
                                        static_cast<std::size_t>(hdr->first) + pi);
                                    if (!pt) break;
                                    if (pi) o << ", ";
                                    o << "{\"x\": " << pt->x_offset
                                      << ", \"y\": " << pt->y_offset
                                      << ", \"type\": " << static_cast<int>(pt->type)
                                      << ", \"type_name\": \"" << escape_string(point_type_name(pt->type)) << "\""
                                      << ", \"flags\": " << static_cast<int>(pt->flags)
                                      << "}";
                                }
                                o << "]}";
                                hdr_idx = hdr->next_header;
                            }
                            o << "]";
                        }
                    }
                    o << ", \"links\": [";
                    // Emit the link data (road/rail network). Each link is
                    // a neighbor VU_ID + the 8 movement costs. The viewer
                    // uses costs[Wheeled] and costs[Rail] to color the link
                    // as a road (brown) or rail (dark gray). The full
                    // costs[8] array is also emitted so the campaign A*
                    // pathfinder (architecture §11.4) can use per-mode
                    // traversal costs when computing routes.
                    for (std::size_t j = 0; j < ob.link_data.size(); ++j) {
                        const auto& lk = ob.link_data[j];
                        if (j) o << ", ";
                        o << "{\"n\": " << lk.neighbor_num
                          << ", \"c\": " << static_cast<int>(lk.neighbor_creator)
                          << ", \"road\": " << (lk.is_road() ? "true" : "false")
                          << ", \"rail\": " << (lk.is_rail() ? "true" : "false")
                          << ", \"costs\": [";
                        for (int ci = 0; ci < 8; ++ci) {
                            if (ci) o << ", ";
                            o << static_cast<int>(lk.costs[ci]);
                        }
                        o << "]}";
                    }
                    o << "]}";
                    if (i + 1 < obj_ref.objectives.size()) o << ",";
                    o << "\n";
                }
                o << "    ]\n";
                o << "  }";
            } catch (const std::exception& e) {
                o << ",\n  \"objectives\": {\"error\": \"" << escape_string(e.what()) << "\"}";
            }
        }

        // --- Units (.uni) ---
        // Class-table dispatch (the same Falcon4.ct the game loads) makes
        // subclass identification deterministic; the trial-and-error
        // validator remains as fallback. Version gates per the header
        // comment in unit_decoder.hpp.
        const SubFile* uni_sf = cam.find("uni");
        if (uni_sf) {
            try {
                UnitDecodeOptions uo;
                uo.camp_version = camp_version;
                uo.class_table = opts.class_table ? opts.class_table : nullptr;
                DecodedUnits units = decode_uni(uni_sf->data.data(), uni_sf->data.size(), uo);
                o << ",\n  \"units\": {\n";
                o << "    \"count\": " << units.count << ",\n";
                o << "    \"decoded\": " << units.units.size() << ",\n";
                o << "    \"bytes_consumed\": " << units.bytes_consumed << ",\n";
                o << "    \"inner_size\": " << units.inner_size << ",\n";
                o << "    \"items\": [\n";
                for (std::size_t i = 0; i < units.units.size(); ++i) {
                    const auto& u = units.units[i];
                    o << "      {\"type\": " << u.type
                      << ", \"id_num\": " << u.id_num
                      << ", \"id_creator\": " << u.id_creator
                      << ", \"unit_class\": \"" << unit_class_name(u.unit_class) << "\"";
                    // unit_subtype: 0 if no class table, else STYPE_UNIT_*.
                    // Emitted unconditionally so the viewer can rely on the
                    // field always being present (consistent with objective_type).
                    uint8_t unit_sub = 0;
                    uint8_t unit_domain = 0;
                    if (opts.class_table && opts.class_table->loaded()) {
                        unit_sub = opts.class_table->unit_subtype_for(u.entity_type);
                        if (const auto* e = opts.class_table->lookup(u.entity_type)) {
                            unit_domain = e->domain;
                        }
                    }
                    o << ", \"unit_subtype\": " << static_cast<int>(unit_sub)
                      << ", \"domain\": " << static_cast<int>(unit_domain);
                    // Theater-db enrichment (Falcon4.UCD + VCD): emit the
                    // unit's class name (e.g. "Armor", "Infantry", "Fighter
                    // Squadron") and the per-group vehicle composition.
                    // Looked up via class_table.data_ptr_for(entity_type) →
                    // DTYPE_UNIT → index into UCD.
                    if (opts.theater_db && opts.theater_db->units.loaded()
                        && opts.class_table && opts.class_table->loaded()) {
                        uint8_t dt = 0;
                        uint32_t dp = 0;
                        if (opts.class_table->data_ptr_for(u.entity_type, dt, dp)
                            && dt == DTYPE_UNIT) {
                            const auto* ucd = opts.theater_db->units.at(dp);
                            if (ucd) {
                                o << ", \"class_name\": \"" << escape_string(ucd->name) << "\""
                                  << ", \"movement_type\": " << ucd->movement_type
                                  << ", \"movement_type_name\": \"" << escape_string(movement_type_name(ucd->movement_type)) << "\""
                                  << ", \"movement_speed\": " << ucd->movement_speed
                                  << ", \"max_range\": " << ucd->max_range
                                  << ", \"fuel\": " << ucd->fuel
                                  << ", \"pt_data_index\": " << ucd->pt_data_index;
                                // Vehicle composition: 16 groups, each with
                                // a vehicle-type index (into VCD) and a count.
                                // Combine with roster to get live per-group
                                // counts. Emit as array of group objects.
                                o << ", \"vehicle_groups\": [";
                                bool first_grp = true;
                                for (int g = 0; g < TD_VEHICLE_GROUPS_PER_UNIT; ++g) {
                                    const int32_t n_elements = ucd->num_elements[g];
                                    const int16_t vt = ucd->vehicle_type[g];
                                    if (n_elements <= 0 || vt <= 0) continue;
                                    if (!first_grp) o << ", ";
                                    first_grp = false;
                                    // Decode live count from roster (2 bits/group)
                                    const int live = (u.roster >> (g * 2)) & 0x03;
                                    // The vehicle_type field in UCD is a 0-based
                                    // index into the class table entries[] array,
                                    // NOT an entity_type. Convert to entity_type by
                                    // adding VU_LAST_ENTITY_TYPE (100) before resolving
                                    // through the class table.
                                    const uint16_t vt_et = static_cast<uint16_t>(
                                        vt + VU_LAST_ENTITY_TYPE);
                                    o << "{\"group\": " << g
                                      << ", \"vehicle_type\": " << vt_et
                                      << ", \"count\": " << n_elements
                                      << ", \"live_count\": " << live;
                                    // Look up vehicle name from VCD via the class
                                    // table. entity_type → ClassTable.data_ptr_for()
                                    // → DTYPE_VEHICLE check → VCD index → VCD name.
                                    if (opts.theater_db->vehicles.loaded()) {
                                       const VehicleClassData* vcd = nullptr;
                                        if (opts.class_table) {
                                            uint8_t vcd_dt = 0;
                                            uint32_t vcd_idx = 0;
                                            if (opts.class_table->data_ptr_for(
                                                    vt_et, vcd_dt, vcd_idx)
                                                && vcd_dt == DTYPE_VEHICLE) {
                                                vcd = opts.theater_db->vehicles.at(
                                                    static_cast<std::size_t>(vcd_idx));
                                            }
                                        }
                                        if (vcd) {
                                            o << ", \"vehicle_name\": \"" << escape_string(vcd->name) << "\""
                                              << ", \"vehicle_nctr\": \"" << escape_string(vcd->nctr) << "\""
                                              << ", \"hit_points\": " << vcd->hit_points
                                              << ", \"max_speed\": " << vcd->max_speed;
                                        }
                                    }
                                    o << "}";
                                }
                                o << "]";
                                // UCD.Scores[16] — per-mission-role scoring.
                                // Phase 1 fix A.8: previously decoded by
                                // theater_data.cpp but never emitted. Lets
                                // the campaign AI pick the best unit class
                                // for a given mission type (BARCAP, INTERCEPT,
                                // CAS, ...). Higher score = better suited.
                                o << ", \"scores\": [";
                                for (int si = 0; si < TD_MAXIMUM_ROLES; ++si) {
                                    if (si) o << ", ";
                                    o << static_cast<int>(ucd->scores[si]);
                                }
                                o << "]";
                            }
                        }
                    }
                    // roster: 32-bit packed per-group vehicle count (2 bits
                    // per group, 16 groups, max 3 per group). GetNumVehicles(vg)
                    // = (roster >> (vg*2)) & 0x03. Live campaign state.
                    o << ", \"roster\": " << u.roster;
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
                      << ", \"waypoints\": [";
                    // Each waypoint: grid_x/y/z, arrive, action, route_action,
                    // formation, flags, optional target_id+target_building,
                    // optional depart. Drawn on the canvas as a polyline.
                    for (std::size_t wi = 0; wi < u.waypoints.size(); ++wi) {
                        const auto& w = u.waypoints[wi];
                        if (wi) o << ", ";
                        o << "{\"x\": " << w.grid_x
                          << ", \"y\": " << w.grid_y
                          << ", \"z\": " << w.grid_z
                          << ", \"arrive\": " << w.arrive
                          << ", \"action\": " << static_cast<int>(w.action)
                          << ", \"route_action\": " << static_cast<int>(w.route_action)
                          << ", \"formation\": " << static_cast<int>(w.formation)
                          << ", \"flags\": " << w.flags;
                        if (w.haves & 0x02) {
                            o << ", \"target_num\": " << w.target_id_num
                              << ", \"target_creator\": " << w.target_id_creator
                              << ", \"target_building\": " << static_cast<int>(w.target_building);
                        }
                        if (w.haves & 0x01) {
                            o << ", \"depart\": " << w.depart;
                        }
                        o << "}";
                    }
                    o << "]"
                      << ", \"supply\": " << static_cast<int>(u.subclass.supply)
                      << ", \"morale\": " << static_cast<int>(u.subclass.morale)
                      << ", \"fatigue\": " << static_cast<int>(u.subclass.fatigue)
                      << ", \"fuel\": " << u.subclass.fuel
                      << ", \"elements\": " << static_cast<int>(u.subclass.elements)
                      << ", \"losses\": " << static_cast<int>(u.losses);
                    // Hierarchy: Battalion.parent_id and Brigade.element_ids.
                    // Used by the viewer to highlight parent/child units.
                    if (u.unit_class == UnitClass::Battalion) {
                        o << ", \"parent_id\": " << u.subclass.parent_id_num
                          << ", \"last_move\": " << u.subclass.last_move
                          << ", \"last_combat\": " << u.subclass.last_combat
                          << ", \"heading\": " << static_cast<int>(u.subclass.heading)
                          << ", \"final_heading\": " << static_cast<int>(u.subclass.final_heading)
                          << ", \"position\": " << static_cast<int>(u.subclass.position);
                    } else if (u.unit_class == UnitClass::Brigade) {
                        o << ", \"element_ids\": [";
                        for (std::size_t j = 0; j < u.subclass.element_ids.size(); j += 2) {
                            if (j) o << ", ";
                            o << u.subclass.element_ids[j];  // num (creator is at j+1)
                        }
                        o << "]";
                    } else if (u.unit_class == UnitClass::Squadron) {
                        // Squadron→Airbase link: VU_ID of the objective this
                        // squadron is based at. Used by the viewer to draw
                        // a line from the squadron to its home airbase.
                        o << ", \"airbase_id\": " << u.subclass.airbase_id_num
                          << ", \"specialty\": " << static_cast<int>(u.subclass.specialty)
                          << ", \"aa_kills\": " << u.subclass.aa_kills
                          << ", \"ag_kills\": " << u.subclass.ag_kills
                          << ", \"as_kills\": " << u.subclass.as_kills
                          << ", \"an_kills\": " << u.subclass.an_kills
                          << ", \"missions_flown\": " << u.subclass.missions_flown
                          << ", \"mission_score\": " << u.subclass.mission_score
                          << ", \"total_losses\": " << static_cast<int>(u.subclass.total_losses)
                          << ", \"pilot_losses\": " << static_cast<int>(u.subclass.pilot_losses)
                          << ", \"squadron_patch\": " << static_cast<int>(u.subclass.squadron_patch);
                        // Pilot roster: 48 pilots per squadron.
                        o << ", \"pilots\": [";
                        for (std::size_t j = 0; j < u.subclass.pilots.size(); ++j) {
                            const auto& p = u.subclass.pilots[j];
                            if (j) o << ", ";
                            o << "{\"id\": " << p.pilot_id
                              << ", \"skill\": " << static_cast<int>(p.skill)
                              << ", \"rating\": " << static_cast<int>(p.rating)
                              << ", \"status\": " << static_cast<int>(p.status)
                              << ", \"aa\": " << static_cast<int>(p.aa_kills)
                              << ", \"ag\": " << static_cast<int>(p.ag_kills)
                              << ", \"as\": " << static_cast<int>(p.as_kills)
                              << ", \"an\": " << static_cast<int>(p.an_kills)
                              << ", \"missions\": " << p.missions_flown
                              << "}";
                        }
                        o << "]";
                    } else if (u.unit_class == UnitClass::Flight) {
                        // Phase 1 fix A.1: previously decoded by unit_decoder.cpp
                        // but never emitted. A Flight is a single-aircraft
                        // mission element within a Package.
                        o << ", \"flight_altitude\": " << u.subclass.altitude
                          << ", \"fuel_burnt\": " << u.subclass.fuel_burnt
                          << ", \"time_on_target\": " << u.subclass.time_on_target
                          << ", \"mission_over_time\": " << u.subclass.mission_over_time
                          << ", \"mission_target\": " << u.subclass.mission_target
                          << ", \"loadouts\": " << static_cast<int>(u.subclass.loadouts)
                          << ", \"mission\": " << static_cast<int>(u.subclass.mission)
                          << ", \"flight_priority\": " << static_cast<int>(u.subclass.priority)
                          << ", \"mission_id\": " << static_cast<int>(u.subclass.mission_id)
                          << ", \"eval_flags\": " << static_cast<int>(u.subclass.eval_flags)
                          << ", \"package_id\": " << u.subclass.package_num
                          << ", \"squadron_id\": " << u.subclass.squadron_num
                          << ", \"callsign_id\": " << static_cast<int>(u.subclass.callsign_id)
                          << ", \"callsign_num\": " << static_cast<int>(u.subclass.callsign_num)
                          << ", \"old_mission\": " << static_cast<int>(u.subclass.old_mission)
                          << ", \"mission_context\": " << static_cast<int>(u.subclass.mission_context)
                          << ", \"requester_id\": " << u.subclass.requester_num;
                        // A-G slice: the decoded loadout stations (wire
                        // weapon ids + counts, entry 0 of the save's
                        // LoadoutStruct[]). Emitted wire-faithful — the
                        // ENGINE mapping (wire id -> WeaponClassTable
                        // handle) happens at the campaign bridge, keyed on
                        // these ids. When theater data carries a WCD, the
                        // weapon's NAME is attached for human reading.
                        if (!u.subclass.loadout_stations.empty()) {
                            o << ", \"loadout_stations\": [";
                            bool first_station = true;
                            for (const auto& st : u.subclass.loadout_stations) {
                                o << (first_station ? "" : ", ")
                                  << "{\"id\": " << st.weapon_id
                                  << ", \"count\": " << st.count;
                                if (opts.theater_db &&
                                    opts.theater_db->weapons.loaded()) {
                                    if (const auto* wcd =
                                            opts.theater_db->weapons.at(
                                                st.weapon_id)) {
                                        o << ", \"name\": \""
                                          << escape_string(wcd->name)
                                          << "\"";
                                    }
                                }
                                o << "}";
                                first_station = false;
                            }
                            o << "]";
                        }
                    } else if (u.unit_class == UnitClass::Package) {
                        // Phase 1 fix A.1: previously decoded by unit_decoder.cpp
                        // but never emitted. A Package groups multiple Flights
                        // into a coordinated strike/mission.
                        o << ", \"wait_cycles\": " << static_cast<int>(u.subclass.wait_cycles)
                          << ", \"interceptor_id\": " << u.subclass.interceptor_num
                          << ", \"awacs_id\": " << u.subclass.awacs_num
                          << ", \"jstar_id\": " << u.subclass.jstar_num
                          << ", \"ecm_id\": " << u.subclass.ecm_num
                          << ", \"tanker_id\": " << u.subclass.tanker_num
                          << ", \"element_ids\": [";
                        for (std::size_t j = 0; j < u.subclass.element_ids.size(); j += 2) {
                            if (j) o << ", ";
                            o << u.subclass.element_ids[j];
                        }
                        o << "]";
                        // Mission request — either branch. The mis_request
                        // drives the package (mission, target, TOT).
                        {
                            const auto& mr = u.subclass.mis_request;
                            o << ", \"mis_request\": {"
                              << "\"mission\": " << static_cast<int>(mr.mission)
                              << ", \"tot\": " << mr.tot
                              << ", \"priority\": " << mr.priority
                              << ", \"action_type\": " << static_cast<int>(mr.action_type)
                              << ", \"target_num\": " << mr.target_id_num
                              << ", \"target_creator\": " << mr.target_id_creator
                              << ", \"requester_num\": " << mr.requester_id_num
                              << "}";
                        }
                        o << ", \"package_branch\": \""
                          << (u.subclass.package_branch == PackageBranch::Small ? "small"
                              : u.subclass.package_branch == PackageBranch::Big ? "big" : "none")
                          << "\"";
                        if (u.subclass.package_branch == PackageBranch::Big) {
                            o << ", \"flights\": " << static_cast<int>(u.subclass.flights)
                              << ", \"wait_for\": " << u.subclass.wait_for
                              << ", \"takeoff\": " << u.subclass.takeoff
                              << ", \"tp_time\": " << u.subclass.tp_time
                              << ", \"package_flags\": " << u.subclass.package_flags
                              << ", \"caps\": " << u.subclass.caps
                              << ", \"requests\": " << u.subclass.requests
                              << ", \"responses\": " << u.subclass.responses;
                            // Ingress/egress routes (same waypoint shape as
                            // unit routes).
                            for (int route = 0; route < 2; ++route) {
                                const auto& wps = (route == 0)
                                    ? u.subclass.ingress : u.subclass.egress;
                                o << ", \"" << (route == 0 ? "ingress" : "egress") << "\": [";
                                for (std::size_t wi = 0; wi < wps.size(); ++wi) {
                                    const auto& w = wps[wi];
                                    if (wi) o << ", ";
                                    o << "{\"x\": " << w.grid_x
                                      << ", \"y\": " << w.grid_y
                                      << ", \"z\": " << w.grid_z
                                      << ", \"arrive\": " << w.arrive
                                      << ", \"action\": " << static_cast<int>(w.action)
                                      << "}";
                                }
                                o << "]";
                            }
                        }
                    }
                    o << "}";
                    if (i + 1 < units.units.size()) o << ",";
                    o << "\n";
                }
                o << "    ]\n";
                o << "  }";
            } catch (const std::exception& e) {
                o << ",\n  \"units\": {\"error\": \"" << escape_string(e.what()) << "\"}";
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
