// f4-simulation/include/f4/simulation/campaign_session.hpp
//
// PUBLIC HEADER — the live campaign session: the full Phase-C loop
// (C1 ledger + C2 one-pool tasking + C3 routed generation), composed
// into ONE object an interactive host drives frame by frame (the world
// viewer's Campaign window; any future host — the scenario player, an
// editor — composes the same way).
//
// This is the campaign_qc wiring REPACKAGED for a frame-driven host —
// no new campaign logic, no new sim logic, no library boundary crossed
// (f4-campaign still never sees EntityWorld; the session composes the
// same interfaces the QC tool's main() composes, as a reusable object):
//
//     WorldState ──► WorldStateAdapters ──► CampaignResultLedger   (C1)
//                                            ▲
//     Simulation (campaign_flights spawn)    │ sink (kills, bombs)
//        ▲                                   │
//        │ spawner (intents materialize      │
//        │ into THE SIM'S OWN WORLD) ────────┤
//        │                                   │
//     Campaign ladder ──► RouteBuilder ──────┘  (C3 routes + threat map)
//        (C2: draws book the SAME ledger the losses book)
//
// THE ONE-WORLD IMPROVEMENT over campaign_qc: the QC materializes the
// ladder's synthetic flights into a side EntityWorld that nothing ever
// ticks — "materialized" there means counted, not flown. The session
// points the spawner at the SIMULATION's world and registers every
// late spawn through Simulation::register_aircraft(), so generated
// missions taxi, depart, fly their routes, and die inside the same
// physics loop that flies the save's own flights. Generation → spawn
// → FLIGHT, in one world.
//
// Clock model: ONE clock. The sim ticks at the fixed sim_dt (the FM's
// tuned discretization — the "Fix Your Timestep" contract from the
// scenario player); the campaign ladder advances in whole campaign
// seconds accumulated from the same ticks, so tasking cycles, the
// reinforcement cadence, and the aircraft physics share one timeline.
// The host scales WALL-CLOCK time before calling advance() (a speed
// preset multiplies the frame dt; the tick dt never changes).
//
// Determinism: same as every other harness — no RNG, no clocks of
// their own, bus ordering. Two sessions over the same world advanced
// identically produce byte-identical ledger JSON (pinned by test).
//
// The session owns nothing on the GPU and touches no raylib/ImGui —
// it is a headless orchestration object (campaign_session.cpp compiles
// into f4-simulation's unit tests directly). The interactive world
// viewer wraps it; see f4-world-viewer/src/campaign_session_view.cpp.
//
// C++20. All paths given to Options must be ABSOLUTE (the scenario
// JSON resolves its world_json_path relative to the scenario file).

#pragma once

#include <f4/ai/air_picture.hpp>          // FID-5: the aggregate contacts
#include <f4/campaign/campaign.hpp>
#include <f4/campaign/flight_aggregate.hpp>
#include <f4/campaign/flight_writeback.hpp>
#include <f4/campaign/ground_war.hpp>
#include <f4/campaign/ground_writeback.hpp>
#include <f4/campaign/result_ledger.hpp>
#include <f4/campaign/route_builder.hpp>
#include <f4/campaign/world_writeback.hpp>
#include <f4/simulation/campaign_result_sink.hpp>
#include <f4/simulation/campaign_spawner.hpp>
#include <f4/simulation/simulation.hpp>
#include <f4/world/detail/world_state.hpp>
#include <f4/world/world_adapters.hpp>
#include <f4/world_types/class_table.hpp>
#include <f4/data/aircraft_config.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace f4::simulation {

/// FIDELITY_TIERS_PLAN (Docs/FIDELITY_TIERS_PLAN.md): how much of the
/// war runs at sim fidelity.
///
///   FullFidelity — TODAY's behavior, bit for bit: every spawned
///     aircraft runs the full FM + AI + sensors at 60 Hz from spawn to
///     recovery. Every existing golden pins this mode.
///   Tiered — the save's flights stay campaign AGGREGATES
///     (f4-campaign::FlightAggregateEngine: route-leg propagation at a
///     coarse cadence, cruise fuel burn) until an observer bubble (the
///     camera, V-3DLIVE semantics), an airfield-ops window (takeoff /
///     recovery), or an explicit request deaggregates them into real
///     per-flight aircraft. Time compression becomes a fidelity
///     problem, not a clock problem — the FIXED-dt discipline forbids
///     dt scaling, so the number of full-fidelity entities is the only
///     lever.
enum class FidelityPolicy {
    FullFidelity,
    Tiered,
};

