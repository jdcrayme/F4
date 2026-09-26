// f4-simulation/src/ground_strike_harness.cpp
//
// GroundStrikeHarness — see ground_strike_harness.hpp for the design.
// Headless, deterministic, instrumented: the air-to-ground strike as an
// acceptance run.
//
// The structural template is WvrMergeHarness (wvr_merge_harness.cpp —
// itself the mirror of BvrInterceptHarness). The M5b deltas are the
// strike-world injection (inject_strike_world_ — the composed objective,
// the bomb station, the StrikeModule + mission-plan binding; the
// scenario-schema tranche will move these into the scenario JSON), the
// A/G event counters (BombReleased/BombImpact — events 9/10, which the
// A/A harnesses explicitly do not gate on), the objective damage ledger
// cross-check (objective_damage_summary — the event says what happened,
// the ledger says what stuck), and the A/G verdict ladder (release →
// on-target impact → feature damage).
//
// The MD5 + RSS helpers are copied verbatim from wvr_merge_harness.cpp
// (which copied them from bvr_intercept_harness.cpp, which copied them
// from campaign_war_harness.cpp — the F4 codebase prefers duplication
// over premature sharing; the copies stand until a tranche explicitly
// authorizes the extraction, and the certificate baselines stay frozen).

#include "harness_shared.hpp"
#include <f4/simulation/ground_strike_harness.hpp>

#include <f4/simulation/simulation.hpp>
#include <f4/recorder/flight_recorder.hpp>
#include <f4/recorder/combat_event.hpp>
#include <f4/weapons/bomb.hpp>
#include <f4/weapons/bomb_battery.hpp>
#include <f4/weapons/weapon_class_table.hpp>
#include <f4/weapons/weapon_store.hpp>
#include <f4/ai/brain_component.hpp>
#include <f4/ai/modules/strike_module.hpp>
#include <f4/entities/entity.hpp>
#include <f4/json/f4_json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>

#if defined(__linux__)
#include <unistd.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#endif

namespace f4::simulation {

namespace {
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
} // namespace

// ===========================================================================
// MD5 (RFC 1321) — copied verbatim from wvr_merge_harness.cpp.

/// The CombatEventKind values as plain ints (mirror combat_event.hpp —
/// the header keeps f4-recorder out of its include surface).
constexpr int kWvrEngaged        = 11;
constexpr int kWvrDisengaged     = 12;

/// The bomb terminal causes (weapons::bomb_end_cause_name's strings —
/// messages.hpp). Only "impact" is a strike; "expired" is a named
/// failure (a bomb that ran out of time-of-flight never threatened the
/// target).
constexpr const char* kEndCauseImpact  = "impact";
constexpr const char* kEndCauseExpired = "expired";

/// The doctrine stick cap (campaign_bridge.cpp kDoctrineSalvoMax — a QC
/// run wants a representative stick, not the whole loadout).
constexpr int kDoctrineSalvoMax = 4;

/// The doctrine reference delivery speed (campaign_bridge.cpp's
/// bomb_drag_factor_for: 5,000 ft AGL, 675 fps — ~400 kts). The altitude
/// is route-specific here (the harness computes it from the delivery
/// waypoint over the target); the speed stays the doctrine reference.
constexpr double kDeliverySpeedFps = 675.0;

} // namespace

// ===========================================================================
// create / execute
// ===========================================================================

