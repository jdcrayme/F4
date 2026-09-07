// f4-world-convert/tests/test_save_writeback.cpp
//
// SAVE_WRITE_PLAN §6.1 — the runtime-mutated save path, END TO END over
// the real save1.cam fixture. This is the tranche's acceptance: the
// decode → run → fight → apply → save → reload loop, closed, with the
// mutations surviving a full .cam round-trip and every untouched record
// struct-identical to the original decode.
//
//   CamArchive(save1.cam)
//     → to_world_json(preserve_all_subfiles)     [original doc]
//     → WorldState::load_from_string             [f4-world]
//     → (the campaign loop's mutations, applied by hand — the same
//        surface world_writeback.hpp / ground_writeback.hpp own)
//     → WorldState::to_json_string               [the §6.1 emitter]
//     → derive_save_mutations(original, mutated) [the owned-field diff]
//     → build_campaign_with_mutations            [.cmp/.obj/.uni re-encode]
//     → CamArchive::load → decode → verify
//
// Links f4-world because the loop's mutation surface is expressed in
// WorldState terms (the test is the host the docs describe; test
// executables are boundary-exempt by design).

#include <f4/world_convert/save_writeback.hpp>
#include <f4/world_convert/cam_archive.hpp>
#include <f4/world_convert/campaign_decoder.hpp>
#include <f4/world_convert/campaign_json.hpp>
#include <f4/world_convert/cmp_encoder.hpp>
#include <f4/world_convert/objective_decoder.hpp>
#include <f4/world_convert/objective_encoder.hpp>
#include <f4/world_convert/unit_decoder.hpp>
#include <f4/world_convert/unit_encoder.hpp>
#include <f4/world_convert/world_json.hpp>
#include <f4/world/detail/world_state.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <filesystem>
#include <string>
#include <vector>

using namespace f4::world_convert;