/// Session inputs. Paths must be absolute. Defaults mirror campaign_qc
/// where the QC's choice is also right for an interactive host; the
/// differences are deliberate and documented.
struct CampaignSessionOptions {
    /// The decoded campaign world (v71 JSON). Required.
    std::filesystem::path world_json;
    /// FALCON4.ct (vis-type resolution for spawned aircraft).
    std::filesystem::path class_table;
    /// The F-16 aircraft config (FlightModelComponent::init needs the
    /// real aero tables). Required.
    std::filesystem::path aircraft_config;
    /// MissionProfiles.json (the generated mission-profile table).
    /// Required — the ladder cannot task without it.
    std::filesystem::path mission_profiles;

    /// Saved-flight spawn filter (the scenario's campaign_flight
    /// vocabulary): which of the save's own flights fly in the sim.
    /// The QC runs one mission type; a session wants the whole tasking
    /// picture, so the default is "everything", capped for interactivity
    /// (449 FMs at 60 Hz is a headless-budget run, not a UI).
    int team = -1;
    int mission = -1;
    int max_flights = 48;

    /// Tasking cycle period (CampaignConfig::air_task_cycle_sec).
    int tasking_cycle_sec = 1800;
    /// Reinforcement cadence; 43200 = the QC's armed 12 h.
    int reinforce_period_sec = 43200;
    /// C4: the ATM pipeline — the reference's actual tasking (FindBestAir
    /// scoring, escort pairing, TOT slotting, mission recovery) — ON by
    /// default: the session is the C4/C5 development surface. The
    /// C3 role-fallback bridge it replaces was the session's old
    /// default; the legacy ladder remains the library default.
    bool atm_pipeline = true;
    /// C4: the ATM's threat threshold for SEAD pairing (see the QC's
    /// host-side 25 — the fixture theater's single-ring band scores
    /// sit under the reference default of 40).
    int atm_seadescort_threat = 25;

    /// Fixed sim tick (the FM's operating point; never scaled).
    double sim_dt = 1.0 / 60.0;
    /// Per-advance tick cap (the spiral-of-death guard; the scenario
    /// player uses 30 for its 10x ceiling — the session's speed presets
    /// reach 240x, so the cap follows). Debt beyond the cap is dropped,
    /// never accumulated: the session stays live, time dilates.
    int max_steps_per_advance = 240;

    /// C5: the wreck hold — killed aircraft are RETIRED (removed from
    /// the roster + destroyed in the world) this many sim seconds
    /// after their EntityKilledMessage, keeping long-horizon rosters
    /// bounded (without it every death freezes a wreck in the world
    /// forever: a 24-hour war accumulates corpses until the per-tick
    /// roster walks drown). The loss is booked in the ledger at EVENT
    /// time, so retiring the wreck never races the books. 0 = the
    /// pre-C5 lifetime (wrecks freeze in place; every golden pins it);
    /// the war harness arms 300.
    double wreck_hold_sec = 0.0;

    /// C6: arm the campaign flights for A/A combat — the mission-role
    /// doctrine (CAP/Sweep/Intercept/Escort fight the full ladder;
    /// every other category flies defensive-only through its BRAINDAT
    /// archetype) + the combat component set + radar-backed detection
    /// (GCI-omniscience goes dark) + the doctrine A/A loadout for
    /// fighting roles. A/A kills book as air losses in the ledger and
    /// the reaper finally has wrecks to reap. Default false — the
    /// session's world is byte-identical to the pre-C6 shape with it
    /// off (the same contract wreck_hold_sec = 0 keeps). The ROE the
    /// armed campaign flies: full (bvr/missiles/guns all free — the
    /// war's acceptance wants fights to resolve).
    bool aa_combat = false;

    /// Real-data tier (Task 64): wcd2json export path folded over the
    /// built-in weapon table (empty = built-ins only). Read at session
    /// create; failures throw (loud), alias misses land in
    /// weapon_import_warnings().
    std::string weapon_data_path;
    /// C6: the BRAINDAT archetype table the doctrine's defensive roles
    /// stand down through. Empty = the sim's build-tree generated
    /// default (simdata/braindata.json). Only read when aa_combat.
    std::filesystem::path brain_data;

