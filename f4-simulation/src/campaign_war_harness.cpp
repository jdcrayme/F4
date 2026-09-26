// f4-simulation/src/campaign_war_harness.cpp
//
// CampaignWarHarness — see campaign_war_harness.hpp for the design.
// Headless, deterministic, instrumented: the 24-hour war as an
// acceptance run.

#include <f4/simulation/campaign_war_harness.hpp>

#include "harness_shared.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>

#if defined(__linux__)
#include <unistd.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#endif

namespace f4::simulation {

namespace {

// The digest, RSS telemetry, and combat-event kinds are single-sourced in
// harness_shared.hpp (this file's copies were the four-way duplication).
using harness_shared::md5_hex;
using harness_shared::current_rss_kb;
using harness_shared::kTrackAcquired;
using harness_shared::kTrackDropped;
using harness_shared::kRwrLock;
using harness_shared::kRwrLaunch;
using harness_shared::kMissileLaunched;
using harness_shared::kMissileDetonated;
using harness_shared::kDamageApplied;
using harness_shared::kEntityKilled;
using harness_shared::kGunFired;
using harness_shared::kBombReleased;
using harness_shared::kBombImpact;
using harness_shared::kEndCauseTargetHit;
/// True when `slot` appears in `war` (find semantics, tiny vectors).
bool listed(const std::vector<int>& war, int slot) {
    return std::find(war.begin(), war.end(), slot) != war.end();
}

} // namespace

// ===========================================================================
// create / execute
// ===========================================================================

std::unique_ptr<CampaignWarHarness>
CampaignWarHarness::create(const WarHarnessOptions& opts, std::string* error) {
    const auto fail = [error](const std::string& msg) {
        if (error != nullptr) *error = msg;
        return nullptr;
    };
    if (opts.horizon_sec <= 0) {
        return fail("war harness: horizon_sec must be positive");
    }
    if (opts.sample_sec <= 0.0) {
        return fail("war harness: sample_sec must be positive");
    }
    if (opts.runs < 1 || opts.runs > 8) {
        return fail("war harness: runs must be 1..8");
    }
    if (opts.speed < 1.0) {
        return fail("war harness: speed must be >= 1.0 (the acceleration "
                    "preset the run certifies; 1.0 = ungated telemetry)");
    }
    if (opts.dilation_tolerance < 0.0 || opts.dilation_tolerance >= 1.0) {
        return fail("war harness: dilation_tolerance must be in [0, 1)");
    }
    if (opts.max_deagg_aircraft < 0) {
        return fail("war harness: max_deagg_aircraft must be >= 0 "
                    "(0 = the Tier-B ceiling is not gated)");
    }
    if (opts.session.world_json.empty()) {
        return fail("war harness: session world_json is empty");
    }
    auto harness =
        std::unique_ptr<CampaignWarHarness>(new CampaignWarHarness());
    harness->opts_ = opts;
    return harness;
}

CampaignWarHarness::~CampaignWarHarness() = default;

const WarReport& CampaignWarHarness::execute(ProgressFn on_sample) {
    report_ = {};
    const auto wall_start = std::chrono::steady_clock::now();
    for (int run = 0; run < opts_.runs && !report_.aborted; ++run) {
        run_pass_(run, on_sample);
        if (opts_.max_wall_sec_total > 0.0) {
            const std::chrono::duration<double> spent =
                std::chrono::steady_clock::now() - wall_start;
            if (spent.count() > opts_.max_wall_sec_total) {
                report_.aborted = true;
                report_.abort_reason =
                    "wall-clock watchdog exceeded after run " +
                    std::to_string(run) + " (" +
                    std::to_string(spent.count()) + "s > " +
                    std::to_string(opts_.max_wall_sec_total) + "s)";
                break;
            }
        }
    }
    finalize_();
    return report_;
}

// ===========================================================================
// One full pass
// ===========================================================================

void CampaignWarHarness::run_pass_(int run, const ProgressFn& on_sample) {
    // The harness forces its own wreck policy: a 24-hour acceptance
    // run without the reaper is a leak-acceptance run (the exact
    // failure class C5 exists to catch).
    CampaignSessionOptions sopts = opts_.session;
    sopts.wreck_hold_sec = opts_.wreck_hold_sec;

    std::string err;
    auto session = CampaignSession::create(sopts, &err);
    if (session == nullptr) {
        report_.aborted = true;
        report_.abort_reason = "session create failed (run " +
                               std::to_string(run) + "): " + err;
        return;
    }
    session->set_paused(false);
    session_ = session.get();

    // Baseline (before the first tick): the roster identity's left
    // side, the belligerents, and per-team pool state.
    const auto& s0 = session_->stats();
    pass_t0_ = s0.sim_time_s;
    pass_roster0_ = s0.live_aircraft;
    pass_belligerents_ = session_->campaign().belligerent_teams();
    pass_expected_.clear();
    for (const auto& sq : session_->ledger().squadrons()) {
        if (sq.availability <= 0) continue;
        const auto slot = static_cast<int>(sq.owner);
        if (listed(pass_belligerents_, slot) &&
            !listed(pass_expected_, slot)) {
            pass_expected_.push_back(slot);
        }
    }
    if (run == 0) {
        report_.belligerent_air = !pass_expected_.empty();
    }

    pass_prev_teams_.clear();
    for (const auto& t : session_->ledger().teams()) {
        PrevTeam p;
        p.slot = t.slot;
        p.losses = t.losses;
        p.reinforced = t.reinforced;
        p.recovered = t.recovered;
        p.drawn_total = 0;  // no draws before the first tick
        p.tasking = session_->ledger().team_aircraft_tasking(t.slot);
        pass_prev_teams_.push_back(p);
    }

    pass_samples_ = 0;
    pass_next_sample_t_ = pass_t0_ + opts_.sample_sec;
    pass_first_sample_ = true;
    pass_prev_ = WarHourSample{};
    pass_prev_.sim_time_s = pass_t0_;
    pass_sample_wall_ = std::chrono::steady_clock::now();

    // The loop: 4-sim-second advance batches (the session's 240-tick
    // per-advance cap at 60 Hz, fully drained — byte-equivalent to any
    // other split of the same ticks, pinned by the C2 tests). The
    // loop keys on the SIM CLOCK, not the debt, so it self-corrects.
    const double target =
        pass_t0_ + static_cast<double>(opts_.horizon_sec);
    const auto pass_wall_start = std::chrono::steady_clock::now();
    int stalled_advances = 0;
    while (session_->stats().sim_time_s < target) {
        const double before = session_->stats().sim_time_s;
        session_->advance(4.0);
        if (session_->stats().sim_time_s <= before + 1e-9) {
            // The C5-FIX-1 class: the campaign clock froze. In-process
            // advance() only stalls when the drain itself broke — a
            // run of consecutive frozen batches is conclusive (never
            // merely slow: each batch owes 240 full ticks).
            ++stalled_advances;
            if (stalled_advances > 64) {
                report_.aborted = true;
                report_.abort_reason =
                    "campaign clock stopped advancing (sim " +
                    std::to_string(session_->stats().sim_time_s) +
                    "s, run " + std::to_string(run) + ")";
                session_ = nullptr;
                return;
            }
        } else {
            stalled_advances = 0;
        }

        while (session_->stats().sim_time_s >= pass_next_sample_t_ &&
               pass_next_sample_t_ <= target) {
            sample_(run);
            const double next = pass_next_sample_t_ + opts_.sample_sec;
            pass_next_sample_t_ = next;
            if (on_sample != nullptr && run == 0 && !report_.diary.empty()) {
                on_sample(report_.diary.back());
            }
        }

        if (opts_.max_wall_sec_total > 0.0) {
            const std::chrono::duration<double> spent =
                std::chrono::steady_clock::now() - pass_wall_start;
            if (spent.count() > opts_.max_wall_sec_total) {
                report_.aborted = true;
                report_.abort_reason =
                    "wall-clock watchdog exceeded mid-run (sim " +
                    std::to_string(session_->stats().sim_time_s) +
                    "s, run " + std::to_string(run) + ")";
                session_ = nullptr;
                return;
            }
        }
    }

    // FID-6: the pass-level acceleration numbers (run 0). The
    // sustained rate covers the WHOLE pass (every batch's wall, the
    // samples' windows included) — the per-sample gate can miss a
    // slow patch that never spans a sample boundary; this one cannot.
    // The peaks/dips are derived from the diary rows (already
    // collected with their per-sample flags).
    if (run == 0) {
        const std::chrono::duration<double> pass_wall =
            std::chrono::steady_clock::now() - pass_wall_start;
        const double sim_advanced =
            session_->stats().sim_time_s - pass_t0_;
        if (pass_wall.count() > 1e-9) {
            report_.verdict.sustained_rate =
                sim_advanced / pass_wall.count();
        }
        bool have_rate = false;
        for (const auto& s : report_.diary) {
            if (s.agg_live > report_.deagg_peak) {
                report_.deagg_peak = s.agg_live;
            }
            if (s.sim_rate > 0.0 &&
                (!have_rate || s.sim_rate < report_.verdict.min_sample_rate)) {
                report_.verdict.min_sample_rate = s.sim_rate;
                have_rate = true;
            }
        }
        // The end state counts too: the last sample's window does not
        // reach the horizon's end (the final partial window is
        // unsampled), so the ceiling's last chance to fire is here.
        const int end_live = session_->stats().agg_live;
        if (end_live > report_.deagg_peak) {
            report_.deagg_peak = end_live;
        }
        if (opts_.max_deagg_aircraft > 0 && end_live > opts_.max_deagg_aircraft &&
            report_.verdict.deagg_bounded) {
            report_.verdict.deagg_bounded = false;
            report_.verdict.deagg_report =
                "end state: deaggregated set " + std::to_string(end_live) +
                " > ceiling " + std::to_string(opts_.max_deagg_aircraft);
        }
        if (opts_.speed >= 2.0) {
            report_.verdict.rate_gated = true;
            const double required =
                opts_.speed * (1.0 - opts_.dilation_tolerance);
            if (report_.verdict.zero_dilation &&
                report_.verdict.sustained_rate < required &&
                report_.verdict.sustained_rate > 0.0) {
                report_.verdict.zero_dilation = false;
                report_.verdict.dilation_report =
                    "sustained rate " +
                    std::to_string(report_.verdict.sustained_rate) +
                    " < required " + std::to_string(required) +
                    " (preset " + std::to_string(opts_.speed) + ")";
            }
        }
    }

    // The pass's ledger bytes + MD5. Run 0 keeps the document; every
    // run re-derives the digest, and run 1+ compares the BYTES (the
    // strict form of the MD5 certificate — a digest collision cannot
    // pass).
    const auto json = session_->ledger_json();
    const auto md5 = md5_hex(json);
    if (run == 0) {
        report_.ledger_json = json;
        report_.verdict.ledger_md5_run0 = md5;
    } else {
        report_.verdict.ledger_md5_run1 = md5;
        if (json != report_.ledger_json) {
            report_.verdict.deterministic = false;
        }
    }

    if (run == 0) {
        // Headline counters + the final pool rows from the end state.
        const auto& st = session_->stats();
        const auto* atm = session_->campaign().atm_stats();
        report_.cycles = st.cycles;
        report_.intents = st.intents;
        report_.packages = atm != nullptr ? atm->packages_built : 0;
        report_.escorts = atm != nullptr ? atm->escorts_built : 0;
        report_.routes_built = session_->campaign().routes_built();
        report_.routes_failed = session_->campaign().routes_failed();
        report_.drawn = session_->ledger().mission_draw_aircraft();
        report_.air_losses = session_->ledger().air_losses();
        report_.recovered = session_->ledger().aircraft_recovered();
        report_.reinforced = session_->ledger().aircraft_reinforced();
        report_.reinforce_fires = session_->ledger().reinforcement_fires();
        report_.synthetic_spawned = st.synthetic_spawned;
        // FID-5: the tiered war's materialization evidence — the
        // generated missions the session registered as aggregates (the
        // spawner's own counter stays 0 under the deferral arm).
        report_.synthetic_aggregates = st.synthetic_aggregates;
        report_.retired = st.retired;
        report_.live_aircraft = st.live_aircraft;
        report_.airborne = st.airborne;
        report_.samples = pass_samples_;
        // C6: the campaign-combat counters — which war ran (armed?) and
        // what it produced (fighters fielded, A/A kills booked).
        report_.aa_combat = opts_.session.aa_combat;
        report_.armed_aircraft = st.armed_aircraft;
        report_.armed_fighters = st.armed_fighters;
        // FID: which fidelity ran + the tier counters (the accel
        // certificate's headline; the echo pattern of aa_combat).
        report_.fidelity_tiered =
            opts_.session.fidelity_policy == FidelityPolicy::Tiered;
        report_.agg_flights = st.agg_flights;
        report_.agg_live = st.agg_live;
        report_.agg_arrived = st.agg_arrived;
        report_.agg_destroyed = st.agg_destroyed;
        report_.tier_deaggs = st.tier_deaggs;
        report_.tier_reaggs = st.tier_reaggs;
        report_.speed = opts_.speed;
        report_.dilation_tolerance = opts_.dilation_tolerance;
        report_.max_deagg_aircraft = opts_.max_deagg_aircraft;
        // G1: the ground war's headline counters (the QC's exit 13 +
        // the summary's ground block read these; the LEDGER's ground
        // block — inside the byte-stable certificate — is the full
        // story).
        report_.ground_war = opts_.session.ground_war;
        // G2: the interdiction arm + its number (the ledger's own
        // counter — booked with the arm on, engine or not).
        report_.unit_strike = opts_.session.unit_strike;
        report_.ground_losses_air =
            session_->ledger().ground_vehicle_losses_air();
        report_.ground_updates = st.ground_updates;
        report_.ground_battalions = st.ground_battalions;
        report_.ground_mobile = st.ground_mobile;
        report_.ground_losses = st.ground_losses;
        report_.ground_destroyed = st.ground_destroyed;
        report_.ground_captures = st.ground_captures;
        report_.ground_front_columns = st.ground_front_columns;
        if (const auto* gw = session_->ground_war(); gw != nullptr) {
            report_.ground_march_grid = static_cast<int>(
                gw->stats().army_distance_fp >> 8);
            // CAMP-DOM-2: the supply chain's books (zeros when the
            // deepened flow never armed — the summary stays quiet).
            report_.ground_supply_regen = gw->stats().supply_regen_total;
            report_.ground_supply_drawn = gw->stats().supply_drawn_total;
            report_.ground_cut_off = gw->stats().cut_off_events;
            report_.ground_repairs = gw->stats().repair_fires;
            report_.ground_features_repaired =
                gw->stats().features_repaired;
        }
        // CAMP-DOM-6: the task-force movement's headline counters (the
        // QC's exit 18 + the summary's naval_move block read these).
        report_.naval_movement = opts_.session.naval_movement;
        if (const auto* nw = session_->naval_war(); nw != nullptr) {
            report_.naval_updates = nw->stats().updates;
            report_.naval_moved_events = nw->stats().moved_events;
            report_.naval_arrivals = nw->stats().arrivals;
            report_.naval_march_grid = static_cast<int>(
                nw->stats().fleet_distance_fp >> 8);
        }
        report_.ledger_teams.clear();
        for (const auto& t : session_->ledger().teams()) {
            WarHourSample::TeamPool p;
            p.slot = t.slot;
            p.name = t.name;
            p.initial = t.aircraft_initial;
            p.remaining = t.aircraft_remaining;
            p.tasking = session_->ledger().team_aircraft_tasking(t.slot);
            p.drawn = t.drawn;
            p.drawn_total = 0;
            for (const auto& d :
                 session_->ledger().mission_draw_log()) {
                if (static_cast<int>(d.team) == t.slot) {
                    p.drawn_total += d.count;
                }
            }
            p.losses = t.losses;
            p.reinforced = t.reinforced;
            p.recovered = t.recovered;
            report_.ledger_teams.push_back(std::move(p));
        }
    }
    session_ = nullptr;
}

// ===========================================================================
// One sample: collect, check, diary
// ===========================================================================

void CampaignWarHarness::sample_(int run) {
    const auto& st = session_->stats();
    const auto& ledger = session_->ledger();
    const auto& ladder = session_->campaign();

    WarHourSample s;
    s.sample = ++pass_samples_;
    s.campaign_time = session_->campaign_time();
    s.sim_time_s = st.sim_time_s;
    s.cycles = st.cycles;
    s.intents = st.intents;
    s.packages = st.packages;
    s.escorts = st.escorts;
    s.routes_built = ladder.routes_built();
    s.routes_failed = ladder.routes_failed();
    s.drawn = ledger.mission_draw_aircraft();
    s.air_losses = ledger.air_losses();
    s.recovered = ledger.aircraft_recovered();
    s.reinforced = ledger.aircraft_reinforced();
    s.reinforce_fires = ledger.reinforcement_fires();
    s.synthetic_spawned = st.synthetic_spawned;
    s.live_aircraft = st.live_aircraft;
    s.airborne = st.airborne;
    s.retired = st.retired;
    s.world_entities = static_cast<int>(session_->sim().world().size());
    // C6: the armed doctrine's pulse per sample (fighters fielded so far
    // + the A/A kill count — the war's air story in the diary).
    s.armed_fighters = st.armed_fighters;
    s.aa_kills = ledger.air_losses();
    // G1: the ground war's pulse per sample (battalions, losses,
    // captures, the front's shape — the diary's ground columns).
    s.ground_updates = st.ground_updates;
    s.ground_battalions = st.ground_battalions;
    s.ground_mobile = st.ground_mobile;
    s.ground_losses = st.ground_losses;
    s.ground_losses_air = st.ground_losses_air;
    s.ground_destroyed = st.ground_destroyed;
    s.ground_captures = st.ground_captures;
    s.ground_engaged = st.ground_engaged;
    s.ground_front_columns = st.ground_front_columns;
    if (const auto* gw = session_->ground_war(); gw != nullptr) {
        s.ground_march_grid = static_cast<int>(
            gw->stats().army_distance_fp >> 8);
    }
    // FID: the tier machinery's pulse per sample (the aggregate set,
    // the Tier-B set, the deagg/reagg counters — the diary's FID
    // columns; inert zeros in a full-fidelity war).
    s.agg_flights = st.agg_flights;
    s.agg_live = st.agg_live;
    s.agg_arrived = st.agg_arrived;
    s.agg_destroyed = st.agg_destroyed;
    s.tier_deaggs = st.tier_deaggs;
    s.tier_reaggs = st.tier_reaggs;
    s.hour_spawns = s.synthetic_spawned - pass_prev_.synthetic_spawned;
    s.hour_draws = s.drawn - pass_prev_.drawn;
    s.hour_cycles = s.cycles - pass_prev_.cycles;

    // Performance telemetry (run 0's diary rows only; never a verdict).
    if (run == 0) {
        const auto now = std::chrono::steady_clock::now();
        const std::chrono::duration<double> wall =
            now - pass_sample_wall_;
        s.wall_sec = wall.count();
        const double ticks = (s.sim_time_s - pass_prev_.sim_time_s) /
                             opts_.session.sim_dt;
        if (s.wall_sec > 1e-9) {
            s.ticks_per_sec = ticks / s.wall_sec;
        }
        s.rss_kb = current_rss_kb();
        pass_sample_wall_ = now;
        // FID-6: the sample's measured rate — sim seconds advanced per
        // wall second across this window — and, when the dilation gate
        // is armed (speed >= 2), its verdict. The first violation is
        // recorded with numbers; every violation counts.
        if (s.wall_sec > 1e-9) {
            s.sim_rate =
                (s.sim_time_s - pass_prev_.sim_time_s) / s.wall_sec;
        }
        if (opts_.speed >= 2.0) {
            report_.verdict.rate_gated = true;
            const double required =
                opts_.speed * (1.0 - opts_.dilation_tolerance);
            if (s.sim_rate < required) {
                s.dilated = true;
                ++report_.dilated_samples;
                if (report_.verdict.zero_dilation) {
                    report_.verdict.zero_dilation = false;
                    report_.verdict.dilation_report =
                        "sample " + std::to_string(s.sample) + " (sim " +
                        std::to_string(static_cast<long>(s.sim_time_s)) +
                        "s): rate " + std::to_string(s.sim_rate) +
                        " < required " + std::to_string(required) +
                        " (preset " + std::to_string(opts_.speed) + ")";
                }
            }
        }
    }

    // Per-team rows + the per-team book data (the squadron-side sums
    // and unmatched-flow counts the drift checks compare against;
    // both walks are over ledger-owned state, arrival-ordered and
    // bounded by the run's own activity).
    std::vector<TeamBooks> books;
    {
        // Squadron-side sums, per owning team.
        std::vector<std::pair<int, TeamBooks>> by_owner;
        for (const auto& sq : ledger.squadrons()) {
            const auto slot = static_cast<int>(sq.owner);
            bool found = false;
            for (auto& [s_slot, b] : by_owner) {
                if (s_slot == slot) {
                    b.run_draws += sq.run_draws;
                    b.run_losses += sq.run_losses;
                    b.run_reinforced += sq.run_reinforced;
                    b.run_recoveries += sq.run_recoveries;
                    found = true;
                    break;
                }
            }
            if (!found) {
                TeamBooks b;
                b.slot = slot;
                b.run_draws = sq.run_draws;
                b.run_losses = sq.run_losses;
                b.run_reinforced = sq.run_reinforced;
                b.run_recoveries = sq.run_recoveries;
                by_owner.emplace_back(slot, b);
            }
        }
        // Unmatched flows per team (team-side bookings with no
        // squadron): draw/recovery log entries with squadron == 0,
        // air losses with victim_squadron == 0. The team-vs-squadron
        // equalities are exact only for teams with none of these —
        // guarded below (the unmatched flows are already loud in the
        // ledger's own counters).
        for (const auto& d : ledger.mission_draw_log()) {
            if (d.squadron != 0) continue;
            for (auto& [slot, b] : by_owner) {
                if (slot == static_cast<int>(d.team)) {
                    b.unmatched_draws += d.count;
                    break;
                }
            }
        }
        for (const auto& r : ledger.mission_recovery_log()) {
            if (r.squadron != 0) continue;
            for (auto& [slot, b] : by_owner) {
                if (slot == static_cast<int>(r.team)) {
                    b.unmatched_recov += r.released;
                    break;
                }
            }
        }
        for (const auto& l : ledger.air_loss_log()) {
            if (l.victim_squadron != 0) continue;
            for (auto& [slot, b] : by_owner) {
                if (slot == static_cast<int>(l.victim_team)) {
                    b.unresolved_losses += 1;
                    break;
                }
            }
        }
        books.reserve(by_owner.size());
        for (auto& [slot, b] : by_owner) {
            books.push_back(b);
        }
    }

    std::vector<WarHourSample::TeamPool> rows;
    std::vector<PrevTeam> now_teams;
    for (const auto& t : ledger.teams()) {
        WarHourSample::TeamPool p;
        p.slot = t.slot;
        p.name = t.name;
        p.initial = t.aircraft_initial;
        p.remaining = t.aircraft_remaining;
        p.tasking = ledger.team_aircraft_tasking(t.slot);
        p.drawn = t.drawn;
        p.drawn_total = 0;
        for (const auto& d : ledger.mission_draw_log()) {
            if (static_cast<int>(d.team) == t.slot) p.drawn_total += d.count;
        }
        p.losses = t.losses;
        p.reinforced = t.reinforced;
        p.recovered = t.recovered;
        rows.push_back(p);

        PrevTeam now;
        now.slot = t.slot;
        now.losses = t.losses;
        now.reinforced = t.reinforced;
        now.recovered = t.recovered;
        now.drawn_total = p.drawn_total;
        now.tasking = p.tasking;
        now_teams.push_back(now);
    }
    s.teams = std::move(rows);

    if (run == 0) {
        check_sample_(s, books);
    }

    pass_prev_ = s;
    pass_prev_teams_ = std::move(now_teams);
    pass_first_sample_ = false;
    if (run == 0) {
        report_.diary.push_back(std::move(s));
    }
}

// ===========================================================================
// The three per-sample gates (run 0 only)
// ===========================================================================

void CampaignWarHarness::check_sample_(const WarHourSample& s,
                                        const std::vector<TeamBooks>& books) {
    const auto book_of = [&books](int slot) -> const TeamBooks* {
        for (const auto& b : books) {
            if (b.slot == slot) return &b;
        }
        return nullptr;
    };
    const auto prev_of = [this](int slot) -> const PrevTeam* {
        for (const auto& p : pass_prev_teams_) {
            if (p.slot == slot) return &p;
        }
        return nullptr;
    };

    // --- the LEDGER DRIFT gate: the one-pool identities ----------------
    const auto drift = [&](const std::string& what) {
        if (report_.verdict.ledger_consistent) {
            report_.verdict.ledger_consistent = false;
            report_.verdict.ledger_drift =
                "sample " + std::to_string(s.sample) + " (sim " +
                std::to_string(static_cast<long>(s.sim_time_s)) + "s): " +
                what;
        }
    };
    for (const auto& p : s.teams) {
        if (p.remaining < 0 || p.remaining > p.initial) {
            drift("team " + std::to_string(p.slot) + " remaining " +
                  std::to_string(p.remaining) + " outside [0, " +
                  std::to_string(p.initial) + "]");
        }
        if (p.tasking < 0 || p.tasking > p.remaining) {
            drift("team " + std::to_string(p.slot) + " tasking " +
                  std::to_string(p.tasking) + " outside [0, remaining " +
                  std::to_string(p.remaining) + "]");
        }
        if (const auto* b = book_of(p.slot); b != nullptr) {
            // Team books vs squadron books (guarded on unmatched flows —
            // those are team-side-only bookings by design).
            if (b->unmatched_draws == 0 && b->unmatched_recov == 0 &&
                p.drawn != b->run_draws) {
                drift("team " + std::to_string(p.slot) +
                      " outstanding draws " + std::to_string(p.drawn) +
                      " != squadron book " + std::to_string(b->run_draws));
            }
            if (b->unresolved_losses == 0 && p.losses != b->run_losses) {
                drift("team " + std::to_string(p.slot) + " losses " +
                      std::to_string(p.losses) + " != squadron book " +
                      std::to_string(b->run_losses));
            }
            if (p.reinforced != b->run_reinforced) {
                drift("team " + std::to_string(p.slot) + " reinforced " +
                      std::to_string(p.reinforced) + " != squadron book " +
                      std::to_string(b->run_reinforced));
            }
            if (b->unmatched_recov == 0 && p.recovered != b->run_recoveries) {
                drift("team " + std::to_string(p.slot) + " recovered " +
                      std::to_string(p.recovered) + " != squadron book " +
                      std::to_string(b->run_recoveries));
            }
        }
        // Monotone counters never go backwards (existence flows and
        // the log-derived totals; outstanding drawn is NOT monotone —
        // recoveries release it, by design).
        if (!pass_first_sample_) {
            if (const auto* prev = prev_of(p.slot); prev != nullptr) {
                if (p.losses < prev->losses ||
                    p.reinforced < prev->reinforced ||
                    p.recovered < prev->recovered ||
                    p.drawn_total < prev->drawn_total) {
                    drift("team " + std::to_string(p.slot) +
                          " monotone counter went backwards (losses " +
                          std::to_string(prev->losses) + "→" +
                          std::to_string(p.losses) + ", reinforced " +
                          std::to_string(prev->reinforced) + "→" +
                          std::to_string(p.reinforced) + ", recovered " +
                          std::to_string(prev->recovered) + "→" +
                          std::to_string(p.recovered) + ", drawn_total " +
                          std::to_string(prev->drawn_total) + "→" +
                          std::to_string(p.drawn_total) + ")");
                }
            }
        }
    }

    // --- the ENTITY LEAK gate: the roster identity ----------------------
    // FID-6: a tiered session materializes ONE aircraft per
    // deaggregation through the bridge + register_aircraft — NOT the
    // spawner's synthetic counter — and every reagg retires it via
    // retire_aircraft (the reaper's own mechanics; a kill's wreck
    // retires once, whichever path reaps it). The identity gains the
    // tier term; zero in full-fidelity mode (the pre-FID arithmetic,
    // unchanged).
    const int expected = pass_roster0_ + s.synthetic_spawned +
                         s.tier_deaggs - s.retired;
    if (s.live_aircraft != expected && report_.verdict.entities_bounded) {
        report_.verdict.entities_bounded = false;
        report_.verdict.entity_leak =
            "sample " + std::to_string(s.sample) + " (sim " +
            std::to_string(static_cast<long>(s.sim_time_s)) + "s): roster " +
            std::to_string(s.live_aircraft) + " != initial " +
            std::to_string(pass_roster0_) + " + spawned " +
            std::to_string(s.synthetic_spawned) +
            (s.tier_deaggs > 0
                 ? " + tier-deagg " + std::to_string(s.tier_deaggs)
                 : "") +
            " - retired " + std::to_string(s.retired) + " (slack " +
            std::to_string(s.live_aircraft - expected) + ")";
    }

    // --- the FID-6 DEAGG CEILING gate (armed by max_deagg_aircraft) -----
    // The tier machinery's bound: the deaggregated set (Tier-B flights
    // — each one a 60 Hz aircraft) stays within the certificate's
    // ceiling at every sample. The end state is checked post-loop
    // (the final partial window is unsampled).
    if (opts_.max_deagg_aircraft > 0 &&
        s.agg_live > opts_.max_deagg_aircraft &&
        report_.verdict.deagg_bounded) {
        report_.verdict.deagg_bounded = false;
        report_.verdict.deagg_report =
            "sample " + std::to_string(s.sample) + " (sim " +
            std::to_string(static_cast<long>(s.sim_time_s)) + "s): " +
            "deaggregated set " + std::to_string(s.agg_live) + " > ceiling " +
            std::to_string(opts_.max_deagg_aircraft);
    }

    // --- the WAR ALIVE gate ----------------------------------------------
    const auto stall = [&](const std::string& what) {
        if (report_.verdict.war_alive) {
            report_.verdict.war_alive = false;
            report_.verdict.war_stall =
                "sample " + std::to_string(s.sample) + " (sim " +
                std::to_string(static_cast<long>(s.sim_time_s)) + "s): " +
                what;
        }
    };
    // Cycles: a cycle period no longer than the sample cadence owes at
    // least one fire per sample — the campaign clock running, the
    // C5-FIX-1 regression pin at harness level.
    if (static_cast<double>(opts_.session.tasking_cycle_sec) <=
            opts_.sample_sec &&
        s.hour_cycles < 1) {
        stall("no tasking cycle fired this sample (period " +
              std::to_string(opts_.session.tasking_cycle_sec) + "s)");
    }
    // Belligerents that owe draws: a team that has EVER drawn this war
    // owes continuing draws while its pool is taskable (the mid-war
    // stall — a side flying for six hours and then going silent with
    // aircraft available is the exact C5 failure class). A team that
    // NEVER drew is not gated: whether the ladder can match anything
    // to its squadrons is fixture data (role mix, theater DB), and a
    // one-sided generation is VISIBLE in the diary's per-team
    // drawn_total rows rather than gated here — the false-stall is
    // worse than the missing gate.
    for (const auto& p : s.teams) {
        if (!listed(pass_belligerents_, p.slot)) continue;
        if (p.drawn_total <= 0) continue;  // never drew — not expected
        const auto* prev = prev_of(p.slot);
        const int prev_tasking = prev != nullptr ? prev->tasking : p.tasking;
        const int prev_drawn_total =
            prev != nullptr ? prev->drawn_total : p.drawn_total;
        const int hour_team_draws = p.drawn_total - prev_drawn_total;
        if (hour_team_draws <= 0 && p.tasking > 0 && prev_tasking > 0) {
            stall("belligerent team " + std::to_string(p.slot) + " (" +
                  p.name + ") drew nothing this sample with " +
                  std::to_string(p.tasking) + " aircraft taskable");
        }
    }
}

// ===========================================================================
// Final verdicts
// ===========================================================================

void CampaignWarHarness::finalize_() {
    report_.atm_armed = opts_.session.atm_pipeline;
    report_.verdict.drew_aircraft = report_.drawn > 0;
    report_.verdict.routes_built = report_.routes_built > 0;
    // FID-5: a TIERED war materializes its generated missions through
    // the tier machinery (ops windows / bubbles / combat), not the
    // spawner — the aggregate registrations are the evidence. The
    // full-fidelity war keeps the spawner's own counter (the pre-FID-5
    // arithmetic, unchanged).
    report_.verdict.materialized =
        report_.synthetic_spawned > 0 ||
        (report_.fidelity_tiered && report_.synthetic_aggregates > 0);
    // Packages gate only where the ATM pipeline was armed (the legacy
    // ladder builds intents, not packages — its coverage is the
    // routes/materialized gates).
    report_.verdict.packages_built =
        !report_.atm_armed || report_.packages > 0;
    // A war that never fired ONE cycle is not alive either — the
    // per-sample check catches mid-war silence; this catches the war
    // that never started (a cycle period beyond the horizon, a clock
    // that never crossed its first due time).
    if (report_.cycles == 0 && report_.verdict.war_alive) {
        report_.verdict.war_alive = false;
        report_.verdict.war_stall =
            "no tasking cycle fired the entire war (period " +
            std::to_string(opts_.session.tasking_cycle_sec) + "s over a " +
            std::to_string(opts_.horizon_sec) + "s horizon)";
    }
    if (report_.aborted) {
        // An aborted war certifies nothing: clear the digests so the
        // host cannot mistake a partial run's hash for a certificate.
        report_.verdict.ledger_md5_run0.clear();
        report_.verdict.ledger_md5_run1.clear();
    }
}

} // namespace f4::simulation
