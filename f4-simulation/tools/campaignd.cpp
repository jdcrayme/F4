// campaignd — the campaign engine's REFERENCE HOST (CAMP_HOST_PLAN.md
// §7, CAMP-HOST-1). A headless process that speaks the contract over
// line-delimited JSON on stdio: no sockets in the engine, ever — a
// realtime UX bridges stdio→socket on ITS side of the boundary.
//
//   campaignd --world war.world.json \
//             [--class-table falcon4.ct.json] [--aircraft f16.json] \
//             [--mission-profiles MissionProfiles.json] \
//             [--policy tiered|full] [--max-flights N] \
//             [--tasking-cycle-sec S] [--reinforce-period-sec S] \
//             [--no-atm] [--aa-combat] [--ground-war] [--strategy] \
//             [--max-steps-per-advance N]
//
// Session flow: on start the host emits the `hello` response (protocol
// version + identity fingerprint) and then answers one line per
// request. Every response is exactly one line; stderr carries the human
// notes (the diagnostics the QC tool prints to stdout — here stdout IS
// the wire and must stay clean).
//
// Exit codes (the plan §7 table — campaignd's contract with CI):
//   0  green (EOF, and no refusal on the last command)
//  20  protocol violation (malformed line, bad version, unknown op)
//  21  unknown query
//  22  command refused (the refusal rode the wire as data; a scripted
//       golden fails loudly at EOF when the LAST command was refused)
//  24  engine operation failed (save/query)
//  25  session construction failed (bad paths, unloadable data)

#include <f4/campaign/api/protocol.hpp>
#include <f4/simulation/campaign_session_host.hpp>

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>

#ifdef F4_SOURCE_DIR
#define F4_CAMPD_SOURCE_DIR F4_SOURCE_DIR
#else
#define F4_CAMPD_SOURCE_DIR "."
#endif

#ifdef F4_GENERATED_FIXTURES_DIR
#define F4_CAMPD_GENERATED F4_GENERATED_FIXTURES_DIR
#else
#define F4_CAMPD_GENERATED "."
#endif

#ifdef F4_MISSION_PROFILES_JSON
#define F4_CAMPD_PROFILES F4_MISSION_PROFILES_JSON
#else
#define F4_CAMPD_PROFILES ""
#endif

namespace {

namespace api = f4::campaign::api;

struct Args {
    std::filesystem::path world;
    std::filesystem::path class_table;
    std::filesystem::path aircraft_config;
    std::filesystem::path mission_profiles;
    f4::simulation::FidelityPolicy policy{
        f4::simulation::FidelityPolicy::Tiered};
    int max_flights = 48;
    int tasking_cycle_sec = 1800;
    int reinforce_period_sec = 43200;
    bool atm_pipeline = true;
    bool aa_combat = false;
    bool ground_war = false;
    bool strategy_layer = false;
    int max_steps = 240;