    /// G1: run the GROUND WAR — battalion movement, the front line,
    /// engagement attrition, and objective capture, on the campaign
    /// clock (see f4/campaign/ground_war.hpp). Ground events book in
    /// the same ledger the air war books (the C5 certificate covers
    /// both), moved battalions mirror into the sim's entities (the
    /// 3D world's ground units march), and the write-back gains the
    /// ground block (apply_ground_writeback). Default false — the
    /// session is byte-identical to the pre-G1 shape with it off (the
    /// same contract aa_combat keeps).
    bool ground_war = false;
    /// G1: the ground update cadence (campaign seconds; 60 = the
    /// engine default). Compressed-rig hosts shrink it.
    int ground_update_sec = 60;
    /// G1: the GTM orders cadence (campaign seconds; 1800 = the
    /// engine default).
    int ground_orders_sec = 1800;
    /// G1: the ground resupply cadence (campaign seconds; 0 = OFF,
    /// the engine's golden-identity default — the QC's war arms
    /// 43200 like the aircraft reinforcement cadence).
    int ground_resupply_sec = 0;

    /// G2: run the INTERDICTION link — UNIT-targeted delivery missions
    /// (the CAS family) resolve real enemy battalion targets (front-
    /// line ranked) and route to them, and the bombs they drop BOOK
    /// (the sink's unit-loss arm: ground losses air-sourced + per-
    /// vehicle ag credit; the ground-war engine pulls them and thins
    /// the line when ground_war is on — see
    /// Docs/archive/INTERDICTION_PLAN.md). Default false: UNIT-target
    /// profiles stay target-less/route-less (the C3-documented
    /// deferral) and unit-loss events count but do not book — the
    /// session is byte-identical to the pre-G2 shape with it off.
    bool unit_strike = false;

    /// FID-1: the fidelity policy (see FidelityPolicy above). Default
    /// FullFidelity — the session is byte-identical to the pre-FID
    /// shape with it (the same contract aa_combat / ground_war /
    /// unit_strike keep). Tiered arms the aggregate flight engine,
    /// defers the saved flights' aircraft spawn, and drives the
    /// deagg/reagg machinery below.
    FidelityPolicy fidelity_policy = FidelityPolicy::FullFidelity;
    /// FID-2: the aggregate advance cadence (campaign seconds; 60 =
    /// the ground war's update precedent).
    int air_agg_update_sec = 60;
    /// FID-3: the airfield-ops deagg window (campaign seconds) — a
    /// flight whose takeoff slot or mission-over time is within this
    /// window deaggregates (a GROUND spawn for a pending takeoff — the
    /// ATC flies it off; an AIR spawn for a recovery).
    int ops_window_sec = 600;
    /// FID-3: the reagg hysteresis factor — an observer-bubble
    /// deaggregation reaggregates at reagg_factor × the deagg radius
    /// (the band that keeps the boundary from thrashing).
    double air_reagg_factor = 1.5;
    /// FID-3: the minimum deaggregated time (campaign seconds) before
    /// a bubble-driven reagg may fire (the thrash guard).
    double deagg_cooldown_sec = 30.0;

    /// FID-5 (Docs/FIDELITY_TIERS_PLAN.md §4.5): run the event-driven
    /// COMBAT deagg — the aggregate contacts in the shared air picture,
    /// the commit/convergence triggers, and the transient combat windows
    /// (phase-pinned reagg). Tiered sessions only; false restores the
    /// FID-1..4 tiered shape exactly (the A/B and the escape hatch).
    bool combat_deagg = true;
    /// FID-5: register the tasking ladder's SYNTHETIC intents as
    /// AGGREGATES instead of spawning them straight to Tier-B (the
    /// FID-6 certificate's "the war's live aircraft are all synthetic"
    /// lever — the generated war rides the tier machinery like the save's
    /// own flights). Tiered sessions only; false restores the pre-FID-5
    /// spawner behavior byte for byte.
    bool synthetic_as_aggregates = true;
    /// FID-5: the engagement envelope (feet) — two opposing aggregate
    /// tracks whose PREDICTED positions (current + velocity ×
    /// combat_lookahead_sec) come inside this range, closing, deaggregate
    /// and the fight runs in-sim (§4.5's convergence trigger).
    double combat_envelope_ft = 30000.0;
    /// FID-5: the convergence lookahead (campaign seconds).
    int combat_lookahead_sec = 120;
    /// FID-5: the combat deagg pin (campaign seconds) — a combat-
    /// triggered deaggregation stays live this long before the standard
    /// reagg rules apply (the transient window; the phase pin).
    int combat_window_sec = 600;
};