std::unique_ptr<GroundStrikeHarness>
GroundStrikeHarness::create(const GroundStrikeHarnessOptions& opts,
                            std::string* error) {
    const auto fail = [error](const std::string& msg) {
        if (error != nullptr) *error = msg;
        return nullptr;
    };
    if (opts.scenario_json.empty()) {
        return fail("ground strike harness: scenario_json is empty");
    }
    if (!std::filesystem::exists(opts.scenario_json)) {
        return fail("ground strike harness: scenario_json does not exist: " +
                    opts.scenario_json.string());
    }
    if (opts.horizon_sec <= 0) {
        return fail("ground strike harness: horizon_sec must be positive");
    }
    if (opts.sample_sec <= 0.0) {
        return fail("ground strike harness: sample_sec must be positive");
    }
    if (opts.runs < 1 || opts.runs > 8) {
        return fail("ground strike harness: runs must be 1..8");
    }
    if (opts.target_features < 0 || opts.target_features > 255) {
        return fail("ground strike harness: target_features must be 0..255");
    }
    if (opts.bomb_name.empty()) {
        return fail("ground strike harness: bomb_name is empty");
    }
    if (opts.bomb_rounds < 0) {
        return fail("ground strike harness: bomb_rounds must be >= 0");
    }
    auto harness =
        std::unique_ptr<GroundStrikeHarness>(new GroundStrikeHarness());
    harness->opts_ = opts;
    return harness;
}

GroundStrikeHarness::~GroundStrikeHarness() = default;

