// f4-world-convert/src/scenario_pack.cpp
//
// The scenario pack parser — strict, house-parser discipline
// (f4::json::Reader, the wire protocol's own engine). See
// scenario_pack.hpp for the schema and the vocabulary mapping.

#include <f4/world_convert/scenario_pack.hpp>

#include <f4/io/read_file.hpp>
#include <f4/json/reader.hpp>

#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace f4::world_convert {
namespace {

using f4::json::Reader;

[[noreturn]] void pack_error(const std::string& what) {
    throw std::runtime_error("pack: " + what);
}

std::string where(Reader& r) {
    std::ostringstream ss;
    ss << " at offset " << r.position();
    return ss.str();
}

// Read an object's key stream, dispatching each key to `handle`, and
// reject unknown keys by name (the wire protocol's strictness).
template <typename Handle>
void parse_object(Reader& r, const char* what, Handle&& handle) {
    r.skip_ws();
    r.expect('{');
    if (r.consume('}')) return;
    for (;;) {
        r.skip_ws();
        const std::string key = r.read_string();
        r.skip_ws();
        r.expect(':');
        if (!handle(key)) pack_error(std::string(what) + ": unknown key \"" +
                                     key + "\"" + where(r));
        r.skip_ws();
        if (r.consume('}')) break;
        r.expect(',');
    }
}

int read_int_field(Reader& r) {
    return static_cast<int>(r.read_int());
}

} // namespace

PackRelation relation_from_word(const std::string& word) {
    if (word == "none") return PackRelation::NoRelations;
    if (word == "allied") return PackRelation::Allied;
    if (word == "friendly") return PackRelation::Friendly;
    if (word == "neutral") return PackRelation::Neutral;
    if (word == "hostile") return PackRelation::Hostile;
    if (word == "war") return PackRelation::War;
    pack_error("teams: unknown relation word \"" + word +
               "\" (none|allied|friendly|neutral|hostile|war)");
}

int objective_type_from_kind(const std::string& kind) {
    if (kind == "airbase") return 1;
    if (kind == "airstrip") return 2;
    if (kind == "armybase") return 3;
    if (kind == "beach") return 4;
    if (kind == "border") return 5;
    if (kind == "bridge") return 6;
    if (kind == "city") return 8;
    if (kind == "depot") return 10;
    if (kind == "factory") return 11;
    if (kind == "port") return 19;
    if (kind == "powerplant") return 20;
    if (kind == "radar") return 21;
    if (kind == "rail_terminal") return 23;
    if (kind == "town") return 39;
    pack_error("objectives: unknown kind \"" + kind + "\"");
}

int battalion_subtype_from_kind(const std::string& kind) {
    if (kind == "air_defense") return 1;   // STYPE_LAND_AIR_DEFENSE
    if (kind == "airmobile") return 2;     // STYPE_LAND_AIRMOBILE
    if (kind == "armor") return 3;         // STYPE_LAND_ARMOR
    if (kind == "armored_cav") return 4;   // STYPE_LAND_ARMORED_CAV
    if (kind == "engineer") return 5;      // STYPE_LAND_ENGINEER
    if (kind == "hq") return 6;            // STYPE_LAND_HQ
    if (kind == "infantry") return 7;      // STYPE_LAND_INFANTRY
    if (kind == "marine") return 8;        // STYPE_LAND_MARINE
    pack_error("battalions: unknown kind \"" + kind + "\"");
}

int squadron_specialty_from_word(const std::string& word) {
    if (word == "none") return 0;
    if (word == "AA") return 1;
    if (word == "AG") return 2;
    pack_error("squadrons: unknown specialty \"" + word +
               "\" (none|AA|AG)");
}