/// The live campaign session. Create via create(); destroy to reset —
/// the whole graph is rebuilt from the world JSON, so a "reset" is just
/// a new session.
class CampaignSession {
public:
    /// One-frame numbers for the UI panel — everything the Campaign
    /// window shows, computed once per advance() (never per draw).
    struct Stats {
        int cycles = 0;               ///< tasking cycles fired
        /// Seconds until the next tasking cycle fires (the campaign
        /// view's "next ATO wave" countdown — the first generated
        /// missions land at air_task_cycle_sec, NOT at start).
        int next_tasking_sec = 0;
        int intents = 0;              ///< missions generated (ladder)
        int routes_built = 0;         ///< ladder route counters
        int routes_failed = 0;
        int route_waypoints = 0;
        int drawn_aircraft = 0;       ///< ledger: mission draws
        int air_losses = 0;           ///< ledger: combat losses
        int reinforce_fires = 0;      ///< ledger: cadence fires
        int reinforced = 0;           ///< ledger: aircraft delivered
        int synthetic_spawned = 0;    ///< spawner: generated flights
        int live_aircraft = 0;        ///< sim roster size
        int airborne = 0;             ///< FM gear.inAir
        double sim_time_s = 0.0;      ///< sim clock
        int retired = 0;              ///< C5: wrecks reaped so far
        // --- C4 (ATM pipeline) ------------------------------------------
        int packages = 0;             ///< ATM: packages built
        int escorts = 0;              ///< ATM: support flights paired
        int recovered = 0;            ///< ledger: aircraft recovered
        // --- C6 (campaign combat) ----------------------------------------
        int armed_aircraft = 0;       ///< sim: campaign aircraft armed
        int armed_fighters = 0;       ///< sim: fighting roles armed
        int armed_defensive = 0;      ///< sim: defensive roles armed
        int aa_kills = 0;             ///< ledger: air losses (A/A combat)
        // --- G1 (ground war) -----------------------------------------------
        int ground_updates = 0;       ///< engine: update ticks fired
        int ground_battalions = 0;    ///< engine: battalions alive now
        int ground_mobile = 0;        ///< engine: mobile battalions alive
        int ground_losses = 0;        ///< ledger: vehicles lost
        /// G2: the air-sourced share — vehicles removed from battalions
        /// by air power (the interdiction link's headline number).
        int ground_losses_air = 0;    ///< ledger: air-caused vehicles
        int ground_destroyed = 0;     ///< ledger: battalions destroyed
        int ground_captures = 0;      ///< ledger: objectives captured
        int ground_engaged = 0;       ///< engine: pairs in last update
        int ground_front_columns = 0; ///< engine: contested front columns
        // --- FID (fidelity tiers) -----------------------------------------
        int agg_updates = 0;          ///< engine: aggregate update ticks
        int agg_flights = 0;          ///< engine: aggregated flights
        int agg_live = 0;             ///< deaggregated (Tier-B) right now
        int agg_arrived = 0;          ///< reached their last waypoint
        int agg_destroyed = 0;        ///< folded as all-dead
        int tier_deaggs = 0;          ///< session: deaggregations so far
        int tier_reaggs = 0;          ///< session: reaggregations so far
        // --- FID-5 (event-driven combat deagg) -----------------------------
        int combat_deaggs = 0;        ///< deaggs the combat triggers fired
        int synthetic_aggregates = 0; ///< generated missions registered as aggregates
        int agg_contacts = 0;         ///< aggregate contacts in the air picture now
        int deferred_releases = 0;    ///< the commit-window veto's skipped releases
    };

    /// Build the whole graph. Returns nullptr and fills `error` on any
    /// failure (missing fixture, unloadable config, no airbase...).
    /// Throws nothing.
    [[nodiscard]] static std::unique_ptr<CampaignSession>
    create(const CampaignSessionOptions& opts, std::string* error = nullptr);

    ~CampaignSession();

    CampaignSession(const CampaignSession&) = delete;
    CampaignSession& operator=(const CampaignSession&) = delete;

    /// Advance by (speed-scaled) wall-clock seconds: drains the
    /// fixed-timestep accumulator in whole sim_dt ticks, advancing the
    /// campaign ladder and the damage sync in whole campaign seconds.
    /// Returns true when the tick cap hit (the caller may surface
    /// "time dilated" — the debt is dropped, not queued).
    ///
    /// V-THREAD: max_steps_override caps the ticks THIS call may run
    /// (0 = the session's max_steps_per_advance option — the QC and
    /// every test keep that behavior exactly). The session runner's
    /// worker uses the override to keep each lock hold short: it feeds
    /// the accumulator in small tick batches so the UI thread can grab
    /// the session between them. The override never RAISES the cap.
    bool advance(double real_seconds, int max_steps_override = 0);

    /// Pause/resume the drain (advance() no-ops while paused). The
    /// pause state is the UI's, not the sim's — the session keeps it
    /// so a paused session's canvas layer still renders.
    void set_paused(bool p) noexcept { paused_ = p; }
    [[nodiscard]] bool paused() const noexcept { return paused_; }