const GroundStrikeReport& GroundStrikeHarness::execute(ProgressFn on_sample) {
    report_ = {};

    // The M4 refusal order (wvr_merge_harness.cpp verbatim contract):
    // reject a non-combat scenario BEFORE any per-run load validation (a
    // non-combat scenario need not carry a valid aircraft list —
    // load-first would abort with the WRONG failure class). The abort
    // prefix is the QC tool's exit-2 contract; keep "scenario
    // combat.enabled is false" verbatim. Mechanically essential here
    // beyond family etiquette: the static bomb clock
    // (BombSimComponent::set_sim_time) and the spent-bomb sweep are both
    // gated on combat.enabled in Simulation::tick — bombs never fall
    // without it. The scan is best-effort over the raw JSON; parse
    // problems defer to run_pass_'s authoritative load error, and the
    // post-load check stays as defense in depth.
    if (std::string why;
        harness_shared::combat_refusal_reason(opts_.scenario_json, &why)) {
        report_.aborted = true;
        report_.abort_reason = why;
        finalize_();
        return report_;
    }

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
// The strike-world injection (the M5b composition contract)
// ===========================================================================

void GroundStrikeHarness::inject_strike_world_(int run) {
    auto& world = sim_->world();
    const auto& table = sim_->weapon_table();

    // --- 0. The bomb card -------------------------------------------------
    const std::uint32_t bomb_handle = table.find_by_name(opts_.bomb_name);
    if (bomb_handle == weapons::kInvalidWeapon) {
        report_.aborted = true;
        report_.abort_reason =
            "strike weapon '" + opts_.bomb_name +
            "' is not in the weapon class table (run " +
            std::to_string(run) + ") — the harness never flies a strike it "
            "cannot arm";
        return;
    }
    const auto* bomb_rec = table.get(bomb_handle);

    // --- 1. The objective (the injected strike target) --------------------
    // The same shape the world loader builds from campaign data
    // (Transform + FeatureSet + DamageBitmap + identity + team tag) —
    // test_bomb.cpp's synthetic objective, at scenario scale. The
    // features spread along +x centered on the objective; the impact
    // plane is the objective's z (the trigger's dz and the flyout's
    // terminal plane BOTH key on it, so the chain is self-consistent).
    {
        auto obj = world.create();
        auto& tf = obj.add<entities::TransformComponent>();
        tf.position = opts_.target_position;
        auto& fs = obj.add<entities::FeatureSetComponent>();
        const int n = std::clamp(opts_.target_features, 0, 255);
        fs.features_count = static_cast<std::uint8_t>(n);
        const double span =
            static_cast<double>(n > 0 ? n - 1 : 0) *
            opts_.target_feature_spacing_ft;
        for (int i = 0; i < n; ++i) {
            entities::FeatureEntryState f;
            f.name = opts_.target_name + " feature " + std::to_string(i);
            f.hit_points =
                static_cast<std::int16_t>(opts_.target_feature_hit_points);
            f.value = static_cast<std::uint8_t>(
                std::clamp(opts_.target_feature_value, 0, 100));
            f.offset_x = static_cast<float>(
                static_cast<double>(i) * opts_.target_feature_spacing_ft -
                span / 2.0);
            f.offset_y = 0.0f;
            fs.features.push_back(f);
        }
        auto& bitmap = obj.add<entities::DamageBitmapComponent>();
        bitmap.fstatus.assign(static_cast<std::size_t>((n + 3) / 4), 0);
        obj.add<entities::CampaignIdentityComponent>().callsign =
            opts_.target_name;
        obj.set_tag(entities::tags::TEAM,
                    entities::TagValue::from(std::string("red")));
        target_entity_id_ = obj.id().value;
    }

    // --- 2. The ordnance + the fire control (every blue aircraft) ---------
    // The additive station + the StrikeModule configuration + the
    // mission-plan binding — the same wiring the campaign bridge performs
    // for a saved strike flight (arm_flight_strike), exercised at
    // scenario scale. Identical every pass (determinism).
    striker_ids_.clear();
    delivery_dz_ft_ = 0.0;
    for (const auto id : sim_->aircraft_entities()) {
        const entities::EntityHandle h(id, &world);
        const auto team = h.get_tag(entities::tags::TEAM);
        if (!(team && team->as_string() && *team->as_string() == "blue")) {
            continue;
        }
        auto* brain = h.get<f4::ai::BrainComponent>();
        auto* store = h.get<weapons::WeaponStoreComponent>();
        if (brain == nullptr || store == nullptr) continue;

        // Bind the delivery waypoint: by name when the option names one,
        // else the first waypoint carrying an A/G delivery action (the
        // campwp.h values the brain's strike rung keys on).
        auto plan = brain->mission_plan();   // copy — mutated + re-issued
        int idx = -1;
        if (!opts_.delivery_waypoint.empty()) {
            for (std::size_t i = 0; i < plan.route.size(); ++i) {
                if (plan.route[i].name == opts_.delivery_waypoint) {
                    idx = static_cast<int>(i);
                    break;
                }
            }
        }
        if (idx < 0) {
            for (std::size_t i = 0; i < plan.route.size(); ++i) {
                if (f4::ai::modules::is_ag_delivery_action(
                        plan.route[i].action)) {
                    idx = static_cast<int>(i);
                    break;
                }
            }
        }
        if (idx < 0) continue;   // this striker's route carries no delivery
        plan_route_z_ = plan.route[static_cast<std::size_t>(idx)].position.z;

        // The delivery dz (THIS striker's delivery altitude over the
        // target) drives the drag factor — the trigger's range model must
        // agree with the flyout's ballistic fall at the condition the
        // release actually happens at.
        const double delivery_dz =
            plan_route_z_ - opts_.target_position.z;
        delivery_dz_ft_ = delivery_dz;

        plan.route[static_cast<std::size_t>(idx)].target_id =
            target_entity_id_;
        brain->set_mission_plan(std::move(plan));

        // The store is additive: the combat loadout's A/A stations stay.
        if (opts_.bomb_rounds > 0) {
            store->add_station(bomb_handle, opts_.bomb_rounds,
                               "strike (harness)");
        }

        // The fire control: drag factor from the card's own ballistics at
        // THIS delivery condition, doctrine stick, CCIP tolerance from
        // the lethal radius (the arm_flight_strike recipe).
        auto& strike = brain->strike();
        strike.config.drag_factor =
            strike_drag_factor_(bomb_handle, delivery_dz);
        strike.config.salvo_max =
            std::min(std::max(opts_.bomb_rounds, 1), kDoctrineSalvoMax);
        if (bomb_rec != nullptr) {
            strike.config.impact_tolerance_ft =
                std::max(50.0, 0.5 * bomb_rec->lethal_radius_ft);
        }

        striker_ids_.push_back(id.value);
    }

    if (striker_ids_.empty()) {
        report_.aborted = true;
        report_.abort_reason =
            "no blue aircraft could be armed (run " + std::to_string(run) +
            ") — the injection found no blue scenario aircraft with a "
            "brain + store + an A/G delivery waypoint on its route";
        return;
    }

    if (run == 0) {
        report_.strikers_armed = static_cast<int>(striker_ids_.size());
        report_.target_entity_id = target_entity_id_;
        report_.target_features = opts_.target_features;
        report_.verdict.target_entity_id = target_entity_id_;
        report_.verdict.strikers_armed = report_.strikers_armed;
    }
}

// ===========================================================================
// One full pass
// ===========================================================================

void GroundStrikeHarness::run_pass_(int run, const ProgressFn& on_sample) {
    Scenario scenario;
    try {
        scenario = load_scenario(opts_.scenario_json);
    } catch (const std::exception& e) {
        report_.aborted = true;
        report_.abort_reason = "scenario load failed (run " +
                               std::to_string(run) + "): " + e.what();
        return;
    }
    if (!scenario.combat.enabled) {
        report_.aborted = true;
        report_.abort_reason =
            "scenario combat.enabled is false (run " +
            std::to_string(run) +
            ") — the harness refuses to run a non-combat scenario "
            "(silent success would be the worst failure class)";
        return;
    }
    // Force recording: the recorder is the certificate AND the bomb
    // events' source (the combat bridge writes them only when the
    // recorder exists).
    scenario.record = true;
    scenario.record_every = 1;
    scenario.record_path.clear();

    if (run == 0) {
        report_.combat_enabled = true;
        report_.aircraft_count = static_cast<int>(scenario.aircraft.size());
        for (const auto& ac : scenario.aircraft) {
            if (ac.team == "blue") ++report_.blue_aircraft;
            else if (ac.team == "red") ++report_.red_aircraft;
        }
    }

    const auto asset_dir = opts_.asset_dir.empty()
        ? opts_.scenario_json.parent_path()
        : opts_.asset_dir;

    auto sim = std::make_unique<Simulation>(std::move(scenario), asset_dir);
    try {
        sim->initialize();
    } catch (const std::exception& e) {
        report_.aborted = true;
        report_.abort_reason = "sim initialize failed (run " +
                               std::to_string(run) + "): " + e.what();
        return;
    }

    // Per-pass state reset — BEFORE the injection (the injection sets
    // the strike world's state; the counters feed the sample walk).
    pass_bombs_released_ = 0;
    pass_bombs_impacted_ = 0;
    pass_impacts_on_target_ = 0;
    pass_damage_events_ = 0;
    pass_features_destroyed_max_ = 0.0;
    pass_destroyed_pct_max_ = 0.0;
    pass_min_miss_ft_ = -1.0;
    target_entity_id_ = 0;
    striker_ids_.clear();
    delivery_dz_ft_ = 0.0;
    plan_route_z_ = 0.0;

    // The strike-world injection — BEFORE the first tick (the brain has
    // not consumed the mission plan yet; the injected ids are
    // deterministic because the spawn sequence is).
    sim_ = sim.get();
    inject_strike_world_(run);
    if (report_.aborted) {
        sim_ = nullptr;
        return;
    }

    sim->set_paused(false);

    // Baseline (after the injection, before the first tick): the roster
    // identity's left side includes the injected objective.
    pass_t0_ = sim_->sim_time_s();
    pass_initial_entities_ = static_cast<int>(sim_->world().size());
    pass_spawned_ = 0;
    pass_retired_ = 0;
    pass_samples_ = 0;
    pass_next_sample_t_ = pass_t0_ + opts_.sample_sec;
    pass_first_sample_ = true;
    pass_prev_ = GroundStrikeSample{};
    pass_prev_.sim_time_s = pass_t0_;
    pass_sample_wall_ = std::chrono::steady_clock::now();

    const double sim_dt = sim_->scenario().sim_dt > 0.0
        ? sim_->scenario().sim_dt
        : (1.0 / 60.0);
    const double target =
        pass_t0_ + static_cast<double>(opts_.horizon_sec);
    const auto pass_wall_start = std::chrono::steady_clock::now();
    int stalled_advances = 0;

    // The loop: 4-sim-second tick batches (the C5/M4/M5a drain
    // discipline).
    while (sim_->sim_time_s() < target) {
        const double before = sim_->sim_time_s();
        const double batch_target = std::min(before + 4.0, target);
        int steps = 0;
        while (sim_->sim_time_s() < batch_target && steps < 240) {
            sim_->tick(sim_dt);
            ++steps;
        }
        if (sim_->sim_time_s() <= before + 1e-9) {
            ++stalled_advances;
            if (stalled_advances > 64) {
                report_.aborted = true;
                report_.abort_reason =
                    "sim clock stopped advancing (sim " +
                    std::to_string(sim_->sim_time_s()) +
                    "s, run " + std::to_string(run) + ")";
                sim_ = nullptr;
                return;
            }
        } else {
            stalled_advances = 0;
        }

        while (sim_->sim_time_s() >= pass_next_sample_t_ &&
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
                    std::to_string(sim_->sim_time_s()) +
                    "s, run " + std::to_string(run) + ")";
                sim_ = nullptr;
                return;
            }
        }
    }

    // The pass's recorder bytes + MD5. Run 0 keeps the document; every
    // run re-derives the digest, and run 1+ compares the BYTES.
    const auto* rec = sim_->recorder();
    if (run == 0 && target_entity_id_ != 0) {
        // The objective's ledger at run end (what stuck, vs the events).
        // Written straight into the report — the verdict struct is
        // run-0-owned, never reset per pass (the pass counters are).
        const auto ledger = weapons::objective_damage_summary(
            sim_->world(), target_entity_id_);
        report_.verdict.features_destroyed_final =
            static_cast<double>(ledger.features_destroyed_total);
        report_.verdict.destroyed_pct_final = ledger.destroyed_pct;
    }
    const auto json = (rec != nullptr) ? rec->to_json("ground_strike")
                                       : std::string{};
    const auto md5 = md5_hex(json);
    if (run == 0) {
        report_.recorder_json = json;
        report_.verdict.recorder_md5_run0 = md5;
        if (rec != nullptr) {
            run0_events_.clear();
            run0_events_.reserve(rec->combat_events().size());
            for (const auto& e : rec->combat_events()) {
                EventRow r;
                r.tick = e.tick;
                r.sim_time_s = e.sim_time_s;
                r.kind = static_cast<int>(e.kind);
                r.subject_id = e.subject_id;
                r.object_id = e.object_id;
                r.missile_id = e.missile_id;
                r.end_cause = e.end_cause;
                r.weapon_name = e.weapon_name;
                r.damage = e.damage;
                r.hit_points_after = e.hit_points_after;
                r.miss_distance_ft = e.miss_distance_ft;
                run0_events_.push_back(std::move(r));
            }
        }
        report_.bombs_released = pass_bombs_released_;
        report_.bombs_impacted = pass_bombs_impacted_;
        report_.samples = pass_samples_;
    } else {
        report_.verdict.recorder_md5_run1 = md5;
        if (json != report_.recorder_json) {
            report_.verdict.deterministic = false;
        }
    }

    sim_ = nullptr;
}

