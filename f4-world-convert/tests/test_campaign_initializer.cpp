// test_campaign_initializer.cpp — the CAMP-INIT-1 gates.
//
// The acceptance contract (CAMP_HOST_PLAN.md §8):
//   * byte-identity BY CONSTRUCTION — two builds of one pack produce
//     byte-identical .cam archives (the seed is an input, not entropy);
//   * a different seed moves the bytes (the .cmp's CreationRand);
//   * a generated save DECODES IN THE EXISTING READER — every
//     structured sub-file decodes cursor-clean with the synthesized
//     fields intact;
//   * the world JSON (the existing reader's own projection) loads
//     through f4-world's WorldState with the war intact.

#include <gtest/gtest.h>
#include <f4/world_convert/cam_archive.hpp>
#include <f4/world_convert/campaign_decoder.hpp>
#include <f4/world_convert/campaign_initializer.hpp>
#include <f4/world_convert/class_table.hpp>
#include <f4/world_convert/objective_decoder.hpp>
#include <f4/world_convert/scenario_pack.hpp>
#include <f4/world_convert/team_decoder.hpp>
#include <f4/world_convert/unit_decoder.hpp>
#include <f4/world_convert/world_json.hpp>
#include <f4/world/detail/world_state.hpp>

#include <string>
#include <vector>

using namespace f4::world_convert;

namespace {

const char* kPack = R"({
  "pack": 1, "name": "Test War", "camp_version": 71, "seed": 8675309,
  "theater": { "name": "TEST WAR", "size_x": 64, "size_y": 64 },
  "date": { "current_time": 32400000, "day": 1 },
  "bullseye": { "x": 32, "y": 34, "name": 1 },
  "player_team": 1,
  "teams": [
    { "slot": 1, "name": "BLUE", "colour": 2, "air_experience": 80,
      "reserve_aircraft": 6, "replacements": 10 },
    { "slot": 6, "name": "RED", "colour": 5, "air_experience": 60,
      "reserve_aircraft": 6, "replacements": 10 }
  ],
  "relations": [ { "a": 1, "b": 6, "stance": "war" } ],
  "objectives": [
    { "name": "Blue Base",  "kind": "airbase",  "x": 16, "y": 12, "owner": 1, "priority": 60 },
    { "name": "Blue Town",  "kind": "city",     "x": 20, "y": 24, "owner": 1, "priority": 40 },
    { "name": "Red Base",   "kind": "airbase",  "x": 48, "y": 52, "owner": 6, "priority": 60 },
    { "name": "Red Depot",  "kind": "depot",    "x": 44, "y": 40, "owner": 6, "priority": 25 },
    { "name": "Crossroads", "kind": "city",     "x": 32, "y": 32, "owner": 0, "priority": 10 }
  ],
  "links": [ [0, 1], [2, 3], [1, 4], [4, 3] ],
  "squadrons": [
    { "team": 1, "base": "Blue Base", "name": "1st FW", "size": 12, "skill": 5, "specialty": "AA", "entity_type": 473 },
    { "team": 6, "base": "Red Base",  "name": "6th FW", "size": 10, "skill": 4, "specialty": "AG", "entity_type": 473 }
  ],
  "battalions": [
    { "team": 1, "at": "Blue Town", "kind": "armor",    "groups": 4 },
    { "team": 6, "at": "Red Depot", "kind": "infantry", "groups": 3 }
  ]
})";

struct Built {
    ScenarioPack pack;
    InitBuildResult result;
};

Built build_default() {
    Built b;
    b.pack = ScenarioPack::parse(kPack);
    ClassTable ct;
    ct.load_json(FIXTURE_DIR "falcon4.ct.json");
    b.result = CampaignInitializer::build(b.pack, ct);
    return b;
}

// kunsan's own tasking profile — the pack default puts a live save's
// priorities in every named team's .tea block (a zero mission_priority
// row would mean the team never requests anything).
const char* kProfilePack = R"({
  "pack": 1, "name": "Profile War", "seed": 1,
  "theater": { "size_x": 64, "size_y": 64 },
  "teams": [
    { "slot": 1, "name": "BLUE" },
    { "slot": 6, "name": "RED" }
  ],
  "relations": [ { "a": 1, "b": 6, "stance": "war" } ],
  "objectives": [
    { "name": "Blue Base", "kind": "airbase", "x": 16, "y": 12, "owner": 1 },
    { "name": "Red Base",  "kind": "airbase", "x": 48, "y": 52, "owner": 6 }
  ],
  "squadrons": [
    { "team": 1, "base": "Blue Base", "size": 8 },
    { "team": 6, "base": "Red Base", "size": 8 }
  ]
})";

} // namespace