    // --- Read access for the render layer -------------------------------

    /// The simulation (aircraft entities, world, bus, terrain source
    /// registration point).
    [[nodiscard]] f4::simulation::Simulation& sim() noexcept { return *sim_; }
    [[nodiscard]] const f4::simulation::Simulation& sim() const noexcept {
        return *sim_;
    }

    /// The tasking ladder (intents, cycles, route counters).
    [[nodiscard]] const f4::campaign::Campaign& campaign() const noexcept {
        return *ladder_;
    }
    /// The generated missions, newest last (the Campaign window's table).
    [[nodiscard]] const std::vector<f4::campaign::MissionIntent>&
    intents() const noexcept {
        return ladder_->intents();
    }
    /// The spawner's own counters (the QC's summary vocabulary — the
    /// FID-5 deferral split reads it directly).
    [[nodiscard]] const CampaignSimSpawner::Stats& spawner_stats()
        const noexcept {
        return spawner_->stats();
    }
    /// The result ledger (draws, losses, reinforcements, damage).
    [[nodiscard]] const f4::campaign::CampaignResultLedger& ledger()
        const noexcept {
        return *ledger_;
    }
    /// The route builder (owns the threat map the overlay paints).
    [[nodiscard]] const f4::campaign::RouteBuilder& route_builder()
        const noexcept {
        return *route_builder_;
    }
    /// The threat-map viewer team (first belligerent; the threat
    /// overlay and the route planner share the perspective).
    [[nodiscard]] std::uint8_t threat_viewer_team() const noexcept {
        return threat_viewer_;
    }

    /// Absolute campaign time (the save's epoch + the ladder's clock) —
    /// the D# HH:MM:SS the window displays.
    [[nodiscard]] std::int64_t campaign_time() const noexcept {
        return epoch_ + ladder_->clock();
    }

    /// Snapshot of the one-frame numbers (recomputed by advance()).
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

    /// The ledger as campaign_result.json bytes (byte-stable).
    [[nodiscard]] std::string ledger_json() const {
        return ledger_->to_json();
    }

    /// Write the ledger's save-side state back into the session's own
    /// WorldState (team pools, squadron counters, objective fstatus)
    /// and report what landed. The "Write Back" button's payload —
    /// in-memory only; the .cam re-encoder is a future tranche.
    [[nodiscard]] f4::campaign::WorldWritebackResult apply_writeback() {
        return f4::campaign::apply_to(*ledger_, ws_);
    }

    /// G1: write the ground war's state back (battalion positions/
    /// rosters/state, objective owner flips) — the air write-back's
    /// ground twin, the same in-memory contract.
    [[nodiscard]] f4::campaign::GroundWritebackResult
    apply_ground_writeback() {
        if (ground_ == nullptr) {
            return f4::campaign::GroundWritebackResult{};
        }
        return f4::campaign::apply_ground_to(*ground_, ws_);
    }

    /// G1: the ground war engine (null when the session ran without
    /// ground_war). Read access for the viewer's ground panel.
    [[nodiscard]] const f4::campaign::GroundWar* ground_war() const
        noexcept {
        return ground_.get();
    }

    /// The session's own WorldState (the write-back target; the world
    /// JSON re-read for the session — the host's static layer may hold
    /// its own copy).
    [[nodiscard]] const f4::world::WorldState& world_state() const noexcept {
        return ws_;
    }

    // --- V-3DLIVE: camera-driven deaggregation (view bubble) ----------
    /// Point the deaggregation bubble at the host's CAMERA position
    /// (ENU feet) with a host-chosen radius (scales with zoom), and
    /// apply it immediately — a PAUSED session still deaggregates the
    /// units the user zooms into (the viewer calls this under the
    /// session lock as the camera moves). Forwards to Simulation::
    /// set_view_bubble() + one refresh_bubble() pass.
    /// FID: under the Tiered policy the SAME bubble drives the AIR
    /// deaggregation (the ground BubbleManager's own bubble stays
    /// ground-only) — flights within max(radius, the AII SIM_BUBBLE
    /// floor) spawn their aircraft; see force_deaggregate_flight.
    void set_view_bubble(double radius_ft,
                         const f4::geo::WorldPosition& center) {
        sim_->set_view_bubble(radius_ft, center);
        air_bubble_active_ = true;
        air_bubble_radius_ft_ = std::max(radius_ft, default_air_radius_ft_);
        air_bubble_center_ = center;
        sim_->refresh_bubble();
        // FID: the V-3DLIVE rule for the AIR side too — the tier pass
        // runs NOW, so a paused session still deaggregates the flights
        // the user zooms into (the next advance() re-evaluates anyway).
        if (flights_ != nullptr) {
            evaluate_tiers_();
            refresh_stats_();
        }
    }