// ===========================================================================
// Sample collection + per-sample gates
// ===========================================================================

void GroundStrikeHarness::sample_(int run) {
    GroundStrikeSample s;
    s.sample = pass_samples_ + 1;
    s.sim_time_s = sim_->sim_time_s();
    s.initial_entities = pass_initial_entities_;
    s.live_entities = static_cast<int>(sim_->world().size());
    s.live_bombs = static_cast<int>(weapons::count_live_bombs(sim_->world()));

    // Refresh cumulative combat-event counters from the recorder.
    const auto* rec = sim_->recorder();
    int released = 0, impacted = 0, on_target = 0, dmg_events = 0;
    double feat_max = 0.0, pct_max = 0.0, min_miss = -1.0;
    if (rec != nullptr) {
        for (const auto& e : rec->combat_events()) {
            switch (static_cast<int>(e.kind)) {
                case kBombReleased:
                    ++released;
                    break;
                case kBombImpact: {
                    ++impacted;
                    const bool on_tgt =
                        e.end_cause == kEndCauseImpact &&
                        e.object_id == target_entity_id_;
                    if (on_tgt) {
                        ++on_target;
                        if (e.damage > 0.0) ++dmg_events;
                        feat_max = std::max(feat_max, e.damage);
                        pct_max = std::max(pct_max, e.hit_points_after);
                        if (min_miss < 0.0 || e.miss_distance_ft < min_miss) {
                            min_miss = e.miss_distance_ft;
                        }
                    }
                    break;
                }
                default: break;  // the A/A events are not this harness's gates
            }
        }
    }
    s.bombs_released = released;
    s.bombs_impacted = impacted;
    s.impacts_on_target = on_target;
    s.damage_events = dmg_events;
    s.features_destroyed_max = feat_max;
    s.destroyed_pct_max = pct_max;
    s.min_miss_distance_ft = min_miss;

    // The objective's own ledger (what stuck, vs the events above).
    if (target_entity_id_ != 0) {
        const auto ledger = weapons::objective_damage_summary(
            sim_->world(), target_entity_id_);
        s.ledger_features_destroyed = ledger.features_destroyed_total;
        s.ledger_destroyed_pct = ledger.destroyed_pct;
    }

    // Spawned/retired deltas from the previous sample (the M4 identity
    // walk, unchanged — bombs spawn at release, sweep at terminal state).
    if (pass_first_sample_) {
        s.spawned_entities = s.live_entities - s.initial_entities;
        s.retired_entities = 0;
    } else {
        const int live_delta = s.live_entities - pass_prev_.live_entities;
        if (live_delta > 0) {
            s.spawned_entities = pass_prev_.spawned_entities + live_delta;
            s.retired_entities = pass_prev_.retired_entities;
        } else if (live_delta < 0) {
            s.spawned_entities = pass_prev_.spawned_entities;
            s.retired_entities = pass_prev_.retired_entities + (-live_delta);
        } else {
            s.spawned_entities = pass_prev_.spawned_entities;
            s.retired_entities = pass_prev_.retired_entities;
        }
    }

    // Per-sample pulse.
    s.sample_releases = s.bombs_released - pass_prev_.bombs_released;
    s.sample_impacts = s.bombs_impacted - pass_prev_.bombs_impacted;

    // Telemetry (diary only; never a verdict).
    const auto now = std::chrono::steady_clock::now();
    const std::chrono::duration<double> wall = now - pass_sample_wall_;
    s.wall_sec = wall.count();
    s.ticks_per_sec = (s.sim_time_s - pass_prev_.sim_time_s) /
                      (s.wall_sec > 0.0 ? s.wall_sec : 1.0);
    s.rss_kb = current_rss_kb();
    pass_sample_wall_ = now;

    if (run == 0) {
        report_.diary.push_back(s);
        check_sample_(s);
    }

    pass_bombs_released_ = released;
    pass_bombs_impacted_ = impacted;
    pass_impacts_on_target_ = on_target;
    pass_damage_events_ = dmg_events;
    pass_features_destroyed_max_ = feat_max;
    pass_destroyed_pct_max_ = pct_max;
    pass_min_miss_ft_ = min_miss;

    pass_prev_ = s;
    pass_first_sample_ = false;
    ++pass_samples_;
}

