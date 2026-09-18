// f4-world-convert/src/campaign_initializer.cpp
//
// CAMP-INIT-1 — the fresh-save synthesis. See campaign_initializer.hpp
// for the shape and the documented fresh-save conventions.
//
// Every number written here is a pure function of the pack (plus the
// class table's first-match palette). The pack's seed rides the .cmp's
// CreationRand; nothing consults the clock, the filesystem, or any
// entropy source — two builds of one pack are byte-identical by
// construction.

#include <f4/world_convert/campaign_initializer.hpp>

#include <f4/world_convert/cam_archive.hpp>
#include <f4/world_convert/cam_writer.hpp>
#include <f4/world_convert/cmp_encoder.hpp>
#include <f4/world_convert/objective_encoder.hpp>
#include <f4/world_convert/team_encoder.hpp>
#include <f4/world_convert/unit_encoder.hpp>
#include <f4/world_convert/world_json.hpp>

#include <algorithm>
#include <cctype>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

namespace f4::world_convert {
namespace {

[[noreturn]] void init_error(const std::string& what) {
    throw std::runtime_error("init: " + what);
}

// ── small deterministic helpers ──────────────────────────────────────────

// Pack an aircraft/vehicle count into the wire's 2-bit-per-group roster
// bitfield (16 groups; full groups first, remainder in the next group).
// snapshot_squadron_force's roster_group_aircraft() and the ground war's
// vehicle reads decode the same field.
[[nodiscard]] uint32_t pack_roster(int count) {
    uint32_t roster = 0;
    int left = std::max(0, count);
    for (int g = 0; g < 16 && left > 0; ++g) {
        const int in_group = std::min(3, left);
        roster |= static_cast<uint32_t>(in_group) << (2 * g);
        left -= in_group;
    }
    return roster;
}

// Archive sub-file stem from the pack name: alnum/underscore/dash,
// lowercased (the stock archives' "save1.*" convention).
[[nodiscard]] std::string archive_stem(const std::string& name) {
    std::string stem;
    for (const char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c)) != 0)
            stem.push_back(static_cast<char>(std::tolower(
                static_cast<unsigned char>(c))));
        else if (c == '_' || c == '-')
            stem.push_back(c);
        // every other character drops
    }
    if (stem.empty()) stem = "generated";
    return stem;
}

// Fixed-width name fields in the .cmp/.tea blocks (CAMP_NAME_SIZE and
// MAX_TEAM_NAME_SIZE); the encoders NUL-terminate and zero-pad.
void clamp_name(std::string& s, std::size_t width) {
    if (s.size() >= width) s.resize(width - 1);
}

// ── the class-table palette ──────────────────────────────────────────────

// First-match entity types for the pack's named kinds. The scan is over
// the table's own entity_type range (from VU_LAST_ENTITY_TYPE), so the
// resolution is deterministic for a given table — the documented rule.
struct Palette {
    std::map<int, uint16_t> objective;    // ObjectiveType (1..39) → entity
    uint16_t squadron = 0;                // first (UNIT, AIR, VU_TYPE 3)
    std::map<int, uint16_t> battalion;    // STYPE_LAND_* → entity
};

Palette resolve_palette(const ClassTable& ct) {
    Palette p;
    const int first = VU_LAST_ENTITY_TYPE;         // 100
    const int end = first + static_cast<int>(ct.size());
    for (int et = first; et < end; ++et) {
        const auto* e = ct.lookup(static_cast<uint16_t>(et));
        if (e == nullptr) continue;
        if (e->cls == CLASS_OBJECTIVE) {
            if (e->type >= 1 && e->type <= 39 &&
                p.objective.find(e->type) == p.objective.end())
                p.objective.emplace(e->type, static_cast<uint16_t>(et));
        } else if (e->cls == CLASS_UNIT) {
            if (e->domain == DOMAIN_AIR && e->type == 3 && p.squadron == 0)
                p.squadron = static_cast<uint16_t>(et);
            if (e->domain == DOMAIN_LAND && e->type == 1 &&
                p.battalion.find(e->stype) == p.battalion.end())
                p.battalion.emplace(e->stype, static_cast<uint16_t>(et));
        }
    }
    return p;
}

