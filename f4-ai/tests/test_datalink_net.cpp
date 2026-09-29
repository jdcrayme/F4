// test_datalink_net.cpp — unit tests for the DatalinkTier
// (AI_IMPLEMENTATION_PLAN §15 Step 13).
//
// Covers:
//   1. DatalinkNet query semantics — seen_by / seen_by_entity bounds
//      safety, the team-table mirror, the entity-key index.
//   2. node_sees geometry — range edge, horizon clamp, ground sites.
//   3. The fusion's GCI leg — gate-off twin (byte-identical legacy),
//      net in range, net out of range (the asymmetry payload), team
//      isolation (red's net never lights blue's leg), node death
//      (unwire = legacy), own-team dark (a team the net does not
//      serve), policy composition (radar from the policy, GCI from
//      the net), and the two rebuild paths agreeing.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <f4/ai/datalink_net.hpp>
#include <f4/ai/f4_ai.hpp>
#include <f4/entities/f4_entities.hpp>
#include <f4/geo/f4_geo.hpp>
#include <f4/messaging/f4_messaging.hpp>

using namespace f4::ai;
using namespace f4::entities;
using namespace f4::geo;
using namespace f4::messaging;

namespace {

constexpr double FT_PER_NM = 6076.115485;
constexpr double FPS_PER_KT = 1.687809857;

struct EntitySpec {
    WorldPosition pos{};
    WorldPosition vel{};
    const char* team = nullptr;
    const char* role = nullptr;
};

std::uint64_t add_entity(EntityWorld& w, const EntitySpec& s) {
    EntityHandle h = w.create();
    auto& tf = h.add<TransformComponent>();
    tf.position = s.pos;
    tf.vx = s.vel.x;
    tf.vy = s.vel.y;
    tf.vz = s.vel.z;
    if (s.team) h.set_tag(tags::TEAM, TagValue::from(std::string(s.team)));
    if (s.role) h.set_tag(tags::ROLE, TagValue::from(std::string(s.role)));
    return h.id().value;
}

struct OwnshipSpec {
    WorldPosition pos{0.0, 0.0, 20000.0};
    WorldPosition vel{0.0, 600.0 * FPS_PER_KT, 0.0};
};

/// A detection policy that answers all-false (the radar-backed
/// adapter's shape with a dead radar and no passive legs — the cleanest
/// composition probe: every flag the fusion reports must come from
/// exactly one source).
struct BlindPolicy final : public SensorFusion::DetectionPolicy {
    Verdict classify(const TargetInfo&) override { return {}; }
};

/// A net with one node of `node_team` at `node_pos`, serving the given
/// team table. `radius_nm` / `min_alt` shape the geometry.
DatalinkNet make_net(std::vector<std::string> teams,
                     std::int16_t node_team, WorldPosition node_pos,
                     double radius_nm = 200.0, double min_alt = 0.0,
                     bool ground = false) {
    DatalinkNet net;
    net.teams = std::move(teams);
    DatalinkNode node;
    node.entity_id = 777;
    node.team = node_team;
    node.position = node_pos;
    node.range_nm = radius_nm;
    node.min_alt_ft = min_alt;
    node.is_ground_site = ground;
    net.nodes.push_back(node);
    return net;
}

/// Fill the net's contact state for one contact (the host's walk shape:
/// one mask entry + one index entry per contact).
void add_contact(DatalinkNet& net, std::uint64_t entity_id,
                 const WorldPosition& pos) {
    const std::size_t idx = net.contact_seen_teams.size();
    std::uint32_t mask = 0;
    for (const auto& n : net.nodes) {
        if (n.team < 0) continue;
        if (node_sees(n, pos)) {
            mask |= (1u << static_cast<unsigned>(n.team));
        }
    }
    net.contact_seen_teams.push_back(mask);
    net.contact_index_by_entity.emplace(entity_id, idx);
}

const TargetInfo* find_target(const SensorFusion& sf, std::uint64_t id) {
    for (const auto& t : sf.targets()) {
        if (t.entity_id == id) return &t;
    }
    return nullptr;
}

} // anonymous namespace