void GroundStrikeHarness::check_sample_(const GroundStrikeSample& s) {
    // --- ROSTER BOUNDED (the M4 identity) -------------------------------
    const int expected = s.initial_entities + s.spawned_entities -
                         s.retired_entities;
    if (s.live_entities != expected && report_.verdict.roster_bounded) {
        report_.verdict.roster_bounded = false;
        report_.verdict.roster_leak =
            "sample " + std::to_string(s.sample) +
            " (sim " + std::to_string(s.sim_time_s) + "s): live=" +
            std::to_string(s.live_entities) + " != initial=" +
            std::to_string(s.initial_entities) + " + spawned=" +
            std::to_string(s.spawned_entities) + " - retired=" +
            std::to_string(s.retired_entities) + " (expected " +
            std::to_string(expected) + ")";
    }
}

// ===========================================================================
// The vacuum→dragged range scale (the scenario-scale mirror of
// campaign_bridge.cpp's bomb_drag_factor_for — parameterized delivery
// altitude, the same ODE the bomb entity flies, so the trigger and the
// flyout agree on where the bomb lands).
// ===========================================================================

double GroundStrikeHarness::strike_drag_factor_(std::uint32_t bomb_handle,
                                                double dz_ft) const {
    const auto* rec = sim_->weapon_table().get(bomb_handle);
    if (rec == nullptr) return 0.85;   // safe default

    const double alt_ft = std::max(1000.0, dz_ft);
    constexpr double kSpeedFps = kDeliverySpeedFps;
    constexpr double kDt = 0.1;        // coarse: the ratio is smooth

    weapons::Bomb dragged;
    dragged.release(weapons::BombConfig::from_record(*rec),
                    f4::geo::WorldPosition{0.0, 0.0, alt_ft},
                    f4::math::Vec3<double>{kSpeedFps, 0.0, 0.0}, 0.0);
    weapons::BombConfig vac = weapons::BombConfig::from_record(*rec);
    vac.cd = 0.0;
    vac.ref_area_ft2 = 0.0;
    weapons::Bomb vacuum;
    vacuum.release(vac,
                    f4::geo::WorldPosition{0.0, 0.0, alt_ft},
                    f4::math::Vec3<double>{kSpeedFps, 0.0, 0.0}, 0.0);
    auto fly = [](weapons::Bomb& b) {
        double t = 0.0;
        while (!b.terminal() && t < 120.0) { b.tick(kDt); t += kDt; }
    };
    fly(dragged);
    fly(vacuum);
    if (dragged.status() != weapons::BombStatus::Impact ||
        vacuum.status() != weapons::BombStatus::Impact ||
        vacuum.ground_range_ft() < 1.0) {
        return 0.85;
    }
    const double factor = dragged.ground_range_ft() /
                          vacuum.ground_range_ft();
    // Guard pathological cards (drag > vacuum never happens, but a card
    // with silly fields could invert the ratio).
    return std::clamp(factor, 0.3, 1.0);
}