// ── byte-identity by construction ───────────────────────────────────────

TEST(CampaignInitializer, TwoBuildsOfOnePackAreByteIdentical) {
    const Built a = build_default();
    const Built b = build_default();
    ASSERT_EQ(a.result.cam_bytes.size(), b.result.cam_bytes.size());
    EXPECT_TRUE(a.result.cam_bytes == b.result.cam_bytes)
        << "same pack + seed must produce the same archive bytes";
    // The structs behind the bytes agree too.
    EXPECT_EQ(a.result.world.teams.count, b.result.world.teams.count);
    EXPECT_EQ(a.result.world.units.count, b.result.world.units.count);
}

TEST(CampaignInitializer, TheSeedRidesCreationRandAndMovesTheBytes) {
    Built a = build_default();
    EXPECT_EQ(a.result.world.header.creation_rand, 8675309u)
        << "the pack's seed lands in the .cmp's CreationRand";
    EXPECT_EQ(a.result.world.header.creation_time, 32400000)
        << "CreationTime is the pack's opening clock (deterministic)";

    ScenarioPack other = a.pack;
    other.seed = 42;
    ClassTable ct;
    ct.load_json(FIXTURE_DIR "falcon4.ct.json");
    const InitBuildResult b = CampaignInitializer::build(other, ct);
    EXPECT_FALSE(a.result.cam_bytes == b.cam_bytes)
        << "a different seed must move the bytes";
    // The .ver/agnostic container layout keeps every sub-file count;
    // only the .cmp payload's compressed form differs.
    CamArchive ca;
    ca.load_from_memory(b.cam_bytes);
    const SubFile* cb = ca.find("cmp");
    ASSERT_NE(cb, nullptr);
    const CampaignHeader hb =
        decode_cmp(cb->data.data(), cb->data.size(), 71);
    EXPECT_EQ(hb.creation_rand, 42u);
    EXPECT_EQ(hb.current_time, a.result.world.header.current_time);
}

// ── the generated save decodes in the existing reader ───────────────────

TEST(CampaignInitializer, ContainerDecodesAndCarriesTheStockManifest) {
    const Built b = build_default();
    CamArchive cam;
    cam.load_from_memory(b.result.cam_bytes);
    ASSERT_EQ(cam.subfiles().size(), 10u)
        << "cmp/obj/obd/uni/tea/evt/plt/pst/wth/ver — the stock order";
    EXPECT_NE(cam.find("cmp"), nullptr);
    EXPECT_NE(cam.find("obj"), nullptr);
    EXPECT_NE(cam.find("obd"), nullptr);
    EXPECT_NE(cam.find("uni"), nullptr);
    EXPECT_NE(cam.find("tea"), nullptr);
    EXPECT_NE(cam.find("ver"), nullptr);
    // The .ver is the text decimal the reader expects.
    const SubFile* ver = cam.find("ver");
    ASSERT_NE(ver, nullptr);
    const std::string ver_text(ver->data.begin(), ver->data.end());
    EXPECT_EQ(read_version(ver->data.data(), ver->data.size()), 71);
    EXPECT_EQ(ver_text, "71");
    // The passthrough-typed sub-files ride empty (the documented
    // fresh-save convention).
    for (const char* ext : {"evt", "plt", "pst", "wth"}) {
        const SubFile* sf = cam.find(ext);
        ASSERT_NE(sf, nullptr) << ext << " must exist";
        EXPECT_TRUE(sf->data.empty()) << ext << " rides empty";
    }
}

