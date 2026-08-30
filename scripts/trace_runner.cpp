// trace_runner.cpp — headless scenario runner + CSV trace + landing summary.
//
// Usage: trace_runner <scenario.json> [max_ticks] [log_every]
//
// Runs the scenario's Simulation to mission completion (or max_ticks),
// prints one state line every `log_every` ticks, captures the touchdown,
// and writes the FCS CSV trace (scenario fcs_trace_path).

#include <f4/simulation/simulation.hpp>
#include <f4/ai/brain_component.hpp>
#include <f4/ai/modules/takeoff_module.hpp>
#include <f4/ai/modules/landing_module.hpp>
#include <f4/ai/atc/messages.hpp>
#include <f4/flight/flight_model_component.hpp>
#include <f4/entities/entity.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

using namespace f4::simulation;
using namespace f4::ai;
namespace entities = f4::entities;
using f4::ai::modules::LandingState;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <scenario.json> [max_ticks] [log_every]\n", argv[0]);
        return 2;
    }
    const std::filesystem::path scenario_path{argv[1]};
    const int max_ticks  = (argc > 2) ? std::atoi(argv[2]) : 200000;
    const int log_every  = (argc > 3) ? std::atoi(argv[3]) : 300;

    auto scenario = load_scenario(scenario_path);
    Simulation sim(scenario, scenario_path.parent_path());
    sim.initialize();
    const auto& dsc = sim.scenario();

    auto h = entities::EntityHandle(sim.aircraft_entity(), &sim.world());
    auto* brain = h.get<BrainComponent>();
    auto* fm    = h.get<f4::flight::FlightModelComponent>();
    auto* tf    = h.get<entities::TransformComponent>();
    if (!brain || !fm || !tf) { std::fprintf(stderr, "missing components\n"); return 2; }

    // Runway axes for along/cross decomposition.
    const auto& thr = dsc.airfield.threshold_position;
    const double hdg = dsc.airfield.runway_heading_rad;
    const double ux = std::sin(hdg), uy = std::cos(hdg);
    const double cx = std::cos(hdg),  cy = -std::sin(hdg);
    const double rwy_len = std::hypot(
        dsc.airfield.runway_end_position.x - thr.x,
        dsc.airfield.runway_end_position.y - thr.y);
    auto along_of = [&](const f4::geo::WorldPosition& p) {
        return (p.x - thr.x) * ux + (p.y - thr.y) * uy; };
    auto cross_of = [&](const f4::geo::WorldPosition& p) {
        return (p.x - thr.x) * cx + (p.y - thr.y) * cy; };

    std::printf("scenario=%s rwy_hdg=%.1fdeg len=%.0fft thr=(%.0f,%.0f,%.0f)\n",
                dsc.name.c_str(), hdg * 57.29578, rwy_len, thr.x, thr.y, thr.z);

    // STAB diagnostics: log every go-around with its published reason.
    sim.bus().subscribe<f4::ai::atc::GoAroundMessage>(
        [&](const f4::ai::atc::GoAroundMessage& m) {
            std::printf("*** GO-AROUND: reason=%s (cleared=%d)\n",
                        m.reason.c_str(), 0);
        });

    bool saw_touchdown = false, completed = false, saw_goaround = false;
    double td_along = 0, td_cross = 0, td_vs = 0, td_vcas = 0, td_pitch = 0;
    int td_tick = -1, goaround_count = 0;
    std::string last_state;
    int last_state_tick = 0;
    int flare_tick = -1;

    for (int i = 0; i < max_ticks; ++i) {
        sim.tick(1.0 / 60.0);
        const auto& s = fm->state();
        const auto& pos = tf->position;
        const double alt_agl = f4::flight::altitude_agl_ft(s);
        const double vs_fpm  = -s.kin.zdot * 60.0;
        const char* ph = brain->phase_name();
        std::string st = brain->state_name();

        if (st != last_state) {
            std::printf("t=%8.2f  %-22s -> %s\n", i / 60.0, last_state.c_str(), st.c_str());
            last_state = st; last_state_tick = i;
        }
        if (brain->phase() == BrainComponent::Phase::Approach) {
            if (brain->landing().state() == LandingState::GoAround && !saw_goaround) {
                saw_goaround = true; ++goaround_count;
            } else if (brain->landing().state() != LandingState::GoAround &&
                       saw_goaround) {
                saw_goaround = false; // re-arm so multiple GAs are counted
            }
            if (brain->landing().state() == LandingState::Flare && flare_tick < 0)
                flare_tick = i;
            if (!saw_touchdown &&
                brain->landing().state() == LandingState::Rollout) {
                saw_touchdown = true;
                td_tick = i; td_along = along_of(pos); td_cross = cross_of(pos);
                td_vs = vs_fpm; td_vcas = s.vcas;
                td_pitch = f4::flight::to_degrees(s.kin.theta);
            }
        }

        if (i % log_every == 0) {
            std::printf("t=%8.2f ph=%-9s st=%-16s pos=(%8.0f,%8.0f,%7.0f) vcas=%6.1f "
                        "agl=%7.1f vs=%7.0f psi=%6.1f th=%5.1f phi=%5.1f alng=%9.0f lat=%7.0f\n",
                        i / 60.0, ph, st.c_str(), pos.x, pos.y, pos.z, s.vcas,
                        alt_agl, vs_fpm,
                        f4::flight::to_degrees(s.kin.psi),
                        f4::flight::to_degrees(s.kin.theta),
                        f4::flight::to_degrees(s.kin.phi),
                        along_of(pos), cross_of(pos));
        }
        if (brain->phase() == BrainComponent::Phase::Complete) {
            completed = true;
            for (int j = 0; j < 1200; ++j) sim.tick(1.0 / 60.0);
            break;
        }
    }

    sim.write_fcs_trace();

    std::printf("\n===== MISSION SUMMARY =====\n");
    std::printf("completed           : %s\n", completed ? "YES" : "NO (final phase state)");
    if (!completed)
        std::printf("final phase/state   : %s / %s (t=%.1f)\n", brain->phase_name(),
                    brain->state_name().c_str(), last_state_tick / 60.0);
    std::printf("go-arounds          : %d\n", goaround_count);
    std::printf("flare entered       : %s\n", flare_tick >= 0 ? ("YES (t=" + std::to_string(flare_tick / 60.0) + "s)").c_str() : "NO");
    if (saw_touchdown) {
        std::printf("touchdown           : t=%.1fs along=%+.0f ft cross=%+.0f ft\n",
                    td_tick / 60.0, td_along, td_cross);
        std::printf("touchdown vs/vcas   : %.0f fpm / %.1f kts / pitch %.1f deg\n",
                    td_vs, td_vcas, td_pitch);
        std::printf("runway bounds       : along in [-500, rwy_len=%.0f], |cross| < 75\n", rwy_len);
        std::printf("ON RUNWAY           : %s\n",
                    (td_along > -500.0 && td_along < rwy_len && std::abs(td_cross) < 75.0)
                        ? "YES" : "NO  <<<<<<");
    } else {
        std::printf("touchdown           : NEVER\n");
    }
    if (!dsc.fcs_trace_path.empty())
        std::printf("csv trace           : %s\n", dsc.fcs_trace_path.string().c_str());
    return 0;
}