// ===========================================================================
// Final verdict derivation
// ===========================================================================

void GroundStrikeHarness::finalize_() {
    if (report_.aborted) return;

    // --- The strike window (run 0's events) -----------------------------
    bool release_attributed = false;
    double first_release_s = -1.0;
    double first_impact_s = -1.0;
    double first_on_target_s = -1.0;
    double first_damage_s = -1.0;
    int bombs_released = 0;
    int bombs_impacted = 0;
    int impacts_on_target = 0;
    int bomb_misses = 0;
    int damage_events = 0;
    double feat_max = 0.0;
    double pct_max = 0.0;
    double min_miss = -1.0;

    for (const auto& e : run0_events_) {
        switch (e.kind) {
            case kBombReleased:
                ++bombs_released;
                if (first_release_s < 0.0) first_release_s = e.sim_time_s;
                if (std::find(striker_ids_.begin(), striker_ids_.end(),
                              e.subject_id) != striker_ids_.end()) {
                    release_attributed = true;
                }
                break;
            case kBombImpact: {
                ++bombs_impacted;
                if (first_impact_s < 0.0) first_impact_s = e.sim_time_s;
                const bool on_tgt = e.end_cause == kEndCauseImpact &&
                                    e.object_id == target_entity_id_;
                if (on_tgt) {
                    ++impacts_on_target;
                    if (first_on_target_s < 0.0)
                        first_on_target_s = e.sim_time_s;
                    if (min_miss < 0.0 || e.miss_distance_ft < min_miss) {
                        min_miss = e.miss_distance_ft;
                    }
                    if (e.damage > 0.0) {
                        ++damage_events;
                        if (first_damage_s < 0.0)
                            first_damage_s = e.sim_time_s;
                    }
                    feat_max = std::max(feat_max, e.damage);
                    pct_max = std::max(pct_max, e.hit_points_after);
                } else {
                    ++bomb_misses;
                }
                break;
            }
            default: break;
        }
    }

    // --- The verdicts ----------------------------------------------------
    report_.verdict.release_occurred = release_attributed;
    report_.verdict.impact_on_target = impacts_on_target > 0;
    report_.verdict.damage_applied = damage_events > 0;

    // --- The failure rungs (named, never bare booleans) -------------------
    if (!release_attributed) {
        if (striker_ids_.empty()) {
            report_.verdict.release_stall =
                "no striker was armed — the injection found no blue "
                "aircraft with a brain + store + an A/G delivery waypoint";
        } else if (bombs_released == 0) {
            report_.verdict.release_stall =
                "no BombReleased event — the chain stopped at the trigger "
                "rung (an empty store, hold_fire, the CCIP gate never "
                "satisfied, or the delivery waypoint never current; "
                "strikers_armed=" +
                std::to_string(striker_ids_.size()) + ")";
        } else {
            report_.verdict.release_stall =
                "a BombReleased exists but its shooter is not an armed "
                "striker (" + std::to_string(bombs_released) +
                " releases) — a harness bug or an unmapped release path";
        }
    } else if (impacts_on_target == 0) {
        if (bombs_impacted == 0) {
            report_.verdict.impact_failure =
                "released but no BombImpact within horizon — the flyout "
                "never terminated (the ToF limit, a horizon too short, or "
                "the bomb entity never ticked; releases=" +
                std::to_string(bombs_released) + ")";
        } else {
            report_.verdict.impact_failure =
                "bombs fell but none hit the objective (impacts=" +
                std::to_string(bombs_impacted) + ", on-target=0, misses=" +
                std::to_string(bomb_misses) + ") — a flyout/geometry "
                "disagreement between the trigger's drag model and the "
                "ballistic fall, or the impact plane moved";
        }
    } else if (damage_events == 0) {
        report_.verdict.damage_failure =
            "on-target impacts destroyed no features (impacts_on_target=" +
            std::to_string(impacts_on_target) + ", min_miss_distance_ft=" +
            std::to_string(min_miss) + ") — the blast radius vs the "
            "feature placements, or warhead power vs feature hit points";
    }

    // --- The window + counters ------------------------------------------
    report_.verdict.first_release_s = first_release_s;
    report_.verdict.first_impact_s = first_impact_s;
    report_.verdict.first_on_target_s = first_on_target_s;
    report_.verdict.first_damage_s = first_damage_s;
    report_.verdict.bombs_released = bombs_released;
    report_.verdict.bombs_impacted = bombs_impacted;
    report_.verdict.impacts_on_target = impacts_on_target;
    report_.verdict.bomb_misses = bomb_misses;
    report_.verdict.min_miss_distance_ft = min_miss;
    report_.verdict.features_destroyed_max = feat_max;
    report_.verdict.destroyed_pct_max = pct_max;
    // features_destroyed_final / destroyed_pct_final were captured at
    // run 0's end (run_pass_) — the run-0-owned ledger read-back.

    // --- DETERMINISTIC (set in run_pass_; runs == 1 vacuously true) -----
}

} // namespace f4::simulation
