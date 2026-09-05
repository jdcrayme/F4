#include <f4/simulation/simulation.hpp>
#include <f4/ai/brain_component.hpp>
#include <f4/flight/flight_model_component.hpp>
#include <f4/entities/entity.hpp>
#include <cstdio>
#include <cmath>
#include <filesystem>
int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s <scenario.json>\n", argv[0]); return 1; }
    auto scenario = f4::simulation::load_scenario(argv[1]);
    f4::simulation::Simulation sim(scenario, std::filesystem::path(argv[1]).parent_path());
    sim.initialize();
    auto h = f4::entities::EntityHandle(sim.aircraft_entity(), &sim.world());
    auto* tf = h.get<f4::entities::TransformComponent>();
    auto* fm = h.get<f4::flight::FlightModelComponent>();
    auto* brain = h.get<f4::ai::BrainComponent>();
    double max_alt_err = 0, sum_alt_err = 0, max_bank = 0, max_vs = 0, min_vs = 0;
    int n = 0;
    const int TICKS = 12000;
    std::printf("tick    alt     alt_err   vcas   psi     pitch   roll    vs      phase\n");
    for (int i = 0; i < TICKS; ++i) {
        sim.tick(1.0/60.0);
        const auto& s = fm->state();
        const auto& pos = tf->position;
        double alt_err = pos.z - 10000.0;
        max_alt_err = std::max(max_alt_err, std::abs(alt_err));
        sum_alt_err += std::abs(alt_err);
        max_bank = std::max(max_bank, std::abs(f4::flight::to_degrees(s.kin.phi)));
        double vs = -s.kin.zdot * 60.0;
        max_vs = std::max(max_vs, vs);
        min_vs = std::min(min_vs, vs);
        n++;
        if (i % 600 == 0)
            std::printf("%5d %7.0f %8.0f %6.1f %6.1f %6.1f %6.1f %7.0f %s\n",
                i, pos.z, alt_err, s.vcas, f4::flight::to_degrees(s.kin.psi),
                f4::flight::to_degrees(s.kin.theta), f4::flight::to_degrees(s.kin.phi),
                vs, brain->phase_name());
    }
    std::printf("\n=== SUMMARY (%d s, %d ticks) ===\n", TICKS/60, n);
    std::printf("Max altitude error: %.0f ft\n", max_alt_err);
    std::printf("Avg altitude error: %.0f ft\n", sum_alt_err / n);
    std::printf("Max bank angle:     %.1f deg\n", max_bank);
    std::printf("VS range:           %.0f to %.0f fpm\n", min_vs, max_vs);
    return 0;
}