// ============================================================================
// DatalinkNet query semantics
// ============================================================================

TEST(DatalinkNet, SeenByBoundsSafety) {
    DatalinkNet net = make_net({"blue", "red"}, /*node_team=*/0, {});
    net.contact_seen_teams.push_back(0x1u);  // contact 0: blue sees it
    net.contact_index_by_entity.emplace(42u, 0);

    EXPECT_TRUE(net.seen_by(0, 0));
    EXPECT_FALSE(net.seen_by(0, 1));       // red has no node here
    EXPECT_FALSE(net.seen_by(0, -1));      // untagged team
    EXPECT_FALSE(net.seen_by(0, 31));      // out of the mask domain
    EXPECT_FALSE(net.seen_by(1, 0));       // no such contact
    // The entity-keyed seam agrees with the index form for a KNOWN id:
    // 42 -> contact 0, and team 0 holds the bit (the same query the
    // fusion's emplace_target makes through emplace_target's id seam).
    EXPECT_TRUE(net.seen_by_entity(42u, 0));
    EXPECT_FALSE(net.seen_by_entity(42u, 1));   // known id, red dark
    EXPECT_FALSE(net.seen_by_entity(999u, 0));  // unknown id
}

TEST(DatalinkNet, TeamIndexResolution) {
    const DatalinkNet net = make_net({"blue", "red", "green"}, 0, {});
    EXPECT_EQ(net.team_index("blue"), 0);
    EXPECT_EQ(net.team_index("red"), 1);
    EXPECT_EQ(net.team_index("green"), 2);
    EXPECT_EQ(net.team_index("purple"), -1);
    EXPECT_EQ(net.team_index(""), -1);
}

// ============================================================================
// node_sees geometry (v1: range + horizon clamp, flat-earth simple)
// ============================================================================

TEST(DatalinkNodeGeometry, RangeEdge) {
    DatalinkNode node;
    node.position = WorldPosition{0.0, 0.0, 25000.0};
    node.range_nm = 100.0;
    const double r100 = 100.0 * FT_PER_NM;
    EXPECT_TRUE(node_sees(node, WorldPosition{r100, 0.0, 20000.0}));
    // One foot beyond the radius: dark.
    EXPECT_FALSE(node_sees(node, WorldPosition{r100 + 1.0, 0.0, 20000.0}));
    // The range is HORIZONTAL: a contact at the radius but far below
    // (altitude aside) is inside.
    EXPECT_TRUE(node_sees(node, WorldPosition{0.0, r100, 1000.0}));
}

TEST(DatalinkNodeGeometry, HorizonClamp) {
    DatalinkNode node;
    node.position = WorldPosition{0.0, 0.0, 25000.0};
    node.range_nm = 100.0;
    node.min_alt_ft = 5000.0;  // nothing below 5,000 ft MSL
    EXPECT_TRUE(node_sees(node, WorldPosition{10.0 * FT_PER_NM, 0.0, 5000.0}));
    EXPECT_TRUE(node_sees(node, WorldPosition{10.0 * FT_PER_NM, 0.0, 20000.0}));
    EXPECT_FALSE(node_sees(node, WorldPosition{10.0 * FT_PER_NM, 0.0, 4999.0}));
    // A ridge-hugging penetrator at 500 ft: dark to the node.
    EXPECT_FALSE(node_sees(node, WorldPosition{60.0 * FT_PER_NM, 0.0, 500.0}));
}

TEST(DatalinkNodeGeometry, GroundSiteShape) {
    // A ground radar site sits at terrain height and sees up: the
    // is_ground_site bit is informational, the geometry is the same.
    DatalinkNode site;
    site.position = WorldPosition{0.0, 0.0, 500.0};
    site.range_nm = 150.0;
    site.is_ground_site = true;
    EXPECT_TRUE(node_sees(site, WorldPosition{50.0 * FT_PER_NM, 0.0, 30000.0}));
    EXPECT_FALSE(node_sees(site, WorldPosition{200.0 * FT_PER_NM, 0.0, 30000.0}));
}