// The pack's cross-reference pass. The initializer assumes a consistent
// pack — every fault a typo could introduce is named HERE, at the parse,
// never mid-build.
void validate(const ScenarioPack& p) {
    if (p.pack_version != 1)
        pack_error("\"pack\" must be 1 (got " + std::to_string(p.pack_version) + ")");
    if (p.name.empty()) pack_error("\"name\" is required");
    if (p.name.size() > 32)
        pack_error("\"name\" must be <= 32 chars (it names the archive stems)");
    if (p.camp_version != 63 && p.camp_version != 71)
        pack_error("\"camp_version\" must be 63 or 71 (the two live layouts)");

    // The camp map rides the .cmp's i16 CampMapSize: sx*sy*2 bits / 8.
    const long long cells = static_cast<long long>(p.theater.size_x) *
                            static_cast<long long>(p.theater.size_y);
    if (p.theater.size_x < 32 || p.theater.size_y < 32 ||
        cells * 2 / 8 > 32767)
        pack_error("theater sizes must be >= 32 with sx*sy*2/8 <= 32767 "
                   "(the .cmp's i16 CampMapSize cap)");

    if (p.teams.size() < 2)
        pack_error("a war needs at least 2 named teams");
    if (p.teams.size() > 8)
        pack_error("at most 8 teams (the wire's team vocabulary)");
    std::set<int> slots;
    std::set<std::string> names;
    for (const auto& t : p.teams) {
        if (t.slot < 0 || t.slot >= 8)
            pack_error("team slot " + std::to_string(t.slot) + " out of range 0..7");
        if (!slots.insert(t.slot).second)
            pack_error("duplicate team slot " + std::to_string(t.slot));
        if (t.name.empty()) pack_error("team names must be non-empty");
        if (!names.insert(t.name).second)
            pack_error("duplicate team name \"" + t.name + "\"");
        if (t.reserve_aircraft < 0 || t.reserve_aircraft > 9999)
            pack_error("team \"" + t.name + "\" reserve_aircraft out of range");
        if (t.replacements < 0 || t.replacements > 65535)
            pack_error("team \"" + t.name + "\" replacements out of range");
    }
    const auto named = [&](int slot) {
        for (const auto& t : p.teams)
            if (t.slot == slot) return true;
        return false;
    };

    for (const auto& d : p.relations) {
        if (!named(d.a) || !named(d.b))
            pack_error("relation references an unnamed team slot");
        if (d.a == d.b) pack_error("a team cannot be at war with itself");
    }

    if (p.objectives.empty()) pack_error("an OOB needs at least 1 objective");
    if (p.objectives.size() > 2000)
        pack_error("at most 2000 objectives per pack (keep generated wars bounded)");
    std::set<std::string> obj_names;
    for (const auto& o : p.objectives) {
        if (o.name.empty()) pack_error("objective names must be non-empty");
        if (!obj_names.insert(o.name).second)
            pack_error("duplicate objective name \"" + o.name + "\"");
        if (o.x < 0 || o.x >= p.theater.size_x || o.y < 0 || o.y >= p.theater.size_y)
            pack_error("objective \"" + o.name + "\" sits outside the theater grid");
        if (o.owner != 0 && !named(o.owner))
            pack_error("objective \"" + o.name + "\" owner slot " +
                       std::to_string(o.owner) + " is not a named team");
        if (o.priority < 0 || o.priority > 100)
            pack_error("objective \"" + o.name + "\" priority out of range 0..100");
        (void)objective_type_from_kind(o.kind);   // vocabulary check (throws)
    }

    std::set<std::pair<int, int>> seen_links;
    for (const auto& l : p.links) {
        if (l.a < 0 || static_cast<std::size_t>(l.a) >= p.objectives.size() ||
            l.b < 0 || static_cast<std::size_t>(l.b) >= p.objectives.size())
            pack_error("link references an objective index out of range");
        if (l.a == l.b) pack_error("a link cannot join an objective to itself");
        const std::pair<int, int> key{std::min(l.a, l.b), std::max(l.a, l.b)};
        if (!seen_links.insert(key).second)
            pack_error("duplicate link (each pair is encoded in both directions)");
    }

    if (p.squadrons.size() > 128) pack_error("at most 128 squadrons per pack");
    for (const auto& s : p.squadrons) {
        if (!named(s.team))
            pack_error("squadron \"" + s.name + "\" team slot is not a named team");
        if (!obj_names.count(s.base))
            pack_error("squadron \"" + s.name + "\" base \"" + s.base +
                       "\" names no objective");
        if (s.size < 1 || s.size > 48)
            pack_error("squadron \"" + s.name + "\" size out of range 1..48");
        if (s.skill < 0 || s.skill > 9)
            pack_error("squadron \"" + s.name + "\" skill out of range 0..9");
        (void)squadron_specialty_from_word(s.specialty);   // vocabulary check
    }

    if (p.battalions.size() > 512) pack_error("at most 512 battalions per pack");
    for (const auto& b : p.battalions) {
        if (!named(b.team))
            pack_error("battalion team slot is not a named team");
        if (!obj_names.count(b.at))
            pack_error("battalion site \"" + b.at + "\" names no objective");
        if (b.groups < 1 || b.groups > 16)
            pack_error("battalion groups out of range 1..16");
        (void)battalion_subtype_from_kind(b.kind);         // vocabulary check
    }

    if (p.player_team != 0 && !named(p.player_team))
        pack_error("player_team slot is not a named team");
}

