// f4-world-convert/src/save_writeback.cpp
//
// The runtime-mutated save path — see save_writeback.hpp for the design
// and the diff-then-overwrite rationale.

#include <f4/world_convert/save_writeback.hpp>

#include <f4/world_convert/cam_archive.hpp>
#include <f4/world_convert/cam_writer.hpp>
#include <f4/world_convert/campaign_json.hpp>
#include <f4/world_convert/cmp_encoder.hpp>
#include <f4/world_convert/objective_decoder.hpp>
#include <f4/world_convert/objective_encoder.hpp>
#include <f4/world_convert/unit_decoder.hpp>
#include <f4/world_convert/unit_encoder.hpp>
#include <f4/json/reader.hpp>

#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace f4::world_convert {

namespace {

using f4::json::Reader;

// ---------------------------------------------------------------------------
// Per-document field collection (the lossy projection's owned fields).
// ---------------------------------------------------------------------------

struct ObjFields {
    bool seen = false;                    // the item existed in the doc
    std::vector<uint8_t> fstatus;
    std::optional<uint8_t> owner;
};

struct UnitFields {
    bool seen = false;
    std::optional<int> aa_kills, ag_kills, as_kills, an_kills;
    std::optional<int> missions_flown, total_losses;
    std::optional<int> x, y, dest_x, dest_y;
    std::optional<uint32_t> roster;
    std::optional<int> losses, supply, morale, fatigue, heading;
    std::optional<int32_t> last_move, last_combat;
};

struct DocFields {
    // Campaign-level owned fields (absent = not carried by the doc).
    std::optional<int32_t> current_time;
    std::optional<int32_t> last_resupply;
    std::optional<int32_t> last_repair;
    std::optional<int32_t> last_reinforcement;
    std::vector<int32_t> te_number_aircraft;
    bool te_number_aircraft_present = false;