// ============================================================================
// The fusion's GCI leg (Step 13)
// ============================================================================

TEST(DatalinkFusion, GateOffTwinIsTheLegacyOmniscientLeg) {
    // No net wired: the legacy rule stands byte-for-byte — a hostile
    // 250 NM out is GCI-seen (the theater rumor) even though every
    // range-gated leg is dark.
    EntityWorld world;
    MessageBus bus;
    OwnshipSpec os;
    const auto own_id = add_entity(world, {os.pos, os.vel, "blue", "fighter"});
    const auto foe_id =
        add_entity(world, {WorldPosition{0.0, 250.0 * FT_PER_NM, 20000.0},
                           {}, "red", "fighter"});

    SensorFusion sf;
    sf.initialize(own_id, world, bus, SkillLevel::Veteran);
    sf.update(1.0);

    const TargetInfo* t = find_target(sf, foe_id);
    ASSERT_NE(t, nullptr);
    EXPECT_TRUE(t->detected_by_gci);
    EXPECT_FALSE(t->detected_by_radar);  // 250 NM > the 80 NM radar gate
    EXPECT_TRUE(SensorFusion::can_see(*t));
}

TEST(DatalinkFusion, NetInsideRadiusKeepsTheContactSeen) {
    // A node of the ownship's team inside the radius: GCI stays lit.
    EntityWorld world;
    MessageBus bus;
    OwnshipSpec os;
    const auto own_id = add_entity(world, {os.pos, os.vel, "blue", "fighter"});
    const auto foe_id =
        add_entity(world, {WorldPosition{0.0, 100.0 * FT_PER_NM, 20000.0},
                           {}, "red", "fighter"});

    DatalinkNet net = make_net({"blue", "red"}, 0,
                               WorldPosition{0.0, 0.0, 30000.0});
    add_contact(net, foe_id, WorldPosition{0.0, 100.0 * FT_PER_NM, 20000.0});

    SensorFusion sf;
    sf.initialize(own_id, world, bus, SkillLevel::Veteran);
    sf.set_datalink(&net);
    sf.update(1.0);

    const TargetInfo* t = find_target(sf, foe_id);
    ASSERT_NE(t, nullptr);
    EXPECT_TRUE(t->detected_by_gci);
    EXPECT_TRUE(SensorFusion::can_see(*t));
}

TEST(DatalinkFusion, NetBeyondRadiusDropsTheOmniscientRumor) {
    // THE PAYLOAD: a hostile 250 NM out with a 200 NM node is NOT
    // GCI-seen — red no longer auto-sees blue strikers beyond the
    // nodes' geometry. Radar/RWR/visual are range-dark; the contact
    // leaves the visible set entirely.
    EntityWorld world;
    MessageBus bus;
    OwnshipSpec os;
    const auto own_id = add_entity(world, {os.pos, os.vel, "blue", "fighter"});
    const auto foe_id =
        add_entity(world, {WorldPosition{0.0, 250.0 * FT_PER_NM, 20000.0},
                           {}, "red", "fighter"});

    DatalinkNet net = make_net({"blue", "red"}, 0,
                               WorldPosition{0.0, 0.0, 30000.0});
    add_contact(net, foe_id, WorldPosition{0.0, 250.0 * FT_PER_NM, 20000.0});

    SensorFusion sf;
    sf.initialize(own_id, world, bus, SkillLevel::Veteran);
    sf.set_datalink(&net);
    sf.update(1.0);

    const TargetInfo* t = find_target(sf, foe_id);
    ASSERT_NE(t, nullptr);
    EXPECT_FALSE(t->detected_by_gci);
    EXPECT_FALSE(t->detected_by_radar);
    EXPECT_FALSE(SensorFusion::can_see(*t));
}