TEST(CampaignInitializer, CmpDecodesCursorCleanWithThePackFields) {
    const Built b = build_default();
    CamArchive cam;
    cam.load_from_memory(b.result.cam_bytes);
    const SubFile* cmp = cam.find("cmp");
    ASSERT_NE(cmp, nullptr);
    const CampaignHeader h = decode_cmp(cmp->data.data(), cmp->data.size(), 71);
    EXPECT_EQ(h.bytes_consumed, h.decompressed_size)
        << "the cursor must land exactly at the end";
    EXPECT_EQ(h.current_time, 32400000);
    EXPECT_EQ(h.te_number_teams, 2);
    EXPECT_EQ(h.te_number_aircraft[1], 6);
    EXPECT_EQ(h.te_number_aircraft[6], 6);
    EXPECT_EQ(h.te_team, 1);
    EXPECT_EQ(h.active_teams, 2);
    EXPECT_EQ(h.theater_size_x, 64);
    EXPECT_EQ(h.bullseye_x, 32);
    ASSERT_EQ(h.teams.size(), 8u);
    EXPECT_EQ(h.teams[1].name, "BLUE");
    EXPECT_EQ(h.teams[6].name, "RED");
    EXPECT_EQ(h.teams[0].name, "XX") << "unnamed slots keep the placeholder";
    // The maintenance anchors start at the opening clock (one cadence
    // period of grace).
    EXPECT_EQ(h.last_resupply, 32400000);
    EXPECT_EQ(h.last_repair, 32400000);
    EXPECT_EQ(h.last_reinforcement, 32400000);
    // The squadron preload list mirrors the OOB.
    EXPECT_EQ(h.num_avail_squadrons, 2);
    ASSERT_EQ(h.squadrons.size(), 2u);
    EXPECT_EQ(h.squadrons[0].id_num, 6u) << "units follow the objectives' ids";
    EXPECT_EQ(h.squadrons[0].current_strength, 12);
    EXPECT_EQ(h.squadrons[0].country, 1);
    EXPECT_EQ(h.squadrons[1].current_strength, 10);
    // The camp map covers the pack's grid (2 bits per cell).
    EXPECT_EQ(h.camp_map_size, 64 * 64 * 2 / 8);
}

TEST(CampaignInitializer, ObjDecodesCursorCleanWithOwnersAndLinks) {
    const Built b = build_default();
    CamArchive cam;
    cam.load_from_memory(b.result.cam_bytes);
    const SubFile* obj = cam.find("obj");
    ASSERT_NE(obj, nullptr);
    const DecodedObjectives d = decode_obj(obj->data.data(), obj->data.size(), 71);
    EXPECT_EQ(d.bytes_consumed, d.inner_size);
    ASSERT_EQ(d.count, 5);
    ASSERT_EQ(d.objectives.size(), 5u);
    EXPECT_EQ(d.objectives[0].id_num, 1u);
    EXPECT_EQ(d.objectives[0].x, 16);
    EXPECT_EQ(d.objectives[0].y, 12);
    EXPECT_EQ(d.objectives[0].owner, 1);
    EXPECT_EQ(d.objectives[0].first_owner, 1) << "save-start semantics";
    EXPECT_EQ(d.objectives[0].priority, 60);
    EXPECT_EQ(d.objectives[2].owner, 6);
    EXPECT_EQ(d.objectives[4].owner, 0) << "the neutral crossroads";
    // The class table resolved every named kind to a real entity type,
    // and the table reads it back as the right ObjectiveType.
    ClassTable ct;
    ct.load_json(FIXTURE_DIR "falcon4.ct.json");
    EXPECT_EQ(ct.objective_type_for(
                  static_cast<uint16_t>(d.objectives[0].entity_type)),
              1) << "the airbase";
    EXPECT_EQ(ct.objective_type_for(
                  static_cast<uint16_t>(d.objectives[3].entity_type)),
              10) << "the depot";
    // Links are bidirectional: Blue Base ↔ Blue Town, Red Base ↔ Red
    // Depot, Blue Town ↔ Crossroads, Crossroads ↔ Red Depot.
    EXPECT_EQ(d.objectives[0].links, 1);
    EXPECT_EQ(d.objectives[0].link_data[0].neighbor_num, 2u);
    EXPECT_EQ(d.objectives[1].links, 2);
    EXPECT_EQ(d.objectives[1].link_data[0].neighbor_num, 1u);
    EXPECT_EQ(d.objectives[1].link_data[1].neighbor_num, 5u);
    EXPECT_TRUE(d.objectives[0].link_data[0].is_road());
}