    [[nodiscard]] bool ok() const noexcept { return !world.empty(); }
};

void usage(std::ostream& os) {
    os << "campaignd — the campaign engine's reference host (stdio JSON)\n\n"
          "  --world PATH             the decoded campaign world (v71 JSON)\n"
          "  --class-table PATH       FALCON4.ct.json (vis-type resolution)\n"
          "  --aircraft PATH          the F-16 aircraft config\n"
          "  --mission-profiles PATH  MissionProfiles.json (tasking needs it)\n"
          "  --policy tiered|full     the fidelity mode (default tiered)\n"
          "  --max-flights N          saved-flight spawn cap (default 48)\n"
          "  --tasking-cycle-sec S    the ATM cycle (default 1800)\n"
          "  --reinforce-period-sec S (default 43200; 0 = off)\n"
          "  --no-atm                 the legacy ladder instead of the C4 pipeline\n"
          "  --aa-combat              arm the campaign flights for A/A (C6)\n"
          "  --ground-war             the ground war engine (G1)\n"
          "  --strategy               the ATM strategy layer (P7)\n"
          "  --max-steps-per-advance N (the dilation cap, default 240)\n";
}

[[nodiscard]] Args parse_args(int argc, char** argv) {
    Args a;
    // the compiled-in defaults (the same fallback chain the root
    // CMakeLists builds for the other tools)
    const std::filesystem::path src = F4_CAMPD_SOURCE_DIR;
    const std::filesystem::path gen = F4_CAMPD_GENERATED;
    if (const auto p = src / "Data" / "Classes" / "falcon4.ct.json";
        std::filesystem::exists(p)) {
        a.class_table = p;
    } else {
        a.class_table =
            src / "f4-world-convert" / "tests" / "fixtures" / "falcon4.ct.json";
    }
    a.aircraft_config = gen / "f16.json";
    if constexpr (sizeof(F4_CAMPD_PROFILES) > 1) {
        a.mission_profiles = F4_CAMPD_PROFILES;
    }

    for (int i = 1; i < argc; ++i) {
        const std::string k = argv[i];
        const auto next = [&]() -> std::string {
            return (i + 1 < argc) ? argv[++i] : "";
        };
        if (k == "--world") {
            a.world = next();
        } else if (k == "--class-table") {
            a.class_table = next();
        } else if (k == "--aircraft") {
            a.aircraft_config = next();
        } else if (k == "--mission-profiles") {
            a.mission_profiles = next();
        } else if (k == "--policy") {
            a.policy = next() == "full"
                           ? f4::simulation::FidelityPolicy::FullFidelity
                           : f4::simulation::FidelityPolicy::Tiered;
        } else if (k == "--max-flights") {
            a.max_flights = std::atoi(next().c_str());
        } else if (k == "--tasking-cycle-sec") {
            a.tasking_cycle_sec = std::atoi(next().c_str());
        } else if (k == "--reinforce-period-sec") {
            a.reinforce_period_sec = std::atoi(next().c_str());
        } else if (k == "--no-atm") {
            a.atm_pipeline = false;
        } else if (k == "--aa-combat") {
            a.aa_combat = true;
        } else if (k == "--ground-war") {
            a.ground_war = true;
        } else if (k == "--strategy") {
            a.strategy_layer = true;
        } else if (k == "--max-steps-per-advance") {
            a.max_steps = std::atoi(next().c_str());
        } else if (k == "--help" || k == "-h") {
            usage(std::cerr);
            std::exit(0);
        } else {
            std::cerr << "campaignd: unknown flag " << k << "\n";
            usage(std::cerr);
            std::exit(25);
        }
    }
    return a;
}

} // namespace

int main(int argc, char** argv) {
    const Args args = parse_args(argc, argv);
    if (!args.ok()) {
        std::cerr << "campaignd: --world is required\n";
        usage(std::cerr);
        return 25;
    }

    f4::simulation::CampaignSessionOptions opts;
    opts.world_json = args.world;
    opts.class_table = args.class_table;
    opts.aircraft_config = args.aircraft_config;
    opts.mission_profiles = args.mission_profiles;
    opts.fidelity_policy = args.policy;
    opts.max_flights = args.max_flights;
    opts.tasking_cycle_sec = args.tasking_cycle_sec;
    opts.reinforce_period_sec = args.reinforce_period_sec;
    opts.atm_pipeline = args.atm_pipeline;
    opts.aa_combat = args.aa_combat;
    opts.ground_war = args.ground_war;
    opts.strategy_layer = args.strategy_layer;
    opts.max_steps_per_advance = args.max_steps;

    std::string err;
    auto host = f4::simulation::EngineSessionHost::create(opts, &err);
    if (host == nullptr) {
        std::cerr << "campaignd: session construction failed: " << err
                  << "\n";
        return 25;
    }

    // The hello line goes through the SAME dispatcher a request would —
    // the reference host has no private dialect of its own.
    std::string out;
    (void)api::host_handle(*host, R"({"v":1,"op":"hello"})", out);
    std::cout << out << std::flush;

    bool last_refused = false;
    std::string line;
    while (std::getline(std::cin, line)) {
        // blank lines are not protocol violations — scripts end with
        // newlines, not errors
        bool blank = true;
        for (const char c : line) {
            if (!std::isspace(static_cast<unsigned char>(c))) {
                blank = false;
                break;
            }
        }
        if (blank) continue;

        out.clear();
        const auto outcome = api::host_handle(*host, line, out);
        std::cout << out << std::flush;
        if (outcome.kind == api::ProtocolOutcome::Kind::ProtocolError) {
            return outcome.exit_code;
        }
        if (outcome.kind == api::ProtocolOutcome::Kind::Refused) {
            last_refused = true;
        }
    }

    // EOF: green unless the last command was refused (a scripted golden
    // fails loudly — the plan §7 note).
    return last_refused ? 22 : 0;
}