uint16_t objective_entity(const Palette& pal, const PackObjective& o,
                          const ClassTable& ct) {
    if (o.entity_type != 0) {
        if (ct.lookup(static_cast<uint16_t>(o.entity_type)) == nullptr)
            init_error("objective \"" + o.name +
                       "\" pins entity_type " + std::to_string(o.entity_type) +
                       " which the class table does not carry");
        return static_cast<uint16_t>(o.entity_type);
    }
    const int want = objective_type_from_kind(o.kind);
    const auto it = pal.objective.find(want);
    if (it == pal.objective.end())
        init_error("the class table carries no objective entry for kind \"" +
                   o.kind + "\" — pin an explicit entity_type in the pack");
    return it->second;
}

uint16_t squadron_entity(const Palette& pal, const PackSquadron& s,
                         const ClassTable& ct) {
    if (s.entity_type != 0) {
        if (ct.lookup(static_cast<uint16_t>(s.entity_type)) == nullptr)
            init_error("squadron \"" + s.name +
                       "\" pins entity_type " + std::to_string(s.entity_type) +
                       " which the class table does not carry");
        return static_cast<uint16_t>(s.entity_type);
    }
    if (pal.squadron == 0)
        init_error("the class table carries no (UNIT, AIR, SQUADRON) entry — "
                   "pin an explicit entity_type in the pack");
    return pal.squadron;
}

uint16_t battalion_entity(const Palette& pal, const PackBattalion& b,
                          const ClassTable& ct) {
    if (b.entity_type != 0) {
        if (ct.lookup(static_cast<uint16_t>(b.entity_type)) == nullptr)
            init_error("battalion pins entity_type " +
                       std::to_string(b.entity_type) +
                       " which the class table does not carry");
        return static_cast<uint16_t>(b.entity_type);
    }
    const int want = battalion_subtype_from_kind(b.kind);
    const auto it = pal.battalion.find(want);
    if (it == pal.battalion.end())
        init_error("the class table carries no battalion entry for kind \"" +
                   b.kind + "\" — pin an explicit entity_type in the pack");
    return it->second;
}

// ── the camp map ─────────────────────────────────────────────────────────

// Nearest-objective ownership fill over the pack's grid (2 bits per
// cell, row-major, low bits first — the .cmp's own packing). Ties go to
// the wire-order-first objective. Deterministic; inert for the runtime
// (nothing in f4-world/f4-campaign reads the camp map) but it makes the
// save honest for any consumer that does.
void fill_camp_map(CampaignHeader& h, const ScenarioPack& pack,
                   const std::vector<uint8_t>& owners) {
    const int sx = pack.theater.size_x;
    const int sy = pack.theater.size_y;
    h.camp_map_size = static_cast<int16_t>((sx * sy * 2 + 7) / 8);
    h.camp_map.assign(static_cast<std::size_t>(h.camp_map_size), 0);
    auto set_cell = [&](int x, int y, uint8_t owner) {
        const long long idx = static_cast<long long>(y) * sx + x;
        const std::size_t byte = static_cast<std::size_t>(idx * 2 / 8);
        const int shift = static_cast<int>(idx * 2 % 8);
        h.camp_map[byte] =
            static_cast<uint8_t>(h.camp_map[byte] |
                                 static_cast<uint8_t>((owner & 0x03) << shift));
    };
    for (int y = 0; y < sy; ++y) {
        for (int x = 0; x < sx; ++x) {
            std::size_t best = 0;
            long long best_d = -1;
            for (std::size_t i = 0; i < pack.objectives.size(); ++i) {
                const long long dx = pack.objectives[i].x - x;
                const long long dy = pack.objectives[i].y - y;
                const long long d = dx * dx + dy * dy;
                if (best_d < 0 || d < best_d) {
                    best_d = d;
                    best = i;
                }
            }
            set_cell(x, y, owners[best]);
        }
    }
}

} // namespace

// ── build ────────────────────────────────────────────────────────────────