TEST(CampaignInitializer, TeaDecodesCursorCleanWithWarRowsAndAtmBases) {
    const Built b = build_default();
    CamArchive cam;
    cam.load_from_memory(b.result.cam_bytes);
    const SubFile* tea = cam.find("tea");
    ASSERT_NE(tea, nullptr);
    const DecodedTeams d = decode_tea(tea->data.data(), tea->data.size(), 71);
    EXPECT_EQ(d.bytes_consumed, tea->data.size());
    ASSERT_EQ(d.count, 8) << "all 8 slots, the stock shape";
    ASSERT_EQ(d.teams.size(), 8u);
    EXPECT_EQ(d.teams[1].name, "BLUE");
    EXPECT_EQ(d.teams[1].who, 1);
    EXPECT_EQ(d.teams[1].id_num, 3001u);
    EXPECT_EQ(d.teams[6].name, "RED");
    // The war rows: declared in BOTH directions, undeclared = Neutral.
    EXPECT_EQ(d.teams[1].stance[6], 5);
    EXPECT_EQ(d.teams[6].stance[1], 5);
    EXPECT_EQ(d.teams[1].stance[0], 3);
    EXPECT_EQ(d.teams[2].stance[1], 3);
    // Force levels.
    EXPECT_EQ(d.teams[1].replacements_avail, 10);
    EXPECT_EQ(d.teams[1].air_experience, 80);
    // The ATM's airbase rows: one per distinct squadron home base.
    ASSERT_EQ(d.teams[1].atm.airbases.size(), 1u);
    EXPECT_EQ(d.teams[1].atm.airbases[0].id_num, 1u) << "Blue Base's VU";
    ASSERT_EQ(d.teams[6].atm.airbases.size(), 1u);
    EXPECT_EQ(d.teams[6].atm.airbases[0].id_num, 3u) << "Red Base's VU";
    EXPECT_TRUE(d.teams[1].atm.requests.empty())
        << "a fresh war's ATO starts empty";
    // The team's books count the reserve pool + rostered squadrons.
    EXPECT_EQ(d.teams[1].current_aircraft, 6 + 12);
    EXPECT_EQ(d.teams[6].current_aircraft, 6 + 10);
    // GTM/NTM ride the encoder's zero-manager default: the encoder
    // wrote 15 zero bytes for each, and the decoder captures them
    // verbatim (struct equality in both directions).
    ASSERT_EQ(d.teams[1].gtm_raw.size(), 15u);
    for (const uint8_t byte : d.teams[1].gtm_raw) EXPECT_EQ(byte, 0);
    ASSERT_EQ(d.teams[1].ntm_raw.size(), 15u);
    for (const uint8_t byte : d.teams[1].ntm_raw) EXPECT_EQ(byte, 0);
}

TEST(CampaignInitializer, UniDecodesCursorCleanWithRostersAndAnchors) {
    const Built b = build_default();
    CamArchive cam;
    cam.load_from_memory(b.result.cam_bytes);
    const SubFile* uni = cam.find("uni");
    ASSERT_NE(uni, nullptr);
    ClassTable ct;
    ct.load_json(FIXTURE_DIR "falcon4.ct.json");
    UnitDecodeOptions opts;
    opts.camp_version = 71;
    opts.class_table = &ct;
    const DecodedUnits d = decode_uni(uni->data.data(), uni->data.size(), opts);
    EXPECT_EQ(d.bytes_consumed, d.inner_size)
        << "every synthesized record must decode";
    ASSERT_EQ(d.count, 4);
    ASSERT_EQ(d.units.size(), 4u);
    // Wire order: pack squadrons, then pack battalions.
    EXPECT_EQ(d.units[0].unit_class, UnitClass::Squadron);
    EXPECT_EQ(d.units[0].id_num, 6u);
    EXPECT_EQ(d.units[0].owner, 1);
    EXPECT_EQ(d.units[0].entity_type, 473) << "the pack's pinned class";
    // The roster bitfield decodes to exactly the pack's size.
    int aircraft = 0;
    for (int g = 0; g < 16; ++g)
        aircraft += static_cast<int>((d.units[0].roster >> (2 * g)) & 0x3);
    EXPECT_EQ(aircraft, 12);
    EXPECT_EQ(d.units[0].subclass.airbase_id_num, 1u)
        << "anchored to Blue Base (non-zero — the primary resolution path)";
    ASSERT_EQ(d.units[0].subclass.pilots.size(), 48u)
        << "the wire's fixed 48-pilot roster";
    EXPECT_EQ(d.units[0].subclass.pilots[0].skill, 5);
    EXPECT_EQ(d.units[0].subclass.pilots[0].status, 0) << "available";
    // The unfilled slots are the encoder's zero-fill (their aircraft
    // never count — snapshot_squadron_force reads the ROSTER bitfield,
    // not the pilot list).
    EXPECT_EQ(d.units[0].subclass.pilots[12].skill, 0);
    EXPECT_EQ(d.units[0].subclass.specialty, 1) << "AA";
    EXPECT_EQ(d.units[1].owner, 6);
    EXPECT_EQ(d.units[1].subclass.specialty, 2) << "AG";
    EXPECT_EQ(d.units[2].unit_class, UnitClass::Battalion);
    EXPECT_EQ(d.units[2].subclass.supply, 100);
    EXPECT_EQ(d.units[2].subclass.morale, 100);
    int vehicles = 0;
    for (int g = 0; g < 16; ++g)
        vehicles += static_cast<int>((d.units[2].roster >> (2 * g)) & 0x3);
    EXPECT_EQ(vehicles, 12) << "4 groups × 3";
}