    /// Return to the ownship-driven bubble + apply immediately.
    /// FID: the AIR bubble drops with it (unobserved aggregates
    /// reaggregate on the next tier evaluation, cooldown permitting).
    void clear_view_bubble() {
        sim_->clear_view_bubble();
        air_bubble_active_ = false;
        sim_->refresh_bubble();
    }

    // --- FID: the tier machinery (fidelity-tier sessions only) --------

    /// One flight's tier snapshot for the UI (the flights table).
    struct FlightTierView {
        std::uint32_t vu = 0;
        std::uint8_t team = 0;
        std::uint8_t mission = 0;
        int aircraft_count = 0;
        double x_grid = 0.0;
        double y_grid = 0.0;
        float altitude_ft = 0.0f;
        std::int32_t fuel_burnt = 0;
        bool live = false;      ///< Tier-B (deaggregated) right now
        bool arrived = false;
        bool destroyed = false;
        std::int32_t to_depart = -1;        ///< seconds (−1 = none)
        std::int32_t to_mission_over = -1;  ///< seconds (−1 = none)
    };

    /// True under the Tiered policy (the aggregate engine exists).
    [[nodiscard]] bool tiered() const noexcept {
        return flights_ != nullptr;
    }

    /// The flights' tier snapshot (wire order; the Campaign window's
    /// flights table). Called under the session lock.
    [[nodiscard]] std::vector<FlightTierView> flight_tiers() const;

    /// FID-4: force one flight deaggregated NOW — an ops-airfield GROUND
    /// spawn when the aggregate has not departed yet (the ATC flies it
    /// off), an AIR spawn at the aggregate's position otherwise. The
    /// flight stays deaggregated until force_reaggregate_flight
    /// (force beats every automatic trigger). Unknown vu = no-op.
    void force_deaggregate_flight(std::uint32_t vu);

    /// FID-4: force one flight reaggregated NOW — the lead-aircraft
    /// roll-up (position/altitude/fuel fold into the aggregate, the
    /// aircraft entity retires); an all-dead flight folds as destroyed.
    /// Clears the force-deagg pin. Unknown/not-deaggregated vu = no-op.
    void force_reaggregate_flight(std::uint32_t vu);

    /// EntityId lookup for the LIVE world (the sim's): VU_ID.num →
    /// entity, rebuilt at construction from the sim's own population.
    [[nodiscard]] const std::unordered_map<std::uint32_t,
        f4::entities::EntityId>& unit_id_map() const noexcept {
        return unit_id_map_;
    }
    [[nodiscard]] const std::unordered_map<std::uint32_t,
        f4::entities::EntityId>& objective_id_map() const noexcept {
        return objective_id_map_;
    }

private:
    CampaignSession() = default;

    /// One deaggregated flight: the materialized aircraft + the tier
    /// bookkeeping (the trigger that spawned it, when, and until when
    /// an ops-window pin holds). Declared first — the tier methods'
    /// signatures name it.
    struct DeaggregatedFlight {
        f4::entities::EntityId aircraft{};
        /// FID-5 adds Combat: a commit/convergence trigger deaggregated
        /// the flight into a transient fight window (the plan §4.5 —
        /// both flights of a convergence deagg, seeded, deterministic).
        enum class Trigger : std::uint8_t { Force, Ops, Bubble, Combat }
            trigger;
        std::int64_t deagg_time = 0;     ///< campaign time of the deagg
        std::int64_t pinned_until = 0;   ///< ops/combat pin (0 = none)
    };

    /// FID-5: the session's own MissionIntent subscription (registered
    /// BEFORE the spawner's — bus order is subscription order — so the
    /// synthetic-deferral arm hears the intent first).
    void handle_mission_intent_(const f4::campaign::MissionIntent& intent);

    /// FID-5: one combat pass — rebuild the aggregate-contact feed (the
    /// shared air picture's §4.6 form) and run the commit/convergence
    /// triggers (§4.5). Per campaign second, after the tier pass.
    void evaluate_combat_();

    /// FID-5: rebuild aggregate_contacts_ + aggregate_vu_set_ from the
    /// engine's state (the picture-exclusion and launch-veto sets ride
    /// the same buffers).
    void rebuild_aggregate_feed_();

    /// Register any entities the spawner materialized since the last
    /// call (the roster delta) — the one-world closure.
    void adopt_new_spawns_();