TEST(DatalinkFusion, TeamIsolationRedsNetNeverLightsBluesLeg) {
    // The node broadcasts to ITS team only: a blue ownship under a
    // red-only net reads GCI-dark even inside the radius.
    EntityWorld world;
    MessageBus bus;
    OwnshipSpec os;
    const auto own_id = add_entity(world, {os.pos, os.vel, "blue", "fighter"});
    const auto foe_id =
        add_entity(world, {WorldPosition{0.0, 50.0 * FT_PER_NM, 20000.0},
                           {}, "red", "fighter"});

    DatalinkNet net = make_net({"blue", "red"}, /*node_team=*/1,
                               WorldPosition{0.0, 0.0, 30000.0});
    add_contact(net, foe_id, WorldPosition{0.0, 50.0 * FT_PER_NM, 20000.0});

    SensorFusion sf;
    sf.initialize(own_id, world, bus, SkillLevel::Veteran);
    sf.set_datalink(&net);
    sf.update(1.0);

    const TargetInfo* t = find_target(sf, foe_id);
    ASSERT_NE(t, nullptr);
    EXPECT_FALSE(t->detected_by_gci);
}

TEST(DatalinkFusion, OwnTeamAbsentFromNetReadsDark) {
    // A team the net does not serve (the ownship untagged, or the net
    // carries no such team) is GCI-dark — the net only broadcasts to
    // teams it knows.
    EntityWorld world;
    MessageBus bus;
    OwnshipSpec os;
    // Untagged ownship (no TEAM tag).
    const auto own_id = add_entity(world, {os.pos, os.vel, nullptr, "fighter"});
    const auto foe_id =
        add_entity(world, {WorldPosition{0.0, 50.0 * FT_PER_NM, 20000.0},
                           {}, "red", "fighter"});

    DatalinkNet net = make_net({"red"}, 0, WorldPosition{0.0, 0.0, 30000.0});
    add_contact(net, foe_id, WorldPosition{0.0, 50.0 * FT_PER_NM, 20000.0});

    SensorFusion sf;
    sf.initialize(own_id, world, bus, SkillLevel::Veteran);
    sf.set_datalink(&net);
    sf.update(1.0);

    const TargetInfo* t = find_target(sf, foe_id);
    ASSERT_NE(t, nullptr);
    EXPECT_FALSE(t->detected_by_gci);
}

TEST(DatalinkFusion, NodeDeathUnwiresToTheLegacyLeg) {
    // The host's rule: gate on but NO live nodes anywhere = the legacy
    // leg (the plan's "gate off OR no live nodes" line). The host
    // unwires the net; the fusion's omniscient rule stands.
    EntityWorld world;
    MessageBus bus;
    OwnshipSpec os;
    const auto own_id = add_entity(world, {os.pos, os.vel, "blue", "fighter"});
    const auto foe_id =
        add_entity(world, {WorldPosition{0.0, 250.0 * FT_PER_NM, 20000.0},
                           {}, "red", "fighter"});

    DatalinkNet net = make_net({"blue", "red"}, 0,
                               WorldPosition{0.0, 0.0, 30000.0});
    add_contact(net, foe_id, WorldPosition{0.0, 250.0 * FT_PER_NM, 20000.0});

    SensorFusion sf;
    sf.initialize(own_id, world, bus, SkillLevel::Veteran);
    sf.set_datalink(&net);
    sf.update(1.0);
    const TargetInfo* t = find_target(sf, foe_id);
    ASSERT_NE(t, nullptr);
    EXPECT_FALSE(t->detected_by_gci);

    // The AWACS dies: the host unwires (nullptr). Omniscience returns.
    sf.set_datalink(nullptr);
    sf.force_refresh();
    t = find_target(sf, foe_id);
    ASSERT_NE(t, nullptr);
    EXPECT_TRUE(t->detected_by_gci);
}