TEST(CampaignInitializer, ObdIsTheCanonicalZeroDeltaForm) {
    const Built b = build_default();
    CamArchive cam;
    cam.load_from_memory(b.result.cam_bytes);
    const SubFile* obd = cam.find("obd");
    ASSERT_NE(obd, nullptr);
    const DecodedObjectiveDeltas d =
        decode_obd(obd->data.data(), obd->data.size(), 71);
    EXPECT_EQ(d.count, 0);
    EXPECT_TRUE(d.deltas.empty());
    EXPECT_EQ(obd->data.size(), 10u)
        << "[i32 6][i16 0][i32 0] — exactly what save1.cam carries";
}

TEST(CampaignInitializer, CommittedPacksAllBuildAndDecodeCursorClean) {
    ClassTable ct;
    ct.load_json(FIXTURE_DIR "falcon4.ct.json");
    for (const char* pack_name : {"small", "medium", "large", "twinwars"}) {
        const ScenarioPack pack =
            ScenarioPack::load(std::string(FIXTURE_DIR "packs/") + pack_name +
                               ".pack.json");
        const InitBuildResult built = CampaignInitializer::build(pack, ct);
        EXPECT_FALSE(built.cam_bytes.empty());

        CamArchive cam;
        cam.load_from_memory(built.cam_bytes);
        const SubFile* cmp = cam.find("cmp");
        const SubFile* obj = cam.find("obj");
        const SubFile* uni = cam.find("uni");
        const SubFile* tea = cam.find("tea");
        ASSERT_NE(cmp, nullptr);
        ASSERT_NE(obj, nullptr);
        ASSERT_NE(uni, nullptr);
        ASSERT_NE(tea, nullptr);
        const CampaignHeader h =
            decode_cmp(cmp->data.data(), cmp->data.size(), pack.camp_version);
        EXPECT_EQ(h.bytes_consumed, h.decompressed_size) << pack_name;
        const DecodedObjectives o =
            decode_obj(obj->data.data(), obj->data.size(), pack.camp_version);
        EXPECT_EQ(o.bytes_consumed, o.inner_size) << pack_name;
        EXPECT_EQ(o.count, static_cast<int16_t>(pack.objectives.size()))
            << pack_name;
        const DecodedTeams t =
            decode_tea(tea->data.data(), tea->data.size(), pack.camp_version);
        EXPECT_EQ(t.bytes_consumed, tea->data.size()) << pack_name;
        UnitDecodeOptions uo;
        uo.camp_version = pack.camp_version;
        uo.class_table = &ct;
        const DecodedUnits u =
            decode_uni(uni->data.data(), uni->data.size(), uo);
        EXPECT_EQ(u.bytes_consumed, u.inner_size) << pack_name;
        EXPECT_EQ(u.count,
                  static_cast<int16_t>(pack.squadrons.size() +
                                       pack.battalions.size()))
            << pack_name;
        // The two-war bed: the first War pair in slot order is (1,6) —
        // the G1 limitation's own test bed lives at the engine level,
        // but the pack's stance rows are pinned here.
        if (std::string(pack_name) == "twinwars") {
            EXPECT_EQ(t.teams[1].stance[6], 5);
            EXPECT_EQ(t.teams[1].stance[2], 1);
            EXPECT_EQ(t.teams[3].stance[7], 5);
            EXPECT_EQ(t.teams[2].stance[1], 1);
        }
    }
}