// The default tasking profile: korea's own (the stock save's live
// team priorities, both sides run the same row). A zero mission-
// priority row would mean the team never requests anything — a
// generated war must be able to fight. Packs gain explicit overrides
// only when a scenario needs a doctrinal quirk.
const uint8_t kDefaultMissionPriority[41] = {
    0, 10, 10, 10, 0, 0, 10, 40, 20, 40, 10, 20, 30, 40, 80, 60,
    40, 40, 60, 100, 100, 100, 100, 100, 80, 80, 100, 100, 100, 60,
    30, 100, 60, 60, 60, 20, 60, 10, 100, 0, 0};
const uint8_t kDefaultObjtypePriority[20] = {
    0, 40, 10, 100, 0, 0, 60, 20, 0, 40, 40, 20, 0, 20, 0, 0, 0, 20, 0, 20};

InitBuildResult CampaignInitializer::build(const ScenarioPack& pack,
                                           const ClassTable& ct,
                                           InitStats* stats) {
    if (!ct.loaded()) init_error("a loaded class table is required");

    const Palette pal = resolve_palette(ct);

    // Named-slot lookup (the pack's team vocabulary).
    std::map<int, const PackTeam*> team_by_slot;
    for (const auto& t : pack.teams) team_by_slot[t.slot] = &t;

    // Objective name → index (validated unique at the parse).
    std::map<std::string, std::size_t> obj_index;
    for (std::size_t i = 0; i < pack.objectives.size(); ++i)
        obj_index.emplace(pack.objectives[i].name, i);

    InitBuildResult out;
    SynthesizedCampaign& w = out.world;
    w.camp_version = pack.camp_version;

    // ── VU_ID allocation (deterministic counters) ────────────────────
    // Objectives take 1..N (creator 0, the stock convention), units
    // continue N+1.., teams 3000+slot, the teams' ATM managers
    // 3100+slot. last_index_num lands one past the highest issued.
    uint32_t next_id = 1;
    std::vector<uint32_t> objective_ids(pack.objectives.size(), 0);
    for (std::size_t i = 0; i < pack.objectives.size(); ++i)
        objective_ids[i] = next_id++;

    // Per-team tallies for the team blocks (filled while placing units).
    std::map<int, std::vector<uint32_t>> team_airbase_ids; // slot → bases
    std::map<int, int> team_squadron_aircraft;             // slot → roster sum
    int team_airbase_count[8] = {0, 0, 0, 0, 0, 0, 0, 0};

    // ── objectives ───────────────────────────────────────────────────
    w.objectives.count = static_cast<int16_t>(pack.objectives.size());
    w.objectives.objectives.resize(pack.objectives.size());
    std::vector<uint8_t> objective_owners(pack.objectives.size(), 0);
    for (std::size_t i = 0; i < pack.objectives.size(); ++i) {
        const PackObjective& po = pack.objectives[i];
        ObjectiveRecord& r = w.objectives.objectives[i];
        const uint16_t entity = objective_entity(pal, po, ct);
        r.type = static_cast<int16_t>(entity);   // the sentinel IS the
        r.entity_type = entity;                  // entity_type in clean files
        r.id_creator = 0;
        r.id_num = objective_ids[i];
        r.x = static_cast<int16_t>(po.x);
        r.y = static_cast<int16_t>(po.y);
        r.z = 0.0f;
        r.owner = static_cast<uint8_t>(po.owner);
        r.camp_id = static_cast<int16_t>(objective_ids[i]);
        r.supply = static_cast<uint8_t>(std::clamp(po.supply, 0, 100));
        r.fuel = static_cast<uint8_t>(std::clamp(po.fuel, 0, 100));
        r.priority = static_cast<uint8_t>(std::clamp(po.priority, 0, 100));
        r.first_owner = static_cast<uint8_t>(po.owner);   // the wire's own
                                                          // save-start rule
        objective_owners[i] = r.owner;
        // A base-kind objective held by a named team counts toward
        // that team's TeamStatusType.airbases bookkeeping.
        if (po.owner != 0) {
            const int kind = objective_type_from_kind(po.kind);
            if (kind == 1 || kind == 2 || kind == 3)
                team_airbase_count[po.owner & 7]++;
        }
    }
    // Links — each pair encoded in BOTH directions, the real net's shape.
    // Default cost profile: a road-and-tracked link with air overflight;
    // Naval/Rail 250 (unsupported). is_road() reads Wheeled ∈ (0, 250).
    const uint8_t kLinkCosts[MOVEMENT_TYPES] = {0, 20, 30, 60, 10, 10, 250, 250};
    for (const auto& l : pack.links) {
        ObjectiveLink ab;
        std::copy(std::begin(kLinkCosts), std::end(kLinkCosts), ab.costs);
        ab.neighbor_creator = 0;
        ab.neighbor_num = objective_ids[static_cast<std::size_t>(l.b)];
        ObjectiveLink ba;
        std::copy(std::begin(kLinkCosts), std::end(kLinkCosts), ba.costs);
        ba.neighbor_creator = 0;
        ba.neighbor_num = objective_ids[static_cast<std::size_t>(l.a)];
        {
            ObjectiveRecord& r =
                w.objectives.objectives[static_cast<std::size_t>(l.a)];
            r.link_data.push_back(ab);
            r.links = static_cast<uint8_t>(r.link_data.size());
        }
        {
            ObjectiveRecord& r =
                w.objectives.objectives[static_cast<std::size_t>(l.b)];
            r.link_data.push_back(ba);
            r.links = static_cast<uint8_t>(r.link_data.size());
        }
    }

    // ── units: squadrons then battalions (wire order = pack order) ──
    w.units.units.resize(pack.squadrons.size() + pack.battalions.size());
    std::size_t unit_cursor = 0;
    struct SquadronLoc {
        uint32_t vu = 0;
        std::size_t record = 0;
        int team = 0;
        uint32_t airbase = 0;
    };
    std::vector<SquadronLoc> squadron_locs;
    squadron_locs.reserve(pack.squadrons.size());

    for (const auto& ps : pack.squadrons) {
        UnitRecord& u = w.units.units[unit_cursor];
        const PackObjective& base = pack.objectives[obj_index.at(ps.base)];
        const uint16_t entity = squadron_entity(pal, ps, ct);
        u.type = static_cast<int16_t>(entity);
        u.unit_class = UnitClass::Squadron;
        u.entity_type = entity;
        u.id_creator = 0;
        u.id_num = next_id++;
        u.x = static_cast<int16_t>(base.x);
        u.y = static_cast<int16_t>(base.y);
        u.owner = static_cast<uint8_t>(ps.team);
        u.camp_id = static_cast<int16_t>(u.id_num);
        u.roster = pack_roster(ps.size);
        team_squadron_aircraft[ps.team] += ps.size;
        team_airbase_ids[ps.team].push_back(
            objective_ids[obj_index.at(ps.base)]);
        SquadronLoc loc;
        loc.vu = u.id_num;
        loc.record = unit_cursor;
        loc.team = ps.team;
        squadron_locs.push_back(loc);
        ++unit_cursor;
    }
    for (const auto& pb : pack.battalions) {
        UnitRecord& u = w.units.units[unit_cursor];
        const PackObjective& site = pack.objectives[obj_index.at(pb.at)];
        const uint16_t entity = battalion_entity(pal, pb, ct);
        u.type = static_cast<int16_t>(entity);
        u.unit_class = UnitClass::Battalion;
        u.entity_type = entity;
        u.id_creator = 0;
        u.id_num = next_id++;
        u.x = static_cast<int16_t>(site.x);
        u.y = static_cast<int16_t>(site.y);
        u.owner = static_cast<uint8_t>(pb.team);
        u.camp_id = static_cast<int16_t>(u.id_num);
        u.roster = pack_roster(pb.groups * 3);
        u.subclass.supply = 100;
        u.subclass.morale = 100;
        u.subclass.last_move = pack.date.current_time;
        ++unit_cursor;
    }
    w.units.count = static_cast<int16_t>(w.units.units.size());

    // ── teams (all 8 slots, the stock save's own shape) ─────────────
    w.teams.count = 8;
    w.teams.teams.resize(8);
    for (int slot = 0; slot < 8; ++slot) {
        TeamRecord& t = w.teams.teams[static_cast<std::size_t>(slot)];
        t.who = static_cast<uint8_t>(slot);
        t.cteam = static_cast<uint8_t>(slot);
        t.id_creator = 0;
        t.id_num = (team_by_slot.find(slot) != team_by_slot.end())
                       ? 3000u + static_cast<uint32_t>(slot)
                       : 0u;
        // Stance rows: declared relations set BOTH directions; the
        // undeclared default is Neutral (3).
        t.stance.assign(8, static_cast<int16_t>(PackRelation::Neutral));
        t.member.assign(8, 0);
        t.member[static_cast<std::size_t>(slot)] = 1;
        t.name = "XX";                       // the stock placeholder
    }
    for (const auto& rd : pack.relations) {
        const auto v = static_cast<int16_t>(rd.stance);
        w.teams.teams[static_cast<std::size_t>(rd.a)]
            .stance[static_cast<std::size_t>(rd.b)] = v;
        w.teams.teams[static_cast<std::size_t>(rd.b)]
            .stance[static_cast<std::size_t>(rd.a)] = v;
    }
    for (const auto& pt : pack.teams) {
        TeamRecord& t = w.teams.teams[static_cast<std::size_t>(pt.slot)];
        clamp_name(t.name = pt.name, 20);
        clamp_name(t.motto = pt.motto, 200);
        t.team_color = static_cast<uint8_t>(pt.colour & 0xFF);
        t.air_experience = static_cast<uint8_t>(std::clamp(pt.air_experience, 0, 100));
        t.ground_experience =
            static_cast<uint8_t>(std::clamp(pt.ground_experience, 0, 100));
        t.naval_experience =
            static_cast<uint8_t>(std::clamp(pt.naval_experience, 0, 100));
        t.air_defense_experience =
            static_cast<uint8_t>(std::clamp(pt.air_defense_experience, 0, 100));
        t.replacements_avail =
            static_cast<uint16_t>(std::clamp(pt.replacements, 0, 65535));
        t.supply_avail = static_cast<uint16_t>(std::clamp(pt.supply, 0, 65535));
        t.fuel_avail = static_cast<uint16_t>(std::clamp(pt.fuel, 0, 65535));
        // The team's own books: aircraft = reserve pool + rostered
        // squadrons; airbases = owned airbase-kind objectives.
        const int rostered =
            team_squadron_aircraft.find(pt.slot) == team_squadron_aircraft.end()
                ? 0
                : team_squadron_aircraft.at(pt.slot);
        t.current_aircraft = static_cast<uint16_t>(
            std::clamp(pt.reserve_aircraft + rostered, 0, 65535));
        t.start_aircraft = t.current_aircraft;
        t.current_supply = t.supply_avail;
        t.start_supply = t.supply_avail;
        t.current_fuel = t.fuel_avail;
        t.start_fuel = t.fuel_avail;
        t.current_supply_level = 100;
        t.current_fuel_level = 100;
        t.start_supply_level = 100;
        t.start_fuel_level = 100;
        t.current_airbases = static_cast<uint16_t>(
            std::clamp(team_airbase_count[pt.slot & 7], 0, 65535));
        t.start_airbases = t.current_airbases;
        t.initiative = 50;
        // The tasking profile: the stock save's own live priorities
        // (the arrays are the encoder's zero-filled-on-short form).
        t.mission_priority.assign(std::begin(kDefaultMissionPriority),
                                  std::end(kDefaultMissionPriority));
        t.objtype_priority.assign(std::begin(kDefaultObjtypePriority),
                                  std::end(kDefaultObjtypePriority));
        // The team's ATM: one airbase row per squadron home base (32
        // zero schedule blocks — every takeoff slot free), no pending
        // requests (a fresh war's ATO starts empty).
        ATMRecord& atm = t.atm;
        atm.id_creator = 0;
        atm.id_num = 3100u + static_cast<uint32_t>(pt.slot);
        atm.owner = static_cast<uint8_t>(pt.slot);
        std::set<uint32_t> seen;
        for (const uint32_t ab : team_airbase_ids[pt.slot]) {
            if (seen.insert(ab).second) {
                ATMAirbaseRecord rec;
                rec.id_creator = 0;
                rec.id_num = ab;
                atm.airbases.push_back(rec);
            }
        }
    }

    // ── squadron tails (pilots, airbase anchor, specialty) ──────────
    for (std::size_t i = 0; i < pack.squadrons.size(); ++i) {
        const PackSquadron& ps = pack.squadrons[i];
        UnitRecord& u = w.units.units[squadron_locs[i].record];
        u.subclass.specialty =
            static_cast<uint8_t>(squadron_specialty_from_word(ps.specialty));
        u.subclass.fuel = 3000;              // the stock saves' own default
        u.subclass.airbase_id_creator = 0;
        u.subclass.airbase_id_num = objective_ids[obj_index.at(ps.base)];
        u.subclass.pilots.resize(static_cast<std::size_t>(ps.size));
        for (int p = 0; p < ps.size; ++p) {
            PilotRecord& pr =
                u.subclass.pilots[static_cast<std::size_t>(p)];
            pr.pilot_id = static_cast<int16_t>(p);
            pr.skill = static_cast<uint8_t>(ps.skill & 0x0F);
            pr.status = 0;                   // available
        }
    }

    // ── the .cmp header ──────────────────────────────────────────────
    CampaignHeader& h = w.header;
    h.current_time = pack.date.current_time;
    h.te_start_time = pack.date.current_time;
    h.te_time_limit = pack.date.current_time;
    h.te_victory_points = 0;
    h.te_type = 0;
    h.te_number_teams = static_cast<int32_t>(pack.teams.size());
    h.te_number_aircraft.assign(8, 0);
    h.te_number_f16s.assign(8, 0);
    h.te_team_pts.assign(8, 0);
    for (const auto& pt : pack.teams)
        h.te_number_aircraft[static_cast<std::size_t>(pt.slot)] =
            pt.reserve_aircraft;
    h.te_team = pack.player_team;
    h.te_flags = 0;
    h.teams.assign(8, TeamEntry{});
    for (auto& e : h.teams) e.name = "XX";   // the stock placeholder rows
    for (const auto& pt : pack.teams) {
        TeamEntry& e = h.teams[static_cast<std::size_t>(pt.slot)];
        clamp_name(e.name = pt.name, 20);
        clamp_name(e.motto = pt.motto, 200);
        e.colour = static_cast<uint8_t>(pt.colour & 0xFF);
        e.flags = 0;
    }
    h.last_major_event = 0;
    // The maintenance anchors start at the opening clock: one full
    // cadence period of grace before the first resupply/repair/
    // reinforcement cycle fires.
    h.last_resupply = pack.date.current_time;
    h.last_repair = pack.date.current_time;
    h.last_reinforcement = pack.date.current_time;
    h.time_stamp = 1;
    h.group = 0;
    h.ground_ratio = 1;
    h.air_ratio = 1;
    h.air_defense_ratio = 1;
    h.naval_ratio = 1;
    h.brief = 0;
    h.theater_size_x = static_cast<int16_t>(pack.theater.size_x);
    h.theater_size_y = static_cast<int16_t>(pack.theater.size_y);
    h.current_day = static_cast<uint8_t>(std::clamp(pack.date.day, 0, 255));
    h.active_teams = static_cast<uint8_t>(pack.teams.size());
    h.day_zero = 0;
    h.endgame_result = 0;
    h.situation = 0;
    h.enemy_air_exp = 0;
    h.enemy_ad_exp = 0;
    h.bullseye_name = static_cast<uint8_t>(pack.bullseye_name & 0xFF);
    h.bullseye_x = static_cast<int16_t>(pack.bullseye_x);
    h.bullseye_y = static_cast<int16_t>(pack.bullseye_y);
    clamp_name(h.theater_name = pack.theater.name, 40);
    clamp_name(h.scenario =
                   pack.theater.scenario.empty() ? pack.name
                                                 : pack.theater.scenario,
               40);
    clamp_name(h.save_file =
                   pack.theater.save_file.empty() ? pack.name
                                                  : pack.theater.save_file,
               40);
    clamp_name(h.ui_name =
                   pack.theater.ui_name.empty() ? pack.name
                                                : pack.theater.ui_name,
               40);
    // Player squadron: the first squadron of the player team (0 = none).
    h.player_squadron_creator = 0;
    h.player_squadron_num = 0;
    if (pack.player_team != 0) {
        for (const auto& loc : squadron_locs) {
            if (loc.team == pack.player_team) {
                h.player_squadron_num = loc.vu;
                break;
            }
        }
    }
    fill_camp_map(h, pack, objective_owners);
    h.last_index_num = static_cast<int16_t>(std::min<int32_t>(
        static_cast<int32_t>(next_id),
        std::numeric_limits<int16_t>::max()));
    h.num_avail_squadrons = static_cast<int16_t>(pack.squadrons.size());
    // The UI preload list: one entry per squadron, the base's grid
    // position in nominal sim feet (≈11.25 km per grid — the list is
    // UI-preload telemetry the runtime never reads).
    h.squadrons.clear();
    h.squadrons.reserve(pack.squadrons.size());
    constexpr float kGridFeet = 36909.0f;
    for (std::size_t i = 0; i < pack.squadrons.size(); ++i) {
        const PackSquadron& ps = pack.squadrons[i];
        const PackObjective& base = pack.objectives[obj_index.at(ps.base)];
        SquadronUIInfo s;
        s.x = static_cast<float>(base.x) * kGridFeet;
        s.y = static_cast<float>(base.y) * kGridFeet;
        s.id_creator = 0;
        s.id_num = squadron_locs[i].vu;
        s.d_index = 0;
        s.name_id = 0;
        s.airbase_icon = 0;
        s.squadron_patch = 0;
        s.specialty =
            static_cast<uint8_t>(squadron_specialty_from_word(ps.specialty));
        s.current_strength = static_cast<uint8_t>(
            std::clamp(ps.size, 0, 255));
        s.country = static_cast<uint8_t>(ps.team);
        clamp_name(s.airbase_name =
                       ps.name.empty() ? ps.base : ps.name, 40);
        h.squadrons.push_back(std::move(s));
    }
    h.tempo = static_cast<uint8_t>(pack.tempo & 0xFF);
    h.creator_ip = 0;
    h.creation_time = pack.date.current_time;
    h.creation_rand = pack.seed;             // THE seed's home in the wire

    // ── assemble the archive ─────────────────────────────────────────
    const int v = w.camp_version;
    const std::string stem = archive_stem(pack.name);
    CamWriter writer;
    writer.add(stem + ".cmp", encode_cmp(w.header, v));
    writer.add(stem + ".obj", encode_obj(w.objectives, v));
    writer.add(stem + ".obd", encode_obd(w.deltas));   // the canonical
                                                       // zero-delta form
    writer.add(stem + ".uni", encode_uni(w.units, v));
    writer.add(stem + ".tea", encode_tea(w.teams, v));
    // The passthrough-typed sub-files ride EMPTY (see the header's
    // documented fresh-save conventions — nothing in this repo's
    // reader or runtime reads them).
    writer.add(stem + ".evt", {});
    writer.add(stem + ".plt", {});
    writer.add(stem + ".pst", {});
    writer.add(stem + ".wth", {});
    const std::string ver = std::to_string(v);
    writer.add(stem + ".ver",
               std::vector<uint8_t>(ver.begin(), ver.end()));
    out.cam_bytes = writer.build();

    if (stats != nullptr) {
        stats->objectives = static_cast<int>(pack.objectives.size());
        stats->squadrons = static_cast<int>(pack.squadrons.size());
        stats->battalions = static_cast<int>(pack.battalions.size());
        stats->named_teams = static_cast<int>(pack.teams.size());
        stats->last_index_num = next_id;
        stats->cam_bytes = out.cam_bytes.size();
    }
    return out;
}

std::string CampaignInitializer::to_world_json(const InitBuildResult& built,
                                               const ClassTable& ct,
                                               const ScenarioPack& pack) {
    // The EXISTING reader drives: build bytes → CamArchive (in memory) →
    // to_world_json — the same code path cam2json runs for a file.
    CamArchive archive;
    archive.load_from_memory(built.cam_bytes);
    WorldJsonOptions opts;
    opts.theater = pack.theater.id;
    opts.terrain_file = pack.theater.terrain_file;
    opts.class_table = &ct;
    // Qualified call: the free emitter (world_json.hpp), not this class's
    // own static member of the same name.
    return f4::world_convert::to_world_json(archive, opts);
}

} // namespace f4::world_convert
