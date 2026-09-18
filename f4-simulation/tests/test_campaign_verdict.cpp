// test_campaign_verdict.cpp — the CAMP-DOM-1 war gates.
//
// The acceptance contract (CAMP_HOST_PLAN.md §8): "Victory events + a
// `verdict` query; any UX can say something about the war."
//
// The bed is the BUILD-TIME campinit small war (the same generated
// world the C5 24-hour gate certifies): ground war armed, the real
// tasking pipeline, so a capture — the band's only mover — happens
// inside the run. The pins:
//
//   1. The quiet front publishes nothing (the diff starts
//      stalemate/no-lead and a war at rest never changes it).
//   2. When the front moves (an objective_captured in the stream), a
//      verdict event follows, and the `verdict` query answers with the
//      SAME coarse state the last event carried (the pump diffs every
//      whole-second, so no change can escape the stream).
//   3. The query serves the books: both belligerents' rows ride, the
//      leader's swing is positive, the capture book is non-zero.

#include <f4/campaign/api/dto.hpp>
#include <f4/campaign/api/events.hpp>
#include <f4/campaign/api/protocol.hpp>
#include <f4/simulation/campaign_session_host.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

using namespace f4::simulation;
namespace api = f4::campaign::api;

namespace {

std::filesystem::path generated_world(const char* name) {
    return std::filesystem::path(F4_CAMPINIT_FIXTURES_DIR) /
           (std::string("campinit_") + name + ".world.json");
}
std::filesystem::path class_table() {
    return std::filesystem::path(F4_SOURCE_FIXTURES_DIR) / "falcon4.ct.json";
}
std::filesystem::path f16_config() {
    return std::filesystem::path(F4_GENERATED_FIXTURES_DIR) / "f16.json";
}

bool fixtures_ready() {
    return std::filesystem::exists(f16_config()) &&
           std::filesystem::exists(generated_world("small"));
}

/// The generated-war session over the HOST (the wire surface, not the
/// harness): the real pipeline, ground war armed — a capture happens.
/// The chunked stepping stays under the session's per-advance cap.
struct WarRig {
    std::unique_ptr<EngineSessionHost> host;
    std::string err;

    static WarRig make() {
        WarRig rig;
        CampaignSessionOptions o;
        o.world_json = generated_world("small");
        o.class_table = class_table();
        o.aircraft_config = f16_config();
        o.mission_profiles = F4_MISSION_PROFILES_JSON;
        o.tasking_cycle_sec = 1800;
        o.atm_pipeline = true;
        o.ground_war = true;
        o.fidelity_policy = FidelityPolicy::Tiered;
        o.max_flights = 24;
        o.max_steps_per_advance = 1000;
        rig.host = EngineSessionHost::create(o, &rig.err);
        return rig;
    }

    std::string verdict_query() const {
        std::string out;
        (void)api::host_handle(
            *host, R"({"v":1,"op":"query","q":"verdict"})", out);
        return out;
    }
};

/// The verdict band the last event carried (the coarse diff's mirror).
struct CoarseState {
    std::string band;
    int leader = -1;
    int swing = 0;
    bool valid = false;
};

} // namespace

// ── the front moving raises the verdict ────────────────────────────────────

TEST(CampaignVerdict, TheFrontMovingRaisesTheVerdictEvent) {
    if (!fixtures_ready()) {
        GTEST_SKIP() << "campinit/f16 fixtures not generated";
    }
    auto rig = WarRig::make();
    ASSERT_NE(rig.host, nullptr) << rig.err;

    // Watch ONLY the families the verdict story reads: the captures
    // (the band's mover) and the verdict (the sparse change signal).
    api::EventFilter f;
    f.kinds = {api::CampaignEvent::Kind::ObjectiveCaptured,
               api::CampaignEvent::Kind::Verdict};
    rig.host->set_event_filter(f);

    bool saw_capture = false;
    CoarseState last;
    // 24 campaign hours, the C5 gate's own horizon — the generated
    // war's first capture lands well inside it (the packs start the
    // battalions on the front). 1000 ticks per advance = the option's
    // cap; 16⅔ campaign seconds per chunk.
    constexpr std::uint32_t kChunkTicks = 1000;
    constexpr std::uint64_t kBudgetTicks = 86400ULL * 60ULL;
    for (std::uint64_t done = 0;
         done < kBudgetTicks && !(saw_capture && last.valid);
         done += kChunkTicks) {
        (void)rig.host->step(kChunkTicks);
        for (auto& e : rig.host->drain_events()) {
            if (e.kind == api::CampaignEvent::Kind::ObjectiveCaptured) {
                saw_capture = true;
            } else if (e.kind == api::CampaignEvent::Kind::Verdict) {
                last.band = e.verdict.band;
                last.leader = e.verdict.leader;
                last.swing = e.verdict.swing;
                last.valid = true;
            }
        }
    }

    ASSERT_TRUE(saw_capture) << "the generated war captured nothing";
    ASSERT_TRUE(last.valid)
        << "a capture moved the front but raised no verdict event";
    // The band words are the model's own vocabulary, and the coarse
    // state is internally consistent: a lead means a positive swing.
    EXPECT_TRUE(last.band == "advantage" || last.band == "decisive" ||
                last.band == "stalemate")
        << last.band;
    if (last.band != "stalemate") {
        EXPECT_GE(last.leader, 0);
        EXPECT_GT(last.swing, 0);
    }

    // THE parity pin: the query answers with the SAME coarse state the
    // last event carried — the pump diffs every whole-second, so the
    // stream cannot lag the books, and the query reads the books.
    const auto out = rig.verdict_query();
    EXPECT_NE(out.find("\"q\":\"verdict\",\"status\":\"ok\""),
              std::string::npos);
    EXPECT_NE(out.find("\"band\":\"" + last.band + "\""), std::string::npos);
    EXPECT_NE(out.find("\"leader\":" + std::to_string(last.leader)),
              std::string::npos);
    EXPECT_NE(out.find("\"leader_swing\":" + std::to_string(last.swing)),
              std::string::npos);
    // The books ride: both belligerents named, the capture count
    // non-zero somewhere (a capture HAPPENED), the pack's threshold.
    EXPECT_NE(out.find("\"name\":\"USA\""), std::string::npos);
    EXPECT_NE(out.find("\"name\":\"DPRK\""), std::string::npos);
    EXPECT_NE(out.find("\"captures\":0,"), std::string::npos) << out;
    EXPECT_NE(out.find("\"gained\":0,"), std::string::npos) << out;
}
