// campaignd — the campaign engine's REFERENCE HOST (CAMP_HOST_PLAN.md
// §7, CAMP-HOST-1; the journal modes are CAMP-HOST-2). A headless
// process that speaks the contract over line-delimited JSON on stdio:
// no sockets in the engine, ever — a realtime UX bridges stdio→socket
// on ITS side of the boundary.
//
//   campaignd --world war.world.json \
//             [--class-table falcon4.ct.json] [--aircraft f16.json] \
//             [--mission-profiles MissionProfiles.json] \
//             [--policy tiered|full] [--max-flights N] \
//             [--tasking-cycle-sec S] [--reinforce-period-sec S] \
//             [--no-atm] [--aa-combat] [--ground-war] [--strategy] \
//             [--max-steps-per-advance N]
//             [--journal war.jsonl | --verify-journal golden.jsonl]
//             [--command-journal cmds.jsonl | --replay-commands cmds.jsonl]
//
// Journal modes (CAMP-HOST-2 events; CAMP-CMD-1 commands):
//   --journal PATH          record the session's COMPLETE event stream
//                           (engine rate, unfiltered) as append-only
//                           JSONL: one identity header line, one line
//                           per event, one identity footer line.
//   --verify-journal PATH   the replay assertion: the run records as
//                           above AND compares every line against the
//                           golden byte-for-byte; any divergence (a
//                           moved book, a different seed, a lost event)
//                           exits 23 — the identity-drift guard. The
//                           two flags are mutually exclusive (25).
//   --command-journal PATH  record every APPLIED command as its line:
//                           the engine tick it applied at + the intent
//                           (refused commands mutate nothing — they
//                           stay off the record).
//   --replay-commands PATH  the identity statement's second half: the
//                           journal's commands re-apply at their
//                           recorded ticks as the run steps (the
//                           step segmentation is the host's job and
//                           this host does it), wire commands are
//                           refused while a replay is active, and at
//                           EOF the final identity must equal the
//                           journal's footer — "replay-with-commands
//                           reproduces the books" is a byte comparison.
//                           Header drift, leftover commands, or a
//                           footer mismatch exit 23; a malformed
//                           journal file exits 24. Composable with
//                           --verify-journal (the full assertion: same
//                           war, same commands, same events).
//
// Session flow: on start the host emits the `hello` response (protocol
// version + identity fingerprint) and then answers one line per
// request. A step response announces "events":N — the N event lines
// that follow it (subscribed clients only). Every response is exactly
// one line; stderr carries the human notes (the diagnostics the QC tool
// prints to stdout — here stdout IS the wire and must stay clean).
//
// Exit codes (the plan §7 table — campaignd's contract with CI):
//   0  green (EOF, and no refusal on the last command)
//  20  protocol violation (malformed line, bad version, unknown op)
//  21  unknown query
//  22  command refused (the refusal rode the wire as data; a scripted
//       golden fails loudly at EOF when the LAST command was refused)
//  23  identity drift (--verify-journal: the replay is not the record)
//  24  engine operation failed (save/query/journal IO)
//  25  session construction failed (bad paths, unloadable data, usage)

