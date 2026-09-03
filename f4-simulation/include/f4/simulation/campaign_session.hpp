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

#include <f4/campaign/campaign.hpp>
#include <f4/campaign/result_ledger.hpp>
#include <f4/campaign/route_builder.hpp>
#include <f4/campaign/world_writeback.hpp>
#include <f4/simulation/campaign_result_sink.hpp>
#include <f4/simulation/campaign_spawner.hpp>
#include <f4/simulation/simulation.hpp>
#include <f4/world/detail/world_state.hpp>
#include <f4/world/world_adapters.hpp>
#include <f4/world_convert/class_table.hpp>
#include <f4/data/aircraft_config.hpp>
#include <f4/models/model_database.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace f4::simulation {

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
        // --- C4 (ATM pipeline) ------------------------------------------
        int packages = 0;             ///< ATM: packages built
        int escorts = 0;              ///< ATM: support flights paired
        int recovered = 0;            ///< ledger: aircraft recovered
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
    bool advance(double real_seconds);

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

    /// The session's own WorldState (the write-back target; the world
    /// JSON re-read for the session — the host's static layer may hold
    /// its own copy).
    [[nodiscard]] const f4::world::WorldState& world_state() const noexcept {
        return ws_;
    }

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

    /// Register any entities the spawner materialized since the last
    /// call (the roster delta) — the one-world closure.
    void adopt_new_spawns_();

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
    f4::world_convert::ClassTable ct_;
    f4::models::ModelDatabase db_;             // empty: 2D symbols only
    f4::data::AircraftConfig cfg_;
    f4::campaign::MissionProfileTable profiles_;
    f4::weapons::WeaponClassTable weapon_table_;  // arming the spawns

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

    // Display snapshot.
    Stats stats_;
    std::uint8_t threat_viewer_ = 0;
};

} // namespace f4::simulation