// ── the world JSON is the existing reader's own projection ─────────────

TEST(CampaignInitializer, WorldJsonLoadsThroughTheRuntimeWorldState) {
    const Built b = build_default();
    ClassTable ct;
    ct.load_json(FIXTURE_DIR "falcon4.ct.json");
    const std::string world_json =
        CampaignInitializer::to_world_json(b.result, ct, b.pack);

    f4::world::WorldState ws;
    ws.load_from_string(world_json);
    EXPECT_EQ(ws.theater, "korea");
    EXPECT_EQ(ws.version, 71);
    EXPECT_EQ(ws.campaign.current_time, 32400000);
    EXPECT_EQ(ws.campaign.te_number_aircraft[1], 6);
    ASSERT_EQ(ws.teams.size(), 8u);
    EXPECT_EQ(ws.teams[1].name, "BLUE");
    EXPECT_EQ(ws.teams[1].stance[6], 5) << "the war row survives the loop";
    ASSERT_GE(ws.objectives.size(), 5u);
    // The airbase is recognized (objective_type resolved by the class
    // table the emitter was handed) and carries its owner + VU id.
    bool saw_airbase = false;
    for (const auto& o : ws.objectives) {
        if (o.id_num == 1) {
            saw_airbase = true;
            EXPECT_EQ(o.objective_type, 1);
            EXPECT_EQ(o.owner, 1);
            EXPECT_EQ(o.first_owner, 1);
        }
    }
    EXPECT_TRUE(saw_airbase);
    // The squadrons land with their airbase anchors intact.
    int squadrons = 0;
    for (const auto& u : ws.units) {
        if (u.unit_class == f4::entities::UnitClass::Squadron) {
            ++squadrons;
            EXPECT_NE(u.airbase_id, 0u);
        }
    }
    EXPECT_EQ(squadrons, 2);
}

TEST(CampaignInitializer, WorldJsonKeepsThePassthroughSetRaw) {
    // The generated world JSON decodes every structured sub-file
    // (cmp/obj/uni never appear in the raw set) and carries no
    // preserve-mode subfiles_b64 block — the passthrough-typed
    // .evt/.plt/.pst/.wth (plus .obd/.tea, both empty/deltas here) ride
    // in raw_subfiles exactly as cam2json emits them for a real save.
    const Built b = build_default();
    ClassTable ct;
    ct.load_json(FIXTURE_DIR "falcon4.ct.json");
    const std::string world_json =
        CampaignInitializer::to_world_json(b.result, ct, b.pack);
    EXPECT_NE(world_json.find("\"raw_subfiles\""), std::string::npos);
    EXPECT_EQ(world_json.find("\"subfiles_b64\""), std::string::npos);
}

TEST(CampaignInitializer, TheDefaultTaskingProfileIsALiveSavesOwn) {
    // A team without an explicit profile in the pack carries kunsan's
    // own mission priorities — a zero row would mean the team never
    // requests anything, and a generated war must be able to fight.
    Built b;
    b.pack = ScenarioPack::parse(kProfilePack);
    ClassTable ct;
    ct.load_json(FIXTURE_DIR "falcon4.ct.json");
    b.result = CampaignInitializer::build(b.pack, ct);
    CamArchive cam;
    cam.load_from_memory(b.result.cam_bytes);
    const SubFile* tea = cam.find("tea");
    ASSERT_NE(tea, nullptr);
    const DecodedTeams d = decode_tea(tea->data.data(), tea->data.size(), 71);
    ASSERT_EQ(d.count, 8);
    bool nonzero = false;
    for (const auto& mp : d.teams[1].mission_priority)
        if (mp != 0) nonzero = true;
    EXPECT_TRUE(nonzero)
        << "the synthesized team carries a live tasking profile";
    // The ATM airbases: one per squadron home base, schedule all zeros.
    ASSERT_EQ(d.teams[1].atm.airbases.size(), 1u);
    for (const uint8_t block : d.teams[1].atm.airbases[0].schedule)
        EXPECT_EQ(block, 0) << "every takeoff slot free";
}