    /// Retire wrecks whose hold expired (the C5 roster bound; only
    /// armed when wreck_hold_sec > 0 — a no-op otherwise).
    void retire_due_wrecks_();

    /// G1: fire the ground war's update ticks for every whole
    /// ground-second owed by the campaign clock (the ladder's own
    /// per-second cadence, on the ground engine's coarser update
    /// granularity), then mirror moved battalions into the sim's
    /// entities (transforms + tactical state + roster decay + the
    /// ALIVE tag on destruction).
    void advance_ground_();

    /// G1: the entity-side mirror — one pass over the engine's dirty
    /// battalions (the write-back's own activity rule).
    void sync_ground_entities_();

    /// FID-2: fire the aggregate engine's update ticks for every whole
    /// air-aggregate second owed by the campaign clock (the ground
    /// cadence's twin), then mirror moved flights into the sim's
    /// flight entities (transforms + FlightPlanComponent fields) and
    /// run one tier evaluation (bubble / ops-window / force triggers,
    /// the reagg hysteresis + cooldown).
    void advance_flights_();

    /// FID-2: the entity-side mirror — engine positions/altitudes/fuel
    /// into the flight entities' TransformComponent +
    /// FlightPlanComponent (only changed values write).
    void sync_flight_entities_();

    /// FID-3: one tier pass — deagg triggers (force > ops > bubble) and
    /// reagg rules (force pins; ops pins through its window; bubble
    /// obeys the hysteresis band + cooldown). Runs per campaign second.
    void evaluate_tiers_();

    /// FID-4: materialize one aggregate flight (the deagg half of the
    /// handoff): a GROUND spawn when the flight has not departed yet,
    /// an AIR spawn at the aggregate position otherwise; registers +
    /// arms the aircraft (the adopt path's own pairing).
    void deaggregate_flight_(std::size_t index,
                             DeaggregatedFlight::Trigger trigger);

    /// FID-4: fold one live flight back (the reagg half): lead-aircraft
    /// state into the engine, the aircraft retired; an all-dead flight
    /// folds as destroyed. Returns true when a fold happened.
    bool reaggregate_flight_(std::uint32_t vu);

    /// Recompute stats_ from the live objects.
    void refresh_stats_();

    // --- Order matters (reverse destruction) ----------------------------
    // Everything BELOW borrows references from things constructed
    // EARLIER. Destruction runs sink → spawner → ladder → builder →
    // ledger → sim (the bus owner) → config/ct → adapters → ws: every
    // borrower dies before its lender.

    // Pacing (copied from Options — advance() needs them after the
    // options object is long gone).
    double sim_dt_ = 1.0 / 60.0;
    int max_steps_per_advance_ = 240;

    // Data layer (the lenders).
    f4::world::WorldState ws_;                 // the write-back target
    std::unique_ptr<f4::world::WorldStateAdapters> adapters_;
    f4::world_types::ClassTable ct_;
    f4::data::AircraftConfig cfg_;
    f4::campaign::MissionProfileTable profiles_;
    f4::weapons::WeaponClassTable weapon_table_;  // arming the spawns
    std::vector<std::string> weapon_import_warnings_;

    // Spawner lenders (create() used to hand CampaignSimSpawner three
    // LOCALS — the fallback airfield, the per-airbase airfield map, and
    // the template aircraft — that died when create() returned, leaving
    // the spawner's airfield_/airbase_airfields_/tpl_ dangling; the
    // first synthetic spawn after a tasking cycle then read freed
    // memory. The members replace them: the spawner's references stay
    // valid for the session's lifetime (declared before spawner_, so
    // they are destroyed after it — reverse-order destruction keeps
    // every borrower dying before its lender).
    f4::simulation::ScenarioAirfield airfield_{};
    f4::simulation::AirbaseAirfieldMap airbase_airfields_{};
    f4::simulation::ScenarioAircraft spawn_tpl_{};

    // The simulation (owns the ONE world + the bus everything uses).
    std::filesystem::path scenario_temp_dir_;
    std::unique_ptr<f4::simulation::Simulation> sim_;

    // The loop (all bus subscribers; detached in the dtor).
    std::unique_ptr<f4::campaign::CampaignResultLedger> ledger_;
    std::unique_ptr<f4::campaign::RouteBuilder> route_builder_;
    std::unique_ptr<f4::campaign::Campaign> ladder_;
    std::unique_ptr<f4::campaign::GroundWar> ground_;
    std::unique_ptr<f4::simulation::CampaignSimSpawner> spawner_;
    std::unique_ptr<f4::simulation::CampaignResultSink> sink_;