ScenarioPack ScenarioPack::parse(const std::string& json) {
    ScenarioPack p;
    Reader r(json);

    parse_object(r, "pack", [&](const std::string& key) {
        if (key == "pack") { p.pack_version = read_int_field(r); return true; }
        if (key == "name") { p.name = r.read_string(); return true; }
        if (key == "camp_version") { p.camp_version = read_int_field(r); return true; }
        if (key == "seed") { p.seed = static_cast<uint32_t>(r.read_int()); return true; }
        if (key == "theater") {
            parse_object(r, "theater", [&](const std::string& tk) {
                if (tk == "name") { p.theater.name = r.read_string(); return true; }
                if (tk == "ui_name") { p.theater.ui_name = r.read_string(); return true; }
                if (tk == "scenario") { p.theater.scenario = r.read_string(); return true; }
                if (tk == "save_file") { p.theater.save_file = r.read_string(); return true; }
                if (tk == "id") { p.theater.id = r.read_string(); return true; }
                if (tk == "terrain_file") { p.theater.terrain_file = r.read_string(); return true; }
                if (tk == "size_x") { p.theater.size_x = read_int_field(r); return true; }
                if (tk == "size_y") { p.theater.size_y = read_int_field(r); return true; }
                return false;
            });
            return true;
        }
        if (key == "date") {
            parse_object(r, "date", [&](const std::string& dk) {
                if (dk == "current_time") { p.date.current_time = static_cast<int32_t>(r.read_int()); return true; }
                if (dk == "day") { p.date.day = read_int_field(r); return true; }
                return false;
            });
            return true;
        }
        if (key == "weather") {
            parse_object(r, "weather", [&](const std::string& wk) {
                if (wk == "seed") { p.weather.seed = static_cast<uint32_t>(r.read_int()); return true; }
                if (wk == "condition") { p.weather.condition = r.read_string(); return true; }
                return false;
            });
            return true;
        }
        if (key == "bullseye") {
            parse_object(r, "bullseye", [&](const std::string& bk) {
                if (bk == "x") { p.bullseye_x = read_int_field(r); return true; }
                if (bk == "y") { p.bullseye_y = read_int_field(r); return true; }
                if (bk == "name") { p.bullseye_name = read_int_field(r); return true; }
                return false;
            });
            return true;
        }
        if (key == "player_team") { p.player_team = read_int_field(r); return true; }
        if (key == "tempo") { p.tempo = read_int_field(r); return true; }
        if (key == "teams") {
            r.skip_ws();
            r.expect('[');
            if (!r.consume(']')) {
                for (;;) {
                    PackTeam t;
                    parse_object(r, "teams[]", [&](const std::string& tk) {
                        if (tk == "slot") { t.slot = read_int_field(r); return true; }
                        if (tk == "name") { t.name = r.read_string(); return true; }
                        if (tk == "motto") { t.motto = r.read_string(); return true; }
                        if (tk == "colour") { t.colour = read_int_field(r); return true; }
                        if (tk == "air_experience") { t.air_experience = read_int_field(r); return true; }
                        if (tk == "ground_experience") { t.ground_experience = read_int_field(r); return true; }
                        if (tk == "naval_experience") { t.naval_experience = read_int_field(r); return true; }
                        if (tk == "air_defense_experience") { t.air_defense_experience = read_int_field(r); return true; }
                        if (tk == "reserve_aircraft") { t.reserve_aircraft = read_int_field(r); return true; }
                        if (tk == "replacements") { t.replacements = read_int_field(r); return true; }
                        if (tk == "supply") { t.supply = read_int_field(r); return true; }
                        if (tk == "fuel") { t.fuel = read_int_field(r); return true; }
                        return false;
                    });
                    p.teams.push_back(std::move(t));
                    r.skip_ws();
                    if (r.consume(']')) break;
                    r.expect(',');
                }
            }
            return true;
        }
        if (key == "relations") {
            r.skip_ws();
            r.expect('[');
            if (!r.consume(']')) {
                for (;;) {
                    PackRelationDecl d;
                    parse_object(r, "relations[]", [&](const std::string& rk) {
                        if (rk == "a") { d.a = read_int_field(r); return true; }
                        if (rk == "b") { d.b = read_int_field(r); return true; }
                        if (rk == "stance") { d.stance = relation_from_word(r.read_string()); return true; }
                        return false;
                    });
                    p.relations.push_back(d);
                    r.skip_ws();
                    if (r.consume(']')) break;
                    r.expect(',');
                }
            }
            return true;
        }
        if (key == "objectives") {
            r.skip_ws();
            r.expect('[');
            if (!r.consume(']')) {
                for (;;) {
                    PackObjective o;
                    parse_object(r, "objectives[]", [&](const std::string& ok) {
                        if (ok == "name") { o.name = r.read_string(); return true; }
                        if (ok == "kind") { o.kind = r.read_string();
                                            (void)objective_type_from_kind(o.kind); return true; }
                        if (ok == "x") { o.x = read_int_field(r); return true; }
                        if (ok == "y") { o.y = read_int_field(r); return true; }
                        if (ok == "owner") { o.owner = read_int_field(r); return true; }
                        if (ok == "priority") { o.priority = read_int_field(r); return true; }
                        if (ok == "supply") { o.supply = read_int_field(r); return true; }
                        if (ok == "fuel") { o.fuel = read_int_field(r); return true; }
                        if (ok == "entity_type") { o.entity_type = read_int_field(r); return true; }
                        return false;
                    });
                    p.objectives.push_back(std::move(o));
                    r.skip_ws();
                    if (r.consume(']')) break;
                    r.expect(',');
                }
            }
            return true;
        }
        if (key == "links") {
            r.skip_ws();
            r.expect('[');
            if (!r.consume(']')) {
                for (;;) {
                    // Each link is a 2-element array of objective indices.
                    r.skip_ws();
                    r.expect('[');
                    PackLink l;
                    r.skip_ws();
                    l.a = read_int_field(r);
                    r.skip_ws();
                    r.expect(',');
                    r.skip_ws();
                    l.b = read_int_field(r);
                    r.skip_ws();
                    r.expect(']');
                    p.links.push_back(l);
                    r.skip_ws();
                    if (r.consume(']')) break;
                    r.expect(',');
                }
            }
            return true;
        }
        if (key == "squadrons") {
            r.skip_ws();
            r.expect('[');
            if (!r.consume(']')) {
                for (;;) {
                    PackSquadron s;
                    parse_object(r, "squadrons[]", [&](const std::string& sk) {
                        if (sk == "team") { s.team = read_int_field(r); return true; }
                        if (sk == "base") { s.base = r.read_string(); return true; }
                        if (sk == "name") { s.name = r.read_string(); return true; }
                        if (sk == "size") { s.size = read_int_field(r); return true; }
                        if (sk == "skill") { s.skill = read_int_field(r); return true; }
                        if (sk == "specialty") { s.specialty = r.read_string();
                                                 (void)squadron_specialty_from_word(s.specialty); return true; }
                        if (sk == "entity_type") { s.entity_type = read_int_field(r); return true; }
                        return false;
                    });
                    p.squadrons.push_back(std::move(s));
                    r.skip_ws();
                    if (r.consume(']')) break;
                    r.expect(',');
                }
            }
            return true;
        }
        if (key == "battalions") {
            r.skip_ws();
            r.expect('[');
            if (!r.consume(']')) {
                for (;;) {
                    PackBattalion b;
                    parse_object(r, "battalions[]", [&](const std::string& bk) {
                        if (bk == "team") { b.team = read_int_field(r); return true; }
                        if (bk == "at") { b.at = r.read_string(); return true; }
                        if (bk == "kind") { b.kind = r.read_string();
                                            (void)battalion_subtype_from_kind(b.kind); return true; }
                        if (bk == "groups") { b.groups = read_int_field(r); return true; }
                        if (bk == "entity_type") { b.entity_type = read_int_field(r); return true; }
                        return false;
                    });
                    p.battalions.push_back(std::move(b));
                    r.skip_ws();
                    if (r.consume(']')) break;
                    r.expect(',');
                }
            }
            return true;
        }
        return false;
    });

    validate(p);
    return p;
}

ScenarioPack ScenarioPack::load(const std::string& path) {
    const std::vector<uint8_t> bytes =
        f4::io::read_file(std::filesystem::path(path), "ScenarioPack");
    return ScenarioPack::parse(
        std::string(bytes.begin(), bytes.end()));
}

} // namespace f4::world_convert