TEST(DatalinkFusion, HorizonClampGatesTheFusionLeg) {
    // A ridge-hugging penetrator below the node's horizon clamp is not
    // broadcast — the fusion leg stays dark even inside the radius.
    // The low foe flies (400 kt, not the rig's stationary hover):
    // a STATIONARY entity below 8,000 ft is ground clutter to the
    // fusion's world walk (TransformComponent::is_ground_clutter) and
    // would never reach the datalink leg at all.
    EntityWorld world;
    MessageBus bus;
    OwnshipSpec os;
    const auto own_id = add_entity(world, {os.pos, os.vel, "blue", "fighter"});
    const auto low_foe =
        add_entity(world, {WorldPosition{0.0, 50.0 * FT_PER_NM, 500.0},
                           WorldPosition{0.0, 400.0 * FPS_PER_KT, 0.0},
                           "red", "fighter"});
    const auto high_foe =
        add_entity(world, {WorldPosition{0.0, 50.0 * FT_PER_NM, 20000.0},
                           {}, "red", "fighter"});

    DatalinkNet net = make_net({"blue", "red"}, 0,
                               WorldPosition{0.0, 0.0, 30000.0},
                               /*radius_nm=*/200.0, /*min_alt=*/5000.0);
    add_contact(net, low_foe, WorldPosition{0.0, 50.0 * FT_PER_NM, 500.0});
    add_contact(net, high_foe, WorldPosition{0.0, 50.0 * FT_PER_NM, 20000.0});

    SensorFusion sf;
    sf.initialize(own_id, world, bus, SkillLevel::Veteran);
    sf.set_datalink(&net);
    sf.update(1.0);

    const TargetInfo* low = find_target(sf, low_foe);
    ASSERT_NE(low, nullptr);
    EXPECT_FALSE(low->detected_by_gci);
    const TargetInfo* high = find_target(sf, high_foe);
    ASSERT_NE(high, nullptr);
    EXPECT_TRUE(high->detected_by_gci);
}

TEST(DatalinkFusion, PolicyCompositionRadarFromPolicyGciFromNet) {
    // The policy owns radar/RWR/visual; the net owns GCI. A blind
    // policy + an in-range node: every policy flag false, GCI true —
    // the composition the campaign arm builds (radar truth + the
    // datalink picture).
    EntityWorld world;
    MessageBus bus;
    OwnshipSpec os;
    const auto own_id = add_entity(world, {os.pos, os.vel, "blue", "fighter"});
    const auto foe_id =
        add_entity(world, {WorldPosition{0.0, 30.0 * FT_PER_NM, 20000.0},
                           {}, "red", "fighter"});

    BlindPolicy policy;
    DatalinkNet net = make_net({"blue", "red"}, 0,
                               WorldPosition{0.0, 0.0, 30000.0});
    add_contact(net, foe_id, WorldPosition{0.0, 30.0 * FT_PER_NM, 20000.0});

    SensorFusion sf;
    sf.initialize(own_id, world, bus, SkillLevel::Veteran);
    sf.set_detection_policy(&policy);
    sf.set_datalink(&net);
    sf.update(1.0);

    const TargetInfo* t = find_target(sf, foe_id);
    ASSERT_NE(t, nullptr);
    EXPECT_FALSE(t->detected_by_radar);   // the policy's answer
    EXPECT_FALSE(t->detected_by_rwr);
    EXPECT_FALSE(t->detected_by_visual);
    EXPECT_TRUE(t->detected_by_gci);      // the net's answer
    EXPECT_TRUE(SensorFusion::can_see(*t));
}

