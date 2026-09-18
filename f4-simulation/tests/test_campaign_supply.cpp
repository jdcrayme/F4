// test_campaign_supply.cpp — the CAMP-DOM-2 war gates.
//
// The acceptance contract (CAMP_HOST_PLAN.md §8): "upstream interdiction
// targets supply/fuel lines and objectives carry supply; deepen C2's
// pool into per-objective supply feeding reinforcement and repair
// rates."
//
// The engine's mechanics are pinned in f4-campaign's own tests; here we
// pin what the SESSION adds, over the kunsan fixture (the real-save
// rig — its objective supply rows carry the original game's 0xEB
// uninitialized-garbage bytes, which makes them the perfect signal):
//
//   1. THE LIVE OBJECTIVES QUERY — with a ground war running, the
//      `objectives` query serves the ENGINE's clamped mirror (235 →
//      100), not the save's raw rows; without one, the rows stay the
//      save's own bytes (the byte-identity rule). The DOM-1 seam
//      closes: the query is live where the war is.
//   2. THE SOURCED POOL — with the deepened flow armed, the resupply
//      fire regenerates belligerent-held objective stocks out of the
//      team's strategic stock (.tea supply_avail, 1000 on kunsan), and
//      the engine's books move (supply_regen_total > 0).
//   3. THE REPAIR CADENCE — seeded damage (feature 0 destroyed on
//      every belligerent-held objective) heals on the cadence, the
//      objective_repaired events publish, the entity mirror's face
//      joins the repair (VIS_REPAIRED, hp restored), and the write-
//      backs land the healed bitmap + the logistics face.
//   4. THE GOLDEN IDENTITY — every knob off: no repairs, no supply
//      movement, and the query echoes the save's own garbage bytes.

#include <f4/campaign/api/events.hpp>
#include <f4/campaign/api/protocol.hpp>
#include <f4/campaign/ground_war.hpp>
#include <f4/campaign/result_ledger.hpp>
#include <f4/entities/entity.hpp>
#include <f4/simulation/campaign_session.hpp>
#include <f4/simulation/campaign_session_host.hpp>
#include <f4/world/detail/world_state.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace f4::simulation;
namespace api = f4::campaign::api;

