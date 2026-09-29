// TEMP DIAGNOSTIC — the user's bug 1 report: campaign flights "moving
// along the ground (not flying) to target points". Runs the viewer's
// exact session over the showcase save-derived world and samples the
// live aircraft fleet's ground/air state each sim minute.

#include <gtest/gtest.h>

#include "f4/simulation/campaign_session.hpp"

#include <f4/ai/brain_component.hpp>
#include <f4/entities/entity.hpp>
#include <f4/flight/flight_model_component.hpp>

#include <filesystem>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace {

std::string generated_fixture(const char* name) {
    const char* env = std::getenv("F4_GENERATED_FIXTURES_DIR");
    std::string dir = env ? env : "";
#ifdef F4_GENERATED_FIXTURES_DIR
    if (dir.empty()) dir = F4_GENERATED_FIXTURES_DIR;
#endif
    if (dir.empty()) return "";
    const auto path = std::filesystem::path(dir) / name;
    return std::filesystem::exists(path) ? path.generic_string() : "";
}

} // namespace

TEST(FlightStateDiag, ShowcaseSessionAirborneTrace) {
    const auto f16 = generated_fixture("f16.json");
    if (f16.empty()) GTEST_SKIP() << "f16.json fixture not generated";
    const std::filesystem::path world =
        "E:/Code/F4/showcase.world.json";
    if (!std::filesystem::exists(world)) {
        GTEST_SKIP() << "showcase world not present";
    }

    f4::simulation::CampaignSessionOptions o;
    o.world_json = world;
    o.class_table =
        "E:/Code/F4/f4-world-convert/tests/fixtures/falcon4.ct.json";
    o.aircraft_config = f16;
    {
        // The profiles fixture lives in its own generated dir (the QC
        // tool's default: <bin>/generated_campaign).
        std::filesystem::path profiles = generated_fixture("MissionProfiles.json");
        if (profiles.empty()) {
            profiles = "E:/Code/F4/Build/generated_campaign/MissionProfiles.json";
        }
        o.mission_profiles = profiles;
    }
    o.max_flights = 48;
    // The flight-state diagnostics pin the pre-FID spawn-at-init
    // behavior — keep FullFidelity EXPLICITLY now that the session
    // default is Tiered.
    o.fidelity_policy =
        f4::simulation::FidelityPolicy::FullFidelity;
    o.aa_combat = true;
    o.initial_tasking_cycle = true;
    o.atm_pipeline = true;
    o.strategy_layer = true;

    std::string err;
    auto session = f4::simulation::CampaignSession::create(o, &err);
    ASSERT_NE(session, nullptr) << err;

    auto& sim = session->sim();
    std::vector<std::string> timeline_last_(2);
    std::printf("roster at create: %zu aircraft\n",
                sim.aircraft_entities().size());

    for (int minute = 0; minute <= 20; ++minute) {
        if (minute > 0) session->advance(60.0);
        int ground = 0, enroute = 0, approach = 0, complete = 0, no_fm = 0;
        int in_air = 0;
        int with_plan = 0, no_plan = 0;
        std::map<std::string, int> takeoff_state;
        double x_sum = 0.0, y_sum = 0.0, z_sum = 0.0;
        int z_n = 0;
        for (const auto eid : sim.aircraft_entities()) {
            f4::entities::EntityHandle h(eid, &sim.world());
            const auto* fm = h.get<f4::flight::FlightModelComponent>();
            const auto* brain = h.get<f4::ai::BrainComponent>();
            if (fm == nullptr) { ++no_fm; continue; }
            if (fm->model().state().gear.inAir) ++in_air;
            const auto* tr = h.get<f4::entities::TransformComponent>();
            if (tr != nullptr) {
                z_sum += tr->position.z;
                x_sum += tr->position.x;
                y_sum += tr->position.y;
                ++z_n;
            }
            if (brain == nullptr) continue;
            switch (brain->phase()) {
                case f4::ai::BrainComponent::Phase::Ground:    ++ground; break;
                case f4::ai::BrainComponent::Phase::Enroute:   ++enroute; break;
                case f4::ai::BrainComponent::Phase::Approach:  ++approach; break;
                case f4::ai::BrainComponent::Phase::Complete:  ++complete; break;
            }
            if (minute == 5) {
                if (brain->mission_plan().route.empty()) ++no_plan;
                else {
                    ++with_plan;
                    if (with_plan == 1) {
                        std::printf("  sample plan: start_phase=%d wps=%zu\n",
                                    static_cast<int>(
                                        brain->mission_plan().start_phase),
                                    brain->mission_plan().route.size());
                    }
                }
                // TakeoffModule state census (the stall hunt).
                const auto& sm = brain->module();
                ++takeoff_state[sm.state_name()];
            }
        }
        std::printf("t=%3dmin roster=%5zu inAir=%4d ground=%4d enroute=%4d "
                    "approach=%3d complete=%4d nofm=%d avgZ(ft)=%.0f "
                    "avgXY=(%.0f,%.0f) plan=%d noplan=%d\n",
                    minute, sim.aircraft_entities().size(), in_air, ground,
                    enroute, approach, complete, no_fm,
                    z_n > 0 ? z_sum / z_n : 0.0,
                    z_n > 0 ? x_sum / z_n : 0.0,
                    z_n > 0 ? y_sum / z_n : 0.0,
                    minute == 5 ? with_plan : 0,
                    minute == 5 ? no_plan : 0);
        if (minute >= 1 && minute <= 20) {
            // Continuous state timeline for two aircraft: where does the
            // time actually go between Taxi and liftoff?
            for (std::size_t k : {std::size_t{0}, std::size_t{1}}) {
                if (k >= sim.aircraft_entities().size()) break;
                const auto eid = sim.aircraft_entities()[k];
                f4::entities::EntityHandle h(eid, &sim.world());
                const auto* brain = h.get<f4::ai::BrainComponent>();
                const auto* fm = h.get<f4::flight::FlightModelComponent>();
                if (brain == nullptr || fm == nullptr) continue;
                const std::string st = brain->module().state_name();
                if (timeline_last_[k] != st) {
                    timeline_last_[k] = st;
                    std::printf("  tl[%zu] t=%3dmin state=%s vcas=%.1f\n",
                                k, minute, st.c_str(),
                                fm->model().state().vcas);
                }
            }
        }
        if (minute == 3) {
            // One aircraft's taxi deep-dive: commanded vs actual speed.
            const auto eid = sim.aircraft_entities()[1];
            f4::entities::EntityHandle h(eid, &sim.world());
            for (int s30 = 0; s30 < 4; ++s30) {
                const auto* tr = h.get<f4::entities::TransformComponent>();
                const auto* fm = h.get<f4::flight::FlightModelComponent>();
                const auto* brain = h.get<f4::ai::BrainComponent>();
                if (tr == nullptr || fm == nullptr || brain == nullptr) break;
                const auto& st = fm->model().state();
                std::printf("  dive[%llu] +%ds pos=(%.0f,%.0f) vcas=%4.1f kts "
                            "thr=%.3f brake=%d state=%s\n",
                            static_cast<unsigned long long>(eid.value), s30 * 30,
                            tr->position.x, tr->position.y, st.vcas,
                            fm->last_consumed_input().throttle,
                            fm->last_consumed_input().wheelBrakes ? 1 : 0,
                            brain->module().state_name().c_str());
                session->advance(30.0);
            }
        }
        if (minute == 5 || minute == 10 || minute == 15 || minute == 20) {
            for (const auto& [state, n] : takeoff_state) {
                std::printf("  takeoff[%s] = %d\n", state.c_str(), n);
            }
        }
        if (minute == 10) {
            // One aircraft's 60 s trajectory at 10 s granularity.
            const auto eid = sim.aircraft_entities()[1];
            f4::entities::EntityHandle h(eid, &sim.world());
            for (int s10 = 0; s10 < 6; ++s10) {
                const auto* tr = h.get<f4::entities::TransformComponent>();
                if (tr == nullptr) break;
                std::printf("  track[%llu] t+%02ds pos=(%.1f,%.1f) z=%.1f\n",
                            static_cast<unsigned long long>(eid.value),
                            s10 * 10, tr->position.x, tr->position.y,
                            tr->position.z);
                session->advance(10.0);
            }
        }
    }
    SUCCEED();
}