    std::map<uint32_t, ObjFields> objectives;
    std::map<uint32_t, UnitFields> units;
};

// Walk one objective item (the Reader sits at the item's '{'). Returns
// the record's VU_ID.num (captured from the "id_num" key — JSON objects
// are unordered, so the id may arrive before or after the fields) and
// the owned fields.
std::pair<uint32_t, ObjFields> collect_objective(Reader& r) {
    ObjFields f;
    uint32_t id = 0;
    r.skip_ws();
    r.expect('{');
    if (r.consume('}')) return {id, f};
    for (;;) {
        const std::string k = r.read_string();
        r.expect(':');
        if (k == "id_num") {
            id = static_cast<uint32_t>(r.read_int());
            f.seen = true;
        } else if (k == "fstatus") {
            r.skip_ws();
            r.expect('[');
            if (r.consume(']')) {
                // empty bitmap — treated as "not carried"
            } else
                for (;;) {
                    f.fstatus.push_back(static_cast<uint8_t>(r.read_int()));
                    if (r.consume(']')) break;
                    r.expect(',');
                }
        } else if (k == "owner") {
            f.owner = static_cast<uint8_t>(r.read_int());
        } else {
            r.skip_value();
        }
        if (r.consume('}')) break;
        r.expect(',');
    }
    return {id, f};
}

// Walk one unit item (the Reader sits at the item's '{').
std::pair<uint32_t, UnitFields> collect_unit(Reader& r) {
    UnitFields f;
    uint32_t id = 0;
    r.skip_ws();
    r.expect('{');
    if (r.consume('}')) return {id, f};
    for (;;) {
        const std::string k = r.read_string();
        r.expect(':');
        if (k == "id_num") {
            id = static_cast<uint32_t>(r.read_int());
            f.seen = true;
        }
        // --- squadron counters (C1) ---
        else if (k == "aa_kills") f.aa_kills = static_cast<int>(r.read_int());
        else if (k == "ag_kills") f.ag_kills = static_cast<int>(r.read_int());
        else if (k == "as_kills") f.as_kills = static_cast<int>(r.read_int());
        else if (k == "an_kills") f.an_kills = static_cast<int>(r.read_int());
        else if (k == "missions_flown")
            f.missions_flown = static_cast<int>(r.read_int());
        else if (k == "total_losses")
            f.total_losses = static_cast<int>(r.read_int());
        // --- battalion movement/state (G1) ---
        else if (k == "x") f.x = static_cast<int>(r.read_int());
        else if (k == "y") f.y = static_cast<int>(r.read_int());
        else if (k == "dest_x") f.dest_x = static_cast<int>(r.read_int());
        else if (k == "dest_y") f.dest_y = static_cast<int>(r.read_int());
        else if (k == "roster")
            f.roster = static_cast<uint32_t>(r.read_int());
        else if (k == "losses") f.losses = static_cast<int>(r.read_int());
        else if (k == "supply") f.supply = static_cast<int>(r.read_int());
        else if (k == "morale") f.morale = static_cast<int>(r.read_int());
        else if (k == "fatigue") f.fatigue = static_cast<int>(r.read_int());
        else if (k == "heading") f.heading = static_cast<int>(r.read_int());
        else if (k == "last_move")
            f.last_move = static_cast<int32_t>(r.read_int());
        else if (k == "last_combat")
            f.last_combat = static_cast<int32_t>(r.read_int());
        else {
            r.skip_value();
        }
        if (r.consume('}')) break;
        r.expect(',');
    }
    return {id, f};
}

// Walk a whole document (original or mutated — both sides of the diff
// speak the world-JSON vocabulary).
DocFields collect_doc(const std::string& json) {
    DocFields doc;
    Reader r(json);
    r.skip_ws();
    r.expect('{');
    if (r.consume('}')) return doc;

    for (;;) {
        const std::string key = r.read_string();
        r.expect(':');
        if (key == "campaign") {
            // The campaign object carries the owned scalar fields AND the
            // teams array (nested — skip_value handles it).
            r.skip_ws();
            r.expect('{');
            if (!r.consume('}')) {
                for (;;) {
                    const std::string ck = r.read_string();
                    r.expect(':');
                    if (ck == "current_time") {
                        doc.current_time =
                            static_cast<int32_t>(r.read_int());
                    } else if (ck == "last_resupply") {
                        doc.last_resupply =
                            static_cast<int32_t>(r.read_int());
                    } else if (ck == "last_repair") {
                        doc.last_repair =
                            static_cast<int32_t>(r.read_int());
                    } else if (ck == "last_reinforcement") {
                        doc.last_reinforcement =
                            static_cast<int32_t>(r.read_int());
                    } else if (ck == "te_number_aircraft") {
                        r.skip_ws();
                        r.expect('[');
                        doc.te_number_aircraft_present = true;
                        if (r.consume(']')) {
                            // empty array — the pools are not carried
                        } else
                            for (;;) {
                                doc.te_number_aircraft.push_back(
                                    static_cast<int32_t>(r.read_int()));
                                if (r.consume(']')) break;
                                r.expect(',');
                            }
                    } else {
                        r.skip_value();
                    }
                    if (r.consume('}')) break;
                    r.expect(',');
                }
            }
        } else if (key == "objectives") {
            r.skip_ws();
            r.expect('{');
            if (!r.consume('}')) {
                for (;;) {
                    const std::string ok = r.read_string();
                    r.expect(':');
                    if (ok == "items") {
                        r.skip_ws();
                        r.expect('[');
                        if (!r.consume(']')) {
                            for (;;) {
                                auto [id, f] = collect_objective(r);
                                if (f.seen) doc.objectives[id] = std::move(f);
                                if (r.consume(']')) break;
                                r.expect(',');
                            }
                        }
                    } else {
                        r.skip_value();
                    }
                    if (r.consume('}')) break;
                    r.expect(',');
                }
            }
        } else if (key == "units") {
            r.skip_ws();
            r.expect('{');
            if (!r.consume('}')) {
                for (;;) {
                    const std::string uk = r.read_string();
                    r.expect(':');
                    if (uk == "items") {
                        r.skip_ws();
                        r.expect('[');
                        if (!r.consume(']')) {
                            for (;;) {
                                auto [id, f] = collect_unit(r);
                                if (f.seen) doc.units[id] = std::move(f);
                                if (r.consume(']')) break;
                                r.expect(',');
                            }
                        }
                    } else {
                        r.skip_value();
                    }
                    if (r.consume('}')) break;
                    r.expect(',');
                }
            }
        } else {
            r.skip_value();
        }
        if (r.consume('}')) break;
        r.expect(',');
    }
    return doc;
}

// ---------------------------------------------------------------------------
// Mutation application over the decode structs.
// ---------------------------------------------------------------------------

void apply_mutation(ObjectiveRecord& rec, const ObjectiveSaveMutation& m) {
    if (!m.fstatus.empty()) rec.fstatus = m.fstatus;
    if (m.owner.has_value()) rec.owner = *m.owner;
}

void apply_mutation(UnitRecord& rec, const UnitSaveMutation& m) {
    // CampBaseClass-level movement fields (every unit class carries x/y).
    if (m.x.has_value()) rec.x = static_cast<int16_t>(*m.x);
    if (m.y.has_value()) rec.y = static_cast<int16_t>(*m.y);
    if (m.dest_x.has_value()) rec.dest_x = static_cast<int16_t>(*m.dest_x);
    if (m.dest_y.has_value()) rec.dest_y = static_cast<int16_t>(*m.dest_y);
    if (m.roster.has_value()) rec.roster = *m.roster;
    if (m.losses.has_value()) rec.losses = static_cast<uint8_t>(*m.losses);

    // Subclass tails: write only the fields the record's own class
    // carries (a mutation produced from a diff is class-correct by
    // construction; the guard keeps hand-built mutations honest).
    switch (rec.unit_class) {
        case UnitClass::Battalion:
        case UnitClass::TaskForce:
            if (m.supply.has_value())
                rec.subclass.supply = static_cast<uint8_t>(*m.supply);
            if (m.morale.has_value())
                rec.subclass.morale = static_cast<uint8_t>(*m.morale);
            if (m.fatigue.has_value())
                rec.subclass.fatigue = static_cast<uint8_t>(*m.fatigue);
            if (m.heading.has_value())
                rec.subclass.heading = static_cast<uint8_t>(*m.heading);
            if (m.last_move.has_value())
                rec.subclass.last_move = *m.last_move;
            if (m.last_combat.has_value())
                rec.subclass.last_combat = *m.last_combat;
            break;
        case UnitClass::Squadron:
            if (m.aa_kills.has_value())
                rec.subclass.aa_kills = static_cast<int16_t>(*m.aa_kills);
            if (m.ag_kills.has_value())
                rec.subclass.ag_kills = static_cast<int16_t>(*m.ag_kills);
            if (m.as_kills.has_value())
                rec.subclass.as_kills = static_cast<int16_t>(*m.as_kills);
            if (m.an_kills.has_value())
                rec.subclass.an_kills = static_cast<int16_t>(*m.an_kills);
            if (m.missions_flown.has_value())
                rec.subclass.missions_flown =
                    static_cast<int16_t>(*m.missions_flown);
            if (m.total_losses.has_value())
                rec.subclass.total_losses =
                    static_cast<uint8_t>(*m.total_losses);
            break;
        default:
            break;  // Brigade/Flight/Package: nothing in the owned surface
    }
}

// The CampaignSaver's mutation application (mirrored from
// campaign_saver.cpp's anonymous namespace — the same sentinel semantics;
// the codebase prefers duplication over premature sharing).
constexpr int32_t UNSET = INT32_MIN;

void apply_campaign_mutations(CampaignHeader& h,
                              const CampaignMutations& mut) {
    if (mut.current_time != UNSET) h.current_time = mut.current_time;
    if (mut.last_resupply != UNSET) h.last_resupply = mut.last_resupply;
    if (mut.last_repair != UNSET) h.last_repair = mut.last_repair;
    if (mut.last_reinforcement != UNSET) {
        h.last_reinforcement = mut.last_reinforcement;
    }
    if (!mut.te_number_aircraft.empty()) {
        if (h.te_number_aircraft.size() < 8) {
            h.te_number_aircraft.resize(8, 0);
        }
        const std::size_t n =
            std::min<std::size_t>(mut.te_number_aircraft.size(), 8);
        for (std::size_t i = 0; i < n; ++i) {
            h.te_number_aircraft[i] = mut.te_number_aircraft[i];
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------
// The diff: original vs mutated projection, owned fields only.
// ---------------------------------------------------------------------------
DerivedSaveMutations derive_save_mutations(const std::string& original_json,
                                           const std::string& mutated_json) {
    const DocFields orig = collect_doc(original_json);
    const DocFields mutd = collect_doc(mutated_json);

    DerivedSaveMutations out;

    // --- campaign-level fields -------------------------------------------
    // A field the mutated doc doesn't carry = not owned by this state
    // (a synthetic WorldState) — keep the original.
    if (mutd.current_time.has_value() && orig.current_time.has_value() &&
        *mutd.current_time != *orig.current_time) {
        out.campaign.current_time = *mutd.current_time;
    }
    if (mutd.last_resupply.has_value() && orig.last_resupply.has_value() &&
        *mutd.last_resupply != *orig.last_resupply) {
        out.campaign.last_resupply = *mutd.last_resupply;
    }
    if (mutd.last_repair.has_value() && orig.last_repair.has_value() &&
        *mutd.last_repair != *orig.last_repair) {
        out.campaign.last_repair = *mutd.last_repair;
    }
    if (mutd.last_reinforcement.has_value() &&
        orig.last_reinforcement.has_value() &&
        *mutd.last_reinforcement != *orig.last_reinforcement) {
        out.campaign.last_reinforcement = *mutd.last_reinforcement;
    }
    // The pools diff only when the mutated doc carries a non-empty set —
    // an empty projection array means "not carried", not "zeroed".
    if (mutd.te_number_aircraft_present &&
        !mutd.te_number_aircraft.empty() &&
        mutd.te_number_aircraft != orig.te_number_aircraft) {
        out.campaign.te_number_aircraft = mutd.te_number_aircraft;
    }

    // --- objectives (fstatus, owner) --------------------------------------
    for (const auto& [id, mf] : mutd.objectives) {
        const auto it = orig.objectives.find(id);
        if (it == orig.objectives.end()) continue;  // not in the original

        ObjectiveSaveMutation m;
        m.id_num = id;
        bool changed = false;
        if (!mf.fstatus.empty() && mf.fstatus != it->second.fstatus) {
            m.fstatus = mf.fstatus;
            changed = true;
        }
        if (mf.owner.has_value() && it->second.owner.has_value() &&
            *mf.owner != *it->second.owner) {
            m.owner = *mf.owner;
            changed = true;
        }
        if (changed) out.objectives.push_back(std::move(m));
    }

    // --- units (counters + battalion state) --------------------------------
    for (const auto& [id, mf] : mutd.units) {
        const auto it = orig.units.find(id);
        if (it == orig.units.end()) continue;

        UnitSaveMutation m;
        m.id_num = id;
        bool changed = false;
        auto diff_opt = [&changed](const auto& mv, const auto& ov,
                                   auto& dst) {
            if (mv.has_value() && ov.has_value() && *mv != *ov) {
                dst = *mv;
                changed = true;
            }
        };
        diff_opt(mf.aa_kills, it->second.aa_kills, m.aa_kills);
        diff_opt(mf.ag_kills, it->second.ag_kills, m.ag_kills);
        diff_opt(mf.as_kills, it->second.as_kills, m.as_kills);
        diff_opt(mf.an_kills, it->second.an_kills, m.an_kills);
        diff_opt(mf.missions_flown, it->second.missions_flown,
                 m.missions_flown);
        diff_opt(mf.total_losses, it->second.total_losses, m.total_losses);
        diff_opt(mf.x, it->second.x, m.x);
        diff_opt(mf.y, it->second.y, m.y);
        diff_opt(mf.dest_x, it->second.dest_x, m.dest_x);
        diff_opt(mf.dest_y, it->second.dest_y, m.dest_y);
        diff_opt(mf.roster, it->second.roster, m.roster);
        diff_opt(mf.losses, it->second.losses, m.losses);
        diff_opt(mf.supply, it->second.supply, m.supply);
        diff_opt(mf.morale, it->second.morale, m.morale);
        diff_opt(mf.fatigue, it->second.fatigue, m.fatigue);
        diff_opt(mf.heading, it->second.heading, m.heading);
        diff_opt(mf.last_move, it->second.last_move, m.last_move);
        diff_opt(mf.last_combat, it->second.last_combat, m.last_combat);
        if (changed) out.units.push_back(std::move(m));
    }

    return out;
}

// ---------------------------------------------------------------------------
// The assembly: original doc + mutations → .cam bytes.
// ---------------------------------------------------------------------------
std::vector<uint8_t> build_campaign_with_mutations(
    const std::string& original_json, const DerivedSaveMutations& mut) {
    const int camp_version = read_world_json_version(original_json);

    // The .cmp: full-fidelity re-encode from the ORIGINAL's campaign
    // block with the derived mutations applied.
    CampaignHeader h = from_world_json_campaign(original_json, camp_version);
    apply_campaign_mutations(h, mut.campaign);
    auto cmp_bytes = encode_cmp(h, camp_version);

    // The original sub-files (subfiles_b64 → .cam → CamArchive).
    auto passthrough = cam_from_world_json(original_json);
    auto tmp =
        std::filesystem::temp_directory_path() / "f4_save_writeback_tmp.cam";
    {
        std::ofstream f(tmp, std::ios::binary);
        f.write(reinterpret_cast<const char*>(passthrough.data()),
                static_cast<std::streamsize>(passthrough.size()));
    }
    CamArchive cam;
    cam.load(tmp);
    std::error_code ec;
    std::filesystem::remove(tmp, ec);

    // The .obj: decode → apply → re-encode (struct-faithful — the records
    // the diff didn't touch keep the original decode's exact values, and
    // so does every field of the records it did).
    std::vector<uint8_t> obj_bytes;
    if (const SubFile* obj = cam.find("obj"); obj != nullptr) {
        DecodedObjectives dec =
            decode_obj(obj->data.data(), obj->data.size(), camp_version);
        for (const auto& m : mut.objectives) {
            bool found = false;
            for (auto& rec : dec.objectives) {
                if (rec.id_num == m.id_num) {
                    apply_mutation(rec, m);
                    found = true;
                    break;
                }
            }
            if (!found) {
                throw std::runtime_error(
                    "build_campaign_with_mutations: objective mutation id " +
                    std::to_string(m.id_num) + " matches no decoded record");
            }
        }
        obj_bytes = encode_obj(dec, camp_version);
    } else if (!mut.objectives.empty()) {
        throw std::runtime_error(
            "build_campaign_with_mutations: objective mutations given but "
            "the original carries no .obj sub-file");
    }

    // The .uni: same pattern (unit subclass dispatch uses the decoder's
    // trial-and-error without a class table — the save path doesn't have
    // one; the encoders are dispatch-free: they re-encode the subclass
    // the decoder identified).
    std::vector<uint8_t> uni_bytes;
    if (const SubFile* uni = cam.find("uni"); uni != nullptr) {
        UnitDecodeOptions opts;
        opts.camp_version = camp_version;
        DecodedUnits dec =
            decode_uni(uni->data.data(), uni->data.size(), opts);
        for (const auto& m : mut.units) {
            bool found = false;
            for (auto& rec : dec.units) {
                if (rec.id_num == m.id_num) {
                    apply_mutation(rec, m);
                    found = true;
                    break;
                }
            }
            if (!found) {
                throw std::runtime_error(
                    "build_campaign_with_mutations: unit mutation id " +
                    std::to_string(m.id_num) + " matches no decoded record");
            }
        }
        uni_bytes = encode_uni(dec, camp_version);
    } else if (!mut.units.empty()) {
        throw std::runtime_error(
            "build_campaign_with_mutations: unit mutations given but the "
            "original carries no .uni sub-file");
    }

    // Assemble: the re-encoded .cmp + .obj + .uni, everything else
    // verbatim from the original.
    CamWriter w;
    for (const auto& sf : cam.subfiles()) {
        if (sf.ext() == "cmp") {
            w.add(sf.name, cmp_bytes);
        } else if (sf.ext() == "obj" && !obj_bytes.empty()) {
            w.add(sf.name, obj_bytes);
        } else if (sf.ext() == "uni" && !uni_bytes.empty()) {
            w.add(sf.name, uni_bytes);
        } else {
            w.add(sf.name, sf.data);
        }
    }
    return w.build();
}

std::size_t save_campaign_with_mutations(
    const std::string& original_json,
    const std::filesystem::path& output_cam,
    const DerivedSaveMutations& mut) {
    auto bytes = build_campaign_with_mutations(original_json, mut);

    std::error_code mk_ec;
    std::filesystem::create_directories(output_cam.parent_path(), mk_ec);
    std::ofstream f(output_cam, std::ios::binary);
    if (!f) {
        throw std::runtime_error("save_campaign_with_mutations: cannot "
                                 "write " +
                                 output_cam.string());
    }
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    if (!f) {
        throw std::runtime_error("save_campaign_with_mutations: short "
                                 "write to " +
                                 output_cam.string());
    }
    return bytes.size();
}

} // namespace f4::world_convert