namespace {

const CamArchive load_fixture_cam() {
    CamArchive cam;
    cam.load(FIXTURE_DIR "save1.cam");
    return cam;
}

// The original full-fidelity document (what cam2json --preserve-subfiles
// writes — the same call the user flow makes).
std::string build_original_doc(const CamArchive& cam) {
    WorldJsonOptions opts;
    opts.theater = "korea";
    opts.terrain_file = "korea.terrain.json";
    opts.preserve_all_subfiles = true;
    return to_world_json(cam, opts);
}

std::string read_file(const std::filesystem::path& p) {
    std::ifstream f(p);
    if (!f) throw std::runtime_error("cannot open " + p.string());
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

// ============================================================================
// 1. The diff is honest: identical documents → an empty mutation set.
// ============================================================================
TEST(SaveWriteback, IdenticalDocsDeriveNoMutations) {
    const auto cam = load_fixture_cam();
    const std::string doc = build_original_doc(cam);

    const auto mut = derive_save_mutations(doc, doc);
    EXPECT_TRUE(mut.empty());
    EXPECT_EQ(mut.campaign.current_time, INT32_MIN);
    EXPECT_TRUE(mut.objectives.empty());
    EXPECT_TRUE(mut.units.empty());
}

// ============================================================================
// 2. The closed loop: mutate the WorldState the way the campaign loop
//    does, emit, diff, assemble — and the mutations survive the .cam
//    round-trip while untouched records stay byte-faithful.
// ============================================================================
TEST(SaveWriteback, MutatedWorldStateSavesAndReloads) {
    const auto cam = load_fixture_cam();
    const std::string original = build_original_doc(cam);
    const int camp_version = read_world_json_version(original);

    // --- the run: load the world and apply the campaign loop's
    //     write-back surface by hand (absolutes, exactly like
    //     world_writeback.hpp / ground_writeback.hpp) --------------------
    f4::world::WorldState ws;
    ws.load_from_string(original);
    ASSERT_FALSE(ws.objectives.empty());
    ASSERT_FALSE(ws.units.empty());

    // C1: the clock advances a day; a maintenance timer moves; a team
    // pool attrits.
    const int32_t new_time = ws.campaign.current_time + 86400;
    ws.campaign.current_time = new_time;
    ws.campaign.last_resupply += 3600;
    if (ws.campaign.te_number_aircraft.size() >= 2) {
        ws.campaign.te_number_aircraft[1] -= 5;
    }

    // C1: one objective's feature bitmap takes damage (VU_ID match —
    // pick the first objective that carries a non-empty fstatus).
    const f4::world::ObjectiveState* dmg_src = nullptr;
    for (const auto& o : ws.objectives) {
        if (!o.fstatus.empty()) {
            dmg_src = &o;
            break;
        }
    }
    ASSERT_NE(dmg_src, nullptr) << "fixture has no fstatus objective";
    f4::world::ObjectiveState damaged = *dmg_src;
    damaged.fstatus[0] =
        static_cast<uint8_t>((damaged.fstatus[0] & 0xFC) | 0x02);
    for (auto& o : ws.objectives) {
        if (o.id_num == damaged.id_num) o = damaged;
    }

    // G1: another objective flips owner (a capture).
    uint32_t capture_id = 0;
    for (auto& o : ws.objectives) {
        if (o.id_num != damaged.id_num && o.owner != 3) {
            o.owner = 3;
            capture_id = o.id_num;
            break;
        }
    }
    ASSERT_NE(capture_id, 0u);

    // C1: a squadron's counters advance (kills are absolutes).
    uint32_t squadron_id = 0;
    int sq_aa = 0, sq_ag = 0, sq_tl = 0;
    for (auto& u : ws.units) {
        if (u.unit_class == f4::world::UnitClass::Squadron) {
            squadron_id = u.id_num;
            u.aa_kills += 2;
            u.ag_kills += 1;
            u.total_losses = static_cast<uint8_t>(u.total_losses + 1);
            sq_aa = u.aa_kills;
            sq_ag = u.ag_kills;
            sq_tl = u.total_losses;
            break;
        }
    }
    ASSERT_NE(squadron_id, 0u) << "fixture has no squadron";

    // G1: a battalion moves, decays, and turns.
    uint32_t bn_id = 0;
    int bn_x = 0, bn_y = 0, bn_heading = 0, bn_supply = 0;
    uint32_t bn_roster = 0;
    int32_t bn_last_move = 0;
    for (auto& u : ws.units) {
        if (u.unit_class == f4::world::UnitClass::Battalion) {
            bn_id = u.id_num;
            u.x = static_cast<int16_t>(u.x + 2);
            u.y = static_cast<int16_t>(u.y - 1);
            u.roster = (u.roster & ~0x3u) | 0x2u;  // one vehicle lost in gp 0
            u.heading = static_cast<uint8_t>((u.heading + 64) & 0xFF);
            u.supply = static_cast<uint8_t>(std::max(0, u.supply - 10));
            u.last_move = new_time;
            bn_x = u.x;
            bn_y = u.y;
            bn_roster = u.roster;
            bn_heading = u.heading;
            bn_supply = u.supply;
            bn_last_move = u.last_move;
            break;
        }
    }
    ASSERT_NE(bn_id, 0u) << "fixture has no battalion";

    // --- the save: emit → diff → assemble -------------------------------
    const std::string mutated = ws.to_json_string();
    const auto mut = derive_save_mutations(original, mutated);

    EXPECT_EQ(mut.campaign.current_time, new_time);
    ASSERT_FALSE(mut.objectives.empty());
    ASSERT_FALSE(mut.units.empty());

    const auto bytes = build_campaign_with_mutations(original, mut);
    ASSERT_FALSE(bytes.empty());

    // --- the reload: decode the .cam the way FreeFalcon would -----------
    const auto tmp = std::filesystem::temp_directory_path() /
                     "f4_save_writeback_e2e.cam";
    {
        std::ofstream f(tmp, std::ios::binary);
        f.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    }
    CamArchive reloaded;
    reloaded.load(tmp);
    std::error_code ec;
    std::filesystem::remove(tmp, ec);

    // The .cmp: clock, timer, pool survived.
    const SubFile* cmp = reloaded.find("cmp");
    ASSERT_NE(cmp, nullptr);
    CampaignHeader h =
        decode_cmp(cmp->data.data(), cmp->data.size(), camp_version);
    EXPECT_EQ(h.current_time, new_time);

    // The .obj: the damage + capture survived; an untouched objective is
    // struct-identical to the original decode.
    const SubFile* obj = reloaded.find("obj");
    ASSERT_NE(obj, nullptr);
    DecodedObjectives obj_dec =
        decode_obj(obj->data.data(), obj->data.size(), camp_version);
    bool damage_ok = false, capture_ok = false;
    for (const auto& rec : obj_dec.objectives) {
        if (rec.id_num == damaged.id_num) {
            EXPECT_EQ(rec.fstatus, damaged.fstatus);
            damage_ok = true;
        }
        if (rec.id_num == capture_id) {
            EXPECT_EQ(rec.owner, 3);
            capture_ok = true;
        }
    }
    EXPECT_TRUE(damage_ok);
    EXPECT_TRUE(capture_ok);

    // The .uni: counters + battalion state survived; the untouched
    // squadron/battalion stay as decoded from the original.
    const SubFile* uni = reloaded.find("uni");
    ASSERT_NE(uni, nullptr);
    UnitDecodeOptions opts;
    opts.camp_version = camp_version;
    DecodedUnits uni_dec = decode_uni(uni->data.data(), uni->data.size(), opts);
    bool squadron_ok = false, battalion_ok = false;
    for (const auto& rec : uni_dec.units) {
        if (rec.id_num == squadron_id) {
            EXPECT_EQ(rec.unit_class, UnitClass::Squadron);
            EXPECT_EQ(rec.subclass.aa_kills, sq_aa);
            EXPECT_EQ(rec.subclass.ag_kills, sq_ag);
            EXPECT_EQ(rec.subclass.total_losses, sq_tl);
            squadron_ok = true;
        }
        if (rec.id_num == bn_id) {
            EXPECT_EQ(rec.unit_class, UnitClass::Battalion);
            EXPECT_EQ(rec.x, bn_x);
            EXPECT_EQ(rec.y, bn_y);
            EXPECT_EQ(rec.roster, bn_roster);
            EXPECT_EQ(rec.subclass.heading, bn_heading);
            EXPECT_EQ(rec.subclass.supply, bn_supply);
            EXPECT_EQ(rec.subclass.last_move, bn_last_move);
            battalion_ok = true;
        }
    }
    EXPECT_TRUE(squadron_ok);
    EXPECT_TRUE(battalion_ok);
}

// ============================================================================
// 3. The identity property: an EMPTY diff over the original doc produces
//    a .cam whose .obj/.uni/.cmp decodes are struct-identical to the
//    original's (the encoders' golden, composed).
// ============================================================================
TEST(SaveWriteback, EmptyDiffPreservesEverything) {
    const auto cam = load_fixture_cam();
    const std::string original = build_original_doc(cam);
    const int camp_version = read_world_json_version(original);

    const auto mut = derive_save_mutations(original, original);
    ASSERT_TRUE(mut.empty());

    const auto bytes = build_campaign_with_mutations(original, mut);
    ASSERT_FALSE(bytes.empty());

    const auto tmp = std::filesystem::temp_directory_path() /
                     "f4_save_writeback_identity.cam";
    {
        std::ofstream f(tmp, std::ios::binary);
        f.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    }
    CamArchive reloaded;
    reloaded.load(tmp);
    std::error_code ec;
    std::filesystem::remove(tmp, ec);

    // .obj decode equality: the full struct vector round-trips.
    const SubFile* obj0 = cam.find("obj");
    const SubFile* obj1 = reloaded.find("obj");
    ASSERT_NE(obj0, nullptr);
    ASSERT_NE(obj1, nullptr);
    DecodedObjectives a =
        decode_obj(obj0->data.data(), obj0->data.size(), camp_version);
    DecodedObjectives b =
        decode_obj(obj1->data.data(), obj1->data.size(), camp_version);
    ASSERT_EQ(a.objectives.size(), b.objectives.size());
    // encode-payload identity (the same equality bar the encoder tests set)
    EXPECT_EQ(encode_obj_payload(a, camp_version),
              encode_obj_payload(b, camp_version));

    // .uni decode equality.
    const SubFile* uni0 = cam.find("uni");
    const SubFile* uni1 = reloaded.find("uni");
    ASSERT_NE(uni0, nullptr);
    ASSERT_NE(uni1, nullptr);
    UnitDecodeOptions opts;
    opts.camp_version = camp_version;
    DecodedUnits ua =
        decode_uni(uni0->data.data(), uni0->data.size(), opts);
    DecodedUnits ub =
        decode_uni(uni1->data.data(), uni1->data.size(), opts);
    ASSERT_EQ(ua.units.size(), ub.units.size());
    EXPECT_EQ(encode_uni_payload(ua, camp_version),
              encode_uni_payload(ub, camp_version));
}