namespace {

std::filesystem::path kunsan_world() {
    return std::filesystem::path(F4_SIMULATION_TEST_FIXTURES_DIR) /
           "kunsan_campaign.world.json";
}
std::filesystem::path class_table() {
    return std::filesystem::path(F4_SOURCE_FIXTURES_DIR) / "falcon4.ct.json";
}
std::filesystem::path f16_config() {
    return std::filesystem::path(F4_GENERATED_FIXTURES_DIR) / "f16.json";
}

std::string slurp(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/// A kunsan world with SEEDED DAMAGE: every belligerent-held objective
/// (ROK 2 / DPRK 6) carries feature 0 destroyed and full supply — the
/// deterministic bed the repair cadence heals. Written to a temp file
/// through the WorldState's own round-trip (the emitter's face).
std::filesystem::path damaged_kunsan_world() {
    f4::world::WorldState ws;
    const std::string text = slurp(kunsan_world());
    ws.load_from_string(text);
    int seeded = 0;
    for (auto& o : ws.objectives) {
        if (o.owner != 2 && o.owner != 6) continue;
        o.fstatus = {0x03};   // feature 0 destroyed (VIS state 3)
        o.supply = 100;       // the repair gate's stock, satisfied
        ++seeded;
    }
    EXPECT_GT(seeded, 0) << "no belligerent-held objectives to seed";

    const auto path = std::filesystem::temp_directory_path() /
                      "f4_dom2_damaged_kunsan.world.json";
    std::ofstream out(path, std::ios::binary);
    out << ws.to_json_string();
    return path;
}

CampaignSessionOptions make_opts(const std::filesystem::path& world) {
    CampaignSessionOptions o;
    o.world_json = world;
    o.class_table = class_table();
    o.aircraft_config = f16_config();
    o.mission_profiles = F4_MISSION_PROFILES_JSON;
    o.tasking_cycle_sec = 5;
    o.reinforce_period_sec = 0;   // off: the air cadence is not this file's
    o.max_flights = 8;
    o.atm_pipeline = false;
    return o;
}

/// Count the objective_repaired events off the session's bus (the
/// host's own subscription shape — the id kept for the lifetime).
struct RepairCounter {
    int fired = 0;
    std::size_t sub = 0;

    void attach(CampaignSession& s) {
        sub = s.sim().bus().subscribe<f4::campaign::api::CampaignEvent>(
            [this](const f4::campaign::api::CampaignEvent& e) {
                if (e.kind ==
                    f4::campaign::api::CampaignEvent::Kind::ObjectiveRepaired) {
                    ++fired;
                }
            });
    }
};

std::string objectives_query(EngineSessionHost& host) {
    std::string out;
    (void)api::host_handle(
        host, R"({"v":1,"op":"query","q":"objectives"})", out);
    return out;
}
} // namespace

// ── 1. The live objectives query (the DOM-1 seam closes) ──────────────────

TEST(CampaignSupply, LiveQueryServesTheEngineMirror) {
    if (!std::filesystem::exists(f16_config())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    // The static save rows carry the 235 garbage; the engine's clamped
    // mirror carries 100. With the ground war ON, the query serves the
    // MIRROR: no 235 anywhere, and the clamped 100s ride.
    CampaignSessionOptions o = make_opts(kunsan_world());
    o.ground_war = true;
    o.ground_update_sec = 5;
    o.ground_orders_sec = 5;
    std::string err;
    auto host = EngineSessionHost::create(o, &err);
    ASSERT_NE(host, nullptr) << err;

    const std::string json = objectives_query(*host);
    EXPECT_EQ(json.find("\"supply\":235"), std::string::npos)
        << "the query served the save's raw garbage bytes";
    EXPECT_NE(json.find("\"supply\":100"), std::string::npos)
        << "the query never saw the clamped mirror";
    // The overlay's parity: the LIVE owner of a captured objective
    // would flip here too — no capture ran, so the owners agree with
    // the save (both sources, one truth at rest).
    EXPECT_NE(json.find("\"owner\":2"), std::string::npos);
}

TEST(CampaignSupply, QuietQueryKeepsTheSaveBytes) {
    if (!std::filesystem::exists(f16_config())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    // No ground war → the rows stay the WorldState's own: the 235s
    // ride untouched (the pre-DOM-2 byte-identity).
    std::string err;
    auto host = EngineSessionHost::create(make_opts(kunsan_world()), &err);
    ASSERT_NE(host, nullptr) << err;
    const std::string json = objectives_query(*host);
    EXPECT_NE(json.find("\"supply\":235"), std::string::npos);
    EXPECT_NE(json.find("\"supply\":0,"), std::string::npos);
}

// ── 2. The sourced pool ────────────────────────────────────────────────────

TEST(CampaignSupply, SourcedResupplyFeedsObjectiveStocks) {
    if (!std::filesystem::exists(f16_config())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    // Kunsan's belligerent-held objectives sit at supply 0 or 235(→100
    // clamped); the teams carry 1000-unit strategic stocks. The first
    // resupply fire regenerates the below-cap holdings — the deepened
    // pool's books move, the flat G1 refill could never do that.
    CampaignSessionOptions o = make_opts(kunsan_world());
    o.ground_war = true;
    o.ground_update_sec = 5;
    o.ground_orders_sec = 5;
    o.ground_resupply_sec = 60;
    o.ground_objective_supply = true;
    std::string err;
    auto session = CampaignSession::create(o, &err);
    ASSERT_NE(session, nullptr) << "create failed: " << err;
    session->set_paused(false);
    for (int frame = 0; frame < 70; ++frame) {   // ~70 campaign seconds
        session->advance(1.0);
    }

    const auto* war = session->ground_war();
    ASSERT_NE(war, nullptr);
    EXPECT_GT(war->stats().resupply_fires, 0);
    EXPECT_GT(war->stats().supply_regen_total, 0)
        << "the sourced fire regenerated nothing";
    // The flat refill is OFF in this mode — the battalions' supply
    // comes from depots or nothing (their seeds are 100 on kunsan, so
    // draws only happen after movement burn; no assertion either way).
    EXPECT_EQ(war->stats().features_repaired, 0) << "repair is OFF here";

    // Two identically-driven runs agree (the C5 contract, supply
    // edition).
    auto b = CampaignSession::create(o, &err);
    ASSERT_NE(b, nullptr) << err;
    for (int frame = 0; frame < 70; ++frame) b->advance(1.0);
    EXPECT_EQ(session->ledger_json(), b->ledger_json());
}

// ── 3. The repair cadence, end to end ──────────────────────────────────────

TEST(CampaignSupply, RepairHealsSeededDamageAndPublishes) {
    if (!std::filesystem::exists(f16_config())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    const auto world = damaged_kunsan_world();
    CampaignSessionOptions o = make_opts(world);
    o.ground_war = true;
    o.ground_update_sec = 5;
    o.ground_orders_sec = 5;
    o.ground_repair_sec = 60;
    std::string err;
    auto session = CampaignSession::create(o, &err);
    ASSERT_NE(session, nullptr) << "create failed: " << err;

    RepairCounter counter;
    counter.attach(*session);

    session->set_paused(false);
    for (int frame = 0; frame < 70; ++frame) {   // ~70 s: one+ fire
        session->advance(1.0);
    }

    // The engine healed: the repair log carries records, the events
    // published one-for-one, and the stats moved.
    const auto* war = session->ground_war();
    ASSERT_NE(war, nullptr);
    EXPECT_GT(war->stats().repair_fires, 0);
    EXPECT_GT(war->stats().features_repaired, 0);
    ASSERT_EQ(session->ledger().repair_log().size(),
              static_cast<std::size_t>(war->stats().features_repaired))
        << "one record per repaired objective per fire";
    EXPECT_GT(counter.fired, 0);
    EXPECT_EQ(counter.fired,
              static_cast<int>(session->ledger().repair_log().size()));

    // The entity mirror: a repaired objective's feature face is
    // VIS_REPAIRED (1) — the next damage sync diffs the truth.
    const auto& rec = session->ledger().repair_log()[0];
    const auto it = session->objective_id_map().find(rec.objective);
    ASSERT_NE(it, session->objective_id_map().end());
    auto h = f4::entities::EntityHandle(it->second,
                                        &session->sim().world());
    const auto* fs = h.get<f4::entities::FeatureSetComponent>();
    ASSERT_NE(fs, nullptr);
    ASSERT_FALSE(fs->features.empty());
    EXPECT_EQ(fs->features[0].damage_state, 1)
        << "feature 0 was seeded destroyed; the repair healed it";
    const auto* db = h.get<f4::entities::DamageBitmapComponent>();
    ASSERT_NE(db, nullptr);
    EXPECT_EQ(db->fstatus, rec.fstatus);

    // The write-backs: the healed bitmap rides the C1 walk (the
    // damage-state face), the logistics face rides the ground walk.
    const auto wv = session->apply_writeback();
    EXPECT_GT(wv.objectives_written, 0);
    const auto g = session->apply_ground_writeback();
    EXPECT_GT(g.objectives_resupplied, 0);
}

TEST(CampaignSupply, RepairOffLeavesTheSeededDamage) {
    if (!std::filesystem::exists(f16_config())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    // The golden identity: cadence 0 → nothing heals, nothing books.
    const auto world = damaged_kunsan_world();
    CampaignSessionOptions o = make_opts(world);
    o.ground_war = true;
    o.ground_update_sec = 5;
    o.ground_orders_sec = 5;
    std::string err;
    auto session = CampaignSession::create(o, &err);
    ASSERT_NE(session, nullptr) << err;
    session->set_paused(false);
    for (int frame = 0; frame < 20; ++frame) session->advance(1.0);

    const auto* war = session->ground_war();
    ASSERT_NE(war, nullptr);
    EXPECT_EQ(war->stats().repair_fires, 0);
    EXPECT_TRUE(session->ledger().repair_log().empty());
    // The seeded damage is still the mirror's truth (the engine
    // seeded the face; nothing healed it).
    bool seeded_seen = false;
    for (const auto& o : war->objectives()) {
        if (o.owner != 2 && o.owner != 6) continue;
        if (o.fstatus.empty()) continue;
        seeded_seen = true;
        EXPECT_EQ(o.fstatus[0] & 0x03, 3)
            << "an unrepaired objective still carries its damage";
        break;
    }
    EXPECT_TRUE(seeded_seen) << "the mirror never seeded the damage face";
}