#include <f4/campaign/api/command_journal.hpp>
#include <f4/campaign/api/journal.hpp>
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
    std::filesystem::path journal;        // --journal (record)
    std::filesystem::path verify_journal; // --verify-journal (replay)
    std::filesystem::path command_journal;   // --command-journal (record)
    std::filesystem::path replay_commands;   // --replay-commands (replay)

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
          "  --max-steps-per-advance N (the dilation cap, default 240)\n"
          "  --journal PATH           record the event stream (JSONL)\n"
          "  --verify-journal PATH    replay-assert against a golden (exit 23 on drift)\n"
          "  --command-journal PATH   record applied commands + their ticks (JSONL)\n"
          "  --replay-commands PATH   re-apply a command journal at its recorded\n"
          "                           ticks; the final identity must equal the\n"
          "                           journal's footer (exit 23 on drift)\n";
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
        } else if (k == "--journal") {
            a.journal = next();
        } else if (k == "--verify-journal") {
            a.verify_journal = next();
        } else if (k == "--command-journal") {
            a.command_journal = next();
        } else if (k == "--replay-commands") {
            a.replay_commands = next();
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
    if (!args.journal.empty() && !args.verify_journal.empty()) {
        std::cerr << "campaignd: --journal and --verify-journal are "
                     "mutually exclusive\n";
        return 25;
    }
    if (!args.command_journal.empty() && !args.replay_commands.empty()) {
        std::cerr << "campaignd: --command-journal and --replay-commands "
                     "are mutually exclusive\n";
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

    // CAMP-HOST-2: the journal — the engine-rate, COMPLETE record (the
    // wire's subscription filter never touches it). The header rides
    // the session-start identity (hello's), the footer the session-end
    // one; the verifier mode compares every line byte-for-byte and
    // exits 23 at the first divergence (identity drift, plan §5).
    api::EventJournalWriter journal;
    api::EventJournalVerifier verifier;
    if (!args.journal.empty() || !args.verify_journal.empty()) {
        const auto id = host->identity();
        const bool writer_mode = !args.journal.empty();
        std::string jerr;
        if (writer_mode) {
            if (!journal.open(args.journal.string(), id, &jerr)) {
                std::cerr << "campaignd: journal open failed: " << jerr
                          << "\n";
                return 24;
            }
        } else {
            if (!verifier.open(args.verify_journal.string(), id, &jerr)) {
                std::cerr << "campaignd: identity drift (" << jerr
                          << ")\n";
                return 23;
            }
        }
        // The sink: append (record) or assert (replay). A verifier hit
        // aborts the process with 23 — the drift IS the run's verdict.
        (void)host->add_event_sink(
            [&journal, &verifier, writer_mode](
                const api::CampaignEvent& e) {
                if (writer_mode) {
                    journal.append(e);
                    return;
                }
                std::string jerr;
                if (!verifier.expect(e, &jerr)) {
                    std::cerr << "campaignd: identity drift (" << jerr
                              << ")\n";
                    std::exit(23);
                }
            });
    }

    // CAMP-CMD-1: the command journal — record (every APPLIED command
    // journals with its tick) or replay (the journal's commands re-apply
    // at their recorded ticks through step()'s segmentation; wire
    // commands refuse while a replay is active). The footer rides along
    // in replay mode: at EOF the session's final identity must equal it.
    api::CommandJournalWriter cmd_journal;
    api::IdentityFingerprint cmd_footer;
    if (!args.command_journal.empty()) {
        std::string jerr;
        if (!cmd_journal.open(args.command_journal.string(),
                              host->identity(), &jerr)) {
            std::cerr << "campaignd: command journal open failed: " << jerr
                      << "\n";
            return 24;
        }
        host->set_command_journal_sink(
            [&cmd_journal](std::uint64_t tick, std::int64_t t_s,
                           const api::CommandIntent& intent) {
                cmd_journal.append(
                    api::CommandJournalEntry{tick, t_s, intent});
            });
    } else if (!args.replay_commands.empty()) {
        std::vector<api::CommandJournalEntry> entries;
        std::string jerr;
        if (!api::CommandJournalReader::load(args.replay_commands.string(),
                                             host->identity(), entries,
                                             cmd_footer, &jerr)) {
            std::cerr << "campaignd: command journal load failed: " << jerr
                      << "\n";
            // header drift is identity drift (23); a malformed or
            // truncated file is operator error (24)
            return jerr.find("not this session's identity") !=
                           std::string::npos
                       ? 23
                       : 24;
        }
        std::string rerr;
        if (!host->start_command_replay(std::move(entries), &rerr)) {
            std::cerr << "campaignd: command replay refused: " << rerr
                      << "\n";
            return 24;
        }
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

    // EOF verdicts, loudest first (the plan §7 order: drift outranks a
    // refusal — 23 before 22). The command replay's verdict: no journal
    // command may remain past the run's last tick, and the final
    // identity must equal the journal's footer — "replay-with-commands
    // reproduces the books" as a byte comparison.
    if (!args.replay_commands.empty()) {
        if (const auto pending = host->pending_replay_commands();
            pending > 0) {
            std::cerr << "campaignd: identity drift ("
                      << pending
                      << " journal command(s) past the replay's last "
                         "tick)\n";
            return 23;
        }
        const auto id = host->identity();
        if (id.protocol_version != cmd_footer.protocol_version ||
            id.campaign_time_s != cmd_footer.campaign_time_s ||
            id.ledger_fnv != cmd_footer.ledger_fnv) {
            std::cerr << "campaignd: identity drift (replay books are "
                         "not the record: want ledger_fnv "
                      << cmd_footer.ledger_fnv << " @ t="
                      << cmd_footer.campaign_time_s << ", got "
                      << id.ledger_fnv << " @ t=" << id.campaign_time_s
                      << ")\n";
            return 23;
        }
    }

    // EOF: close the event journal (the writer appends the session-end
    // identity; the verifier asserts the golden's).
    if (!args.journal.empty()) {
        std::string jerr;
        if (!journal.close(host->identity(), &jerr)) {
            std::cerr << "campaignd: journal close failed: " << jerr
                      << "\n";
            return 24;
        }
    } else if (!args.verify_journal.empty()) {
        std::string jerr;
        if (!verifier.close(host->identity(), &jerr)) {
            std::cerr << "campaignd: identity drift (" << jerr << ")\n";
            return 23;
        }
    }

    // EOF: close the command journal (the writer appends the session-end
    // identity — the artifact a later --replay-commands consumes).
    if (!args.command_journal.empty()) {
        std::string jerr;
        if (!cmd_journal.close(host->identity(), &jerr)) {
            std::cerr << "campaignd: command journal close failed: "
                      << jerr << "\n";
            return 24;
        }
    }

    // EOF: green unless the last command was refused (a scripted golden
    // fails loudly — the plan §7 note).
    return last_refused ? 22 : 0;
}