    // Cross-reference maps over the SIM's world (VU_ID.num → EntityId),
    // rebuilt after initialize() the way the bridge/sink resolve ids.
    std::unordered_map<std::uint32_t, f4::entities::EntityId> unit_id_map_;
    std::unordered_map<std::uint32_t, f4::entities::EntityId>
        objective_id_map_;

    // Clock + pacing state.
    std::int64_t epoch_ = 0;        ///< the save's campaign.current_time
    double accumulator_ = 0.0;      ///< owed sim seconds (speed-scaled)
    double campaign_sec_accum_ = 0.0;  ///< fractional campaign seconds
    bool paused_ = false;
    std::size_t registered_spawns_ = 0;  ///< spawner().spawned() index

    // C5: the wreck policy. The kill subscription records
    // (entity, sim-time) pairs as EntityKilledMessage lands; the
    // per-campaign-second cadence in advance() retires the ones past
    // the hold. Arrival order, deterministic; ids retire at most once
    // (retire_aircraft's idempotence eats replayed messages).
    double wreck_hold_sec_ = 0.0;
    std::size_t kill_subscription_ = 0;  ///< 0 = not subscribed
    struct PendingWreck {
        f4::entities::EntityId id;
        double death_s = 0.0;
    };
    std::vector<PendingWreck> pending_wrecks_;

    // G1: the ground war's own cadence accumulator (campaign seconds
    // owed to the engine) + the last-synced engine update count (so
    // the entity mirror only walks when the engine actually advanced).
    double ground_sec_accum_ = 0.0;
    int ground_synced_updates_ = 0;

    // FID: the fidelity-tier machinery. flights_ is null unless the
    // Tiered policy armed it (tiered() reads that); every member below
    // is inert without it.
    std::unique_ptr<f4::campaign::FlightAggregateEngine> flights_;
    /// FID pacing (copied from Options — they outlive the options object).
    int air_agg_update_sec_ = 60;
    int ops_window_sec_ = 600;
    double air_reagg_factor_ = 1.5;
    double deagg_cooldown_sec_ = 30.0;

    std::unordered_map<std::uint32_t, DeaggregatedFlight> deaggregated_;
    int tier_deaggs_ = 0;
    int tier_reaggs_ = 0;

    // FID-5: the combat-deagg machinery + the synthetic-intent tiering.
    // Everything below is inert unless the Tiered policy armed flights_
    // AND the corresponding option is on.
    bool synthetic_as_aggregates_ = true;
    bool combat_deagg_ = true;
    double combat_envelope_ft_ = 30000.0;
    int combat_lookahead_sec_ = 120;
    int combat_window_sec_ = 600;
    /// The generated missions' intents, keyed by the reserved-namespace
    /// vu register_synthetic returned (the deagg spawn path reads the
    /// intent — parking/squadron/loadout resolution lives there).
    std::unordered_map<std::uint32_t, f4::campaign::MissionIntent>
        synthetic_intents_;
    /// The aggregate-contact feed the sim's picture appends (non-owning
    /// pointer handed over at create; rebuilt per campaign second).
    std::vector<f4::ai::AggregateContact> aggregate_contacts_;
    /// The flight VUs currently published as contacts — the combat
    /// triggers' id space (a brain's engagement id is matched here) and
    /// the sim's launch-veto set (the SAME set object, so the veto is
    /// always exactly the published contacts).
    std::unordered_set<std::uint64_t> aggregate_vu_set_;
    /// The campaign-flight entities excluded from the picture's world
    /// walk (their truth flows through the feed; a suspended flight's
    /// frozen transform must not linger as a phantom contact).
    std::unordered_set<std::uint64_t> flight_entity_ids_;
    /// Per-airbase parking counters for the synthetic ground spawns (the
    /// spawner's own bookkeeping shape; its counters stay untouched —
    /// deferral means it never spawns these flights).
    std::unordered_map<std::uint64_t, int> synthetic_parking_index_;
    std::size_t intent_subscription_ = 0;  ///< 0 = not subscribed
    int combat_deaggs_ = 0;                ///< combat-trigger deaggs so far
    int synthetic_registered_ = 0;         ///< generated aggregates so far

    double flight_sec_accum_ = 0.0;
    int flight_synced_updates_ = 0;
    double default_air_radius_ft_ = 2560.0;  ///< the AII SIM_BUBBLE floor
    bool air_bubble_active_ = false;         ///< the camera-driven air bubble
    double air_bubble_radius_ft_ = 2560.0;
    f4::geo::WorldPosition air_bubble_center_{};

    // Display snapshot.
    Stats stats_;
    std::uint8_t threat_viewer_ = 0;
};

} // namespace f4::simulation