TEST(DatalinkFusion, PicturePathAndWorldPathAgree) {
    // Both rebuild paths consume the net through the same id-keyed
    // seam: the world-query path and the host picture path produce the
    // same GCI verdicts for the same contacts.
    EntityWorld world;
    MessageBus bus;
    OwnshipSpec os;
    const auto own_id = add_entity(world, {os.pos, os.vel, "blue", "fighter"});
    const auto in_foe =
        add_entity(world, {WorldPosition{0.0, 60.0 * FT_PER_NM, 20000.0},
                           {}, "red", "fighter"});
    const auto out_foe =
        add_entity(world, {WorldPosition{0.0, 250.0 * FT_PER_NM, 20000.0},
                           {}, "red", "fighter"});

    DatalinkNet net = make_net({"blue", "red"}, 0,
                               WorldPosition{0.0, 0.0, 30000.0});
    add_contact(net, in_foe, WorldPosition{0.0, 60.0 * FT_PER_NM, 20000.0});
    add_contact(net, out_foe, WorldPosition{0.0, 250.0 * FT_PER_NM, 20000.0});

    // World-query path.
    SensorFusion sf_world;
    sf_world.initialize(own_id, world, bus, SkillLevel::Veteran);
    sf_world.set_datalink(&net);
    sf_world.update(1.0);

    // Picture path: the host-built snapshot over the same entities.
    AirPicture picture;
    picture.teams = {"blue", "red"};
    const auto push_contact = [&](std::uint64_t id, const WorldPosition& p,
                                  std::int16_t team) {
        AirPictureContact c;
        c.entity_id = id;
        c.position = p;
        c.team = team;
        picture.contacts.push_back(c);
    };
    push_contact(in_foe, WorldPosition{0.0, 60.0 * FT_PER_NM, 20000.0}, 1);
    push_contact(out_foe, WorldPosition{0.0, 250.0 * FT_PER_NM, 20000.0}, 1);

    SensorFusion sf_pic;
    sf_pic.initialize(own_id, world, bus, SkillLevel::Veteran);
    sf_pic.set_datalink(&net);
    sf_pic.set_air_picture(&picture);
    sf_pic.update(1.0);

    const TargetInfo* w_in = find_target(sf_world, in_foe);
    const TargetInfo* p_in = find_target(sf_pic, in_foe);
    ASSERT_NE(w_in, nullptr);
    ASSERT_NE(p_in, nullptr);
    EXPECT_EQ(w_in->detected_by_gci, p_in->detected_by_gci);
    EXPECT_TRUE(w_in->detected_by_gci);

    const TargetInfo* w_out = find_target(sf_world, out_foe);
    const TargetInfo* p_out = find_target(sf_pic, out_foe);
    ASSERT_NE(w_out, nullptr);
    ASSERT_NE(p_out, nullptr);
    EXPECT_EQ(w_out->detected_by_gci, p_out->detected_by_gci);
    EXPECT_FALSE(w_out->detected_by_gci);
}

TEST(DatalinkFusion, ContactUnknownToTheNetReadsDarkUntilNextWalk) {
    // A contact the host's last walk never masked (it joined between
    // walks) reads NOT seen — the fusion never invents GCI knowledge
    // the net does not carry; the next picture walk picks it up.
    EntityWorld world;
    MessageBus bus;
    OwnshipSpec os;
    const auto own_id = add_entity(world, {os.pos, os.vel, "blue", "fighter"});
    const auto foe_id =
        add_entity(world, {WorldPosition{0.0, 50.0 * FT_PER_NM, 20000.0},
                           {}, "red", "fighter"});

    DatalinkNet net = make_net({"blue", "red"}, 0,
                               WorldPosition{0.0, 0.0, 30000.0});
    // NO add_contact — the host's walk has not masked it yet.

    SensorFusion sf;
    sf.initialize(own_id, world, bus, SkillLevel::Veteran);
    sf.set_datalink(&net);
    sf.update(1.0);

    const TargetInfo* t = find_target(sf, foe_id);
    ASSERT_NE(t, nullptr);
    EXPECT_FALSE(t->detected_by_gci);

    // The next walk masks it: the leg lights.
    add_contact(net, foe_id, WorldPosition{0.0, 50.0 * FT_PER_NM, 20000.0});
    sf.force_refresh();
    t = find_target(sf, foe_id);
    ASSERT_NE(t, nullptr);
    EXPECT_TRUE(t->detected_by_gci);
}
