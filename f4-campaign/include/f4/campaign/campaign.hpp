// f4-campaign/include/f4/campaign/campaign.hpp
//
// Campaign — the headless dynamic-campaign engine (Architecture Proposal
// M4.7, first slice per NEXT_PHASE_PLAN.md §B.2).
//
// FreeFalcon's campaign is a master CampaignClass tracking campaign time
// and firing each domain's tasking cycle when its timestamp comes due;
// the Air Tasking Manager then decides which missions should exist and
// builds packages for them. F4's Campaign reproduces that shape on the
// engine-agnostic side:
//
//   * It consumes the f4-world IDataSource interfaces (ICampaignSource,
//     ITeamSource, IUnitCoreSource) — the same boundary discipline
//     everything else uses. It NEVER sees f4-world-convert, EntityWorld
//     components, or a flight model (the NullFlightModel discipline:
//     the campaign tick moves nothing; units stay put this slice).
//   * `tick(delta)` advances the clock and fires the air tasking cycle
//     whenever it comes due. Per belligerent team, the cycle walks the
//     mission-profile table in wire-byte order and generates a mission
//     for every profile whose cadence and availability gates pass:
//
//       - availability (role): the profile's `aro` must match one of the
//         team's squadrons (specialty byte bound through aro_name()).
//       - availability (capability): a profile with caps this slice
//         cannot verify (VEH_STEALTH, VTOL, ...) does not generate —
//         same effect as FreeFalcon's caps check, conservative default.
//       - availability (aircraft): the team's aircraft pool (squadron
//         roster when nonzero, else the campaign source's per-team
//         te_number_aircraft) must cover the profile's `str` (default
//         aircraft count); each generated mission deducts from the
//         pool. A profile whose count can't be met at all (0 aircraft)
//         generates nothing.
//       - target: AMIS_TAR_NONE-carrying profiles (alert, training)
//         generate only when the profile carries no external-target
//         requirement — in this slice all profiles generate against
//         abstract targets; route/target resolution arrives with the
//         M4.3–M4.5 tranche.
//     Each generated mission publishes one MissionIntent on the message
//     bus — the only coupling to the outside world (B.3's sim-side
//     spawner subscribes to exactly this message).
//
//   * Time on target: the M4.7-skeleton rule is the midpoint of the
//     profile's planning-advance window (min_time + max_time)/2 minutes
//     past the cycle time — deterministic, profile-driven, and replaced
//     by real slot scheduling when the ATM tranche lands.
//
//   * Everything is a pure function of (source interfaces, profile
//     table, config, tick history): NO RNG anywhere in the campaign
//     layer (determinism stays absolute — see NEXT_PHASE_PLAN.md §3).
//     The same campaign state ticked the same way produces byte-stable
//     summaries — asserted by the golden test.
//
// Dependencies: f4-world (IDataSource), f4-messaging (bus), f4-json
// (summary writer). C++20.

#pragma once

#include <f4/campaign/atm.hpp>
#include <f4/campaign/mission_profile.hpp>
#include <f4/campaign/mission_type.hpp>
#include <f4/campaign/result_ledger.hpp>
#include <f4/campaign/route_builder.hpp>

#include <f4/messaging/bus.hpp>
#include <f4/world/data_source.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace f4::campaign {

/// Campaign clock in seconds, relative to campaign start (0 = start).
/// FreeFalcon's CampaignTime is likewise a second count; the world's
/// absolute epoch (campaign.current_time) is intentionally NOT folded in —
/// the tick advances a relative clock and the sources carry the epoch.
/// (Defined in mission_type.hpp since the C4 ATM pipeline; re-exported
/// here.)
using CampaignTime = std::int64_t;

/// One generated mission — the campaign's contract with the outside
/// world. Published on the MessageBus as f4::campaign::MissionIntent;
/// B.3's sim-side spawner subscribes and materializes flights through
/// the Milestone-A campaign spawn path.
struct MissionIntent {
    /// Campaign time the intent was issued (seconds, relative).
    CampaignTime issued_time{0};
    /// Campaign time the mission launches (seconds, relative).
    CampaignTime time_on_target{0};
    /// Owning team slot (from the unit data's owner vocabulary).
    std::uint8_t team{0};
    /// Owning team name (empty when the slot is unnamed).
    std::string team_name;
    /// Mission wire byte + canonical name (profile-bound).
    std::uint8_t mission_byte{0};
    std::string mission_name;
    /// Package composition: aircraft count and the source squadron.
    int aircraft_count{0};
    std::uint32_t squadron_id{0};    ///< home squadron VU_ID.num
    std::string squadron_name;       ///< display name from the unit data
    /// Synthetic package identity (deterministic counter).
    std::uint32_t package_id{0};
    /// One flight per package in this slice.
    std::uint32_t flight_id{0};

    // --- C3 (route tranche): target + route ---
    // The synthetic ladder previously tasked against ABSTRACT targets
    // ("route/target resolution arrives with the M4.3–M4.5 tranche");
    // with a route planner attached, every generated mission carries a
    // REAL target objective (enemy-owned, deterministic selection) and
    // the route to fly: takeoff → threat-avoiding ingress → target →
    // egress → landing. Saved-flight intents (emit_flight_intents)
    // leave these empty — their routes live in the save's own waypoint
    // lists, which the sim-side spawner reads directly.
    /// Target objective VU_ID.num (0 = none).
    std::uint32_t target_objective_id{0};
    /// The planned route (empty = no route; the spawner spawns
    /// takeoff-only).
    std::vector<RouteWaypoint> route;
    /// True when this intent came from the synthetic ladder (the
    /// spawner's flight-id namespace differs from the save's VU ids;
    /// its duplicate guard keys on the pair).
    bool synthetic{false};

    // --- C4 (ATM pipeline): package composition -------------------------
    // Multi-flight packages: one intent per FLIGHT (the spawner's
    // contract unchanged — each intent materializes one formation),
    // all flights of a package sharing package_id. The role marks the
    // support flights (SEADESCORT/ESCORT pairs), each carrying its own
    // mission byte and TOT (main TOT + the support profile's
    // separation); escorted_flight_id links the pair (the viewer's
    // package view reads it).
    std::uint8_t flight_role{0};   ///< FlightRole (0 = main)
    std::uint32_t escorted_flight_id{0};  ///< the main flight (0 = main)

    /// P7 — the flight's Rules of Engagement (the wire roe_check byte
    /// the request carried; 0 = weapons free, the default on every
    /// pre-P7 intent). The spawner records it per spawned flight; the
    /// session gates the brain's fire controls with it after arming
    /// (WeaponsTight → BVR hold, WeaponsHold → everything held).
    std::uint8_t roe{0};

    /// DOM-3 — the flight's crew (the squadron's ROSTER SLOTS in pick
    /// order, crew[0] = the lead; empty on every pre-DOM-3 intent and
    /// on every save-loaded flight — the wire zeroes the flight tail's
    /// own pilot bytes, the documented gap). The spawner resolves the
    /// lead's PilotState for the brain-skill stamp when the
    /// pilot-skill flow is armed.
    std::vector<std::uint8_t> crew;

    /// DOM-4 — the flight's SCHEDULED takeoff (the phase-7 slot snap's
    /// output, campaign-relative seconds; 0 when the flight never
    /// slotted — the legacy ladder's intents, the save's own flights).
    /// The session's airfield-ops gate arms against this slot when the
    /// scheduling arm is on — the ATC window wraps the grid's own
    /// minute instead of a TOT-derived guess (the FIDELITY_TIERS §7
    /// delivery-latency divergence closes).
    CampaignTime takeoff{0};

    /// Element-wise equality (tests assert bus content == recorded intents).
    bool operator==(const MissionIntent&) const = default;
};

/// Tunables. Defaults documented in the header doc; every field is
/// pinned by the golden test through the generated summary.
struct CampaignConfig {
    /// Air tasking cycle period (FreeFalcon's ATM::Task cadence; the
    /// plan's 30-minute validation run is one cycle at the default).
    CampaignTime air_task_cycle_sec{1800};
    /// First synthetic package id (deterministic counter start).
    std::uint32_t first_package_id{1};
    /// C2: the reinforcement cadence — FreeFalcon's gate is
    /// `CurrentTime > LastReinforcement + Rate`, and the rate is a
    /// RUNTIME DIFFICULTY SETTING in the reference (campaign ratios,
    /// not wire data), so it lives here as a tunable. The anchor IS
    /// wire data (the .cmp header's last_reinforcement, exposed via
    /// ICampaignSource). The default is DISABLED (0): a fresh ledger
    /// attached to a default-configured Campaign must change nothing
    /// (the C1 golden identity — a stale .cmp anchor would otherwise
    /// fire the cadence the moment a ledger lands). Hosts opt in
    /// (campaign_qc's --tasking arms 12 h unless overridden); 0 stays
    /// the pre-C2 depletion-only behavior.
    CampaignTime reinforcement_period_sec{0};

    /// DOM-2 (supply depth): the strategic reserve flow. When true,
    /// each reinforcement fire also refills the squadrons' consumed
    /// reinforcement budgets toward their wire snapshot out of the
    /// team's `replacements_avail` reserve (decoded since C2, exposed
    /// on ITeamSource, never consumed until now — the CAMPAIGN_LOOP
    /// plan's own "stock-to-budget replenishment flow"). Default
    /// false: the C2 shape (budgets consumed, never replenished) is
    /// the golden identity; the reserve rides untouched.
    bool replacement_stock_flow{false};

    /// C3: tasking role fallback — when NO squadron of the team
    /// carries a profile's exact ARO role, fall back to the
    /// best-available squadron regardless of specialty (a step toward
    /// the reference's own shape: FreeFalcon's squadron selection
    /// SCORES role match and aircraft capability — a counter-air wing
    /// of F-16s is taskable for strike — rather than gating on it;
    /// the full scoring is the C4 FindBestAir tranche). DEFAULT OFF:
    /// the strict role gate is B.3/C2 behavior and its goldens are
    /// pinned byte-identical. Hosts exercising the generation-to-
    /// spawn chain (campaign_qc's --tasking) arm it — TestCamp's
    /// belligerents (the ROK-DPRK war the corrected RelType decode
    /// reveals) field all-counter-air squadrons, so delivery-family
    /// missions never generate under the strict gate.
    bool tasking_role_fallback{false};

    /// C4: the ATM pipeline — the 7-phase tasking engine (request
    /// generation → prioritization → deconfliction → package building
    /// with FindBestAir → escort pairing → route planning → TOT slot
    /// scheduling, plus mission recovery: drawn aircraft that survive
    /// their mission RETURN to the pool). DEFAULT OFF: the legacy
    /// ladder (B.3/C2/C3 shape, role gate + optional fallback) is the
    /// goldens-pinned behavior; hosts wanting the reference's actual
    /// tasking (campaign_qc, the campaign session) arm it. While
    /// armed, `tasking_role_fallback` is inert — FindBestAir SCORES
    /// role/capability and REPLACES the fallback bridge.
    bool atm_pipeline{false};

    /// C4: the ATM's own tunables (aiinput.dat's [ATM] section as
    /// config — the RouteBuilderConfig pattern).
    AtmConfig atm{};

    /// G2 — the interdiction arm: UNIT-targeted delivery missions (the
    /// CAS family) resolve a REAL enemy battalion target — front-line
    /// ranked (distance to the contested FLOT, wire-order ties,
    /// rotation-spread), ledger-destroyed skipped — and route to it;
    /// the bombs those flights drop attrite the line (the sink's
    /// unit_strike arm books, the engine pulls). DEFAULT OFF: the
    /// golden identity (UNIT-target profiles stay target-less and
    /// route-less, exactly the C3-documented deferral). One flag for
    /// both ladders (the ATM's config inherits it at construction).
    bool unit_strike{false};

    /// P7 — the strategy layer: the ATM gains CAP-family station
    /// targeting (defensive CAPs orbit ranked OWN objectives, not
    /// target-less), FindSupportFlights (ADDAWACS/ADDTANKER/ADDECM
    /// share-or-file — AWACS/tanker/ECM stations flying the
    /// TPROF_LOITER racetrack routes), and RequestEnemyMission (a
    /// strike package's ADDBARCAP files a defender BARCAP over the
    /// threatened objective for the enemy's next cycle); the route
    /// builder gains the loiter racetrack circuits; the intents carry
    /// RoE. DEFAULT OFF — the golden identity (target-less CAP,
    /// package-only routes, no filings; every pinned test unchanged).
    /// Requires atm_pipeline (the legacy ladder has no strategy
    /// layer); hosts arm both.
    bool strategy_layer{false};

    /// DOM-3 — the AssignPilots arm: filed flights draw their CREW from
    /// the squadron's decoded pilot roster (the reference's
    /// front-third lead / backward wingmen scan; a squadron that cannot
    /// crew is skipped), the crew rides the MissionIntent and the
    /// personnel books (assignment / loss / recovery), and the events
    /// name the pilots. DEFAULT OFF — the golden identity (rosters
    /// ignored beyond the SCALE-1 skill map). One flag for both
    /// ladders (the ATM's config inherits it at construction).
    bool pilot_assignment{false};

    /// DOM-3 — the rating-decay arm: the squadron's per-role
    /// effectiveness table (the .uni rating[16]) decays 25% per
    /// assignment — new = (int)(0.75 × rating) + 1 — spreading the
    /// sorties across the wing (the rotation pressure). DEFAULT OFF.
    bool rating_decay{false};

    /// DOM-4 — the airbase-scheduling depth arm: the slot grid slides
    /// with the clock (a moving epoch — the 160-minute horizon stops
    /// silencing late filings), FindBestAir's gate applies the
    /// reference's own previous-block rule and counts its denials, a
    /// scrubbed flight's still-future slot releases, a horizon refusal
    /// counts (the slot_denied ledger log + event family ride it), and
    /// the intents carry the scheduled takeoff so the sim's airfield-
    /// ops window arms against the SLOT. DEFAULT OFF — the golden
    /// identity (the campaign-start anchor, the single-block gate, the
    /// silent overflow; every pinned test unchanged). One flag for the
    /// whole arm (the ATM's config inherits it at construction).
    bool airbase_scheduling{false};
};

class Campaign {
public:
    /// Bind the campaign to its world sources, the profile table, and
    /// the message bus. All references must outlive the Campaign.
    /// \param camp    campaign-level state (per-team aircraft pools)
    /// \param teams   team slots + stance matrix (belligerence source)
    /// \param units   unit roster (squadrons: owner, specialty, aircraft)
    /// \param profiles the validated mission profile table (B.1)
    /// \param bus     intents publish here
    /// \param cfg     tunables (see CampaignConfig)
    Campaign(const f4::world::ICampaignSource& camp,
             const f4::world::ITeamSource& teams,
             const f4::world::IUnitCoreSource& units,
             const MissionProfileTable& profiles,
             f4::messaging::MessageBus& bus,
             const CampaignConfig& cfg = {});

    /// Advance the clock by `delta_sec` and fire every tasking cycle that
    /// has come due (a delta spanning several cycles fires them all, in
    /// order). Pure with respect to the sources: calling tick with the
    /// same history always yields the same intents.
    void tick(CampaignTime delta_sec);

    /// Campaign-relative clock (seconds).
    [[nodiscard]] CampaignTime clock() const noexcept { return clock_; }

    /// Tasking cycles fired so far.
    [[nodiscard]] int cycles_fired() const noexcept { return cycles_fired_; }

    /// Seconds until the next tasking cycle fires (the campaign view's
    /// "next ATO wave in MM:SS" readout). next_cycle_ starts at 0, so a
    /// fresh ladder reports the full air_task_cycle_sec — the first
    /// generated missions land exactly that many campaign seconds in.
    [[nodiscard]] CampaignTime seconds_to_next_cycle() const noexcept {
        return next_cycle_ + cfg_.air_task_cycle_sec - clock_;
    }

    /// Reinforcement ticks fired so far (C2 — the cadence fires when
    /// campaign time passes the .cmp anchor + period; see
    /// CampaignConfig::reinforcement_period_sec). Requires a ledger
    /// (the refill lands in the write model); the legacy no-ledger
    /// mode never fires.
    [[nodiscard]] int reinforcement_fires() const noexcept {
        return reinforcement_fires_;
    }

    /// Every intent published since construction, in publish order.
    [[nodiscard]] const std::vector<MissionIntent>& intents() const noexcept {
        return intents_;
    }

    /// C3 route counters — the generation-to-spawn chain's own
    /// telemetry (QC's exit-7 gate reads these; counting route-less
    /// intents from intents() cannot see the failures — an intent
    /// whose build failed carries no route AND no synthetic mark).
    /// routes_failed counts every precondition miss (no target
    /// resolved, build produced < 2 waypoints) while a planner was
    /// attached.
    [[nodiscard]] int routes_built() const noexcept {
        return routes_built_;
    }
    [[nodiscard]] int routes_failed() const noexcept {
        return routes_failed_;
    }
    /// FindSafePath invocations over the threshold (ingress/egress).
    [[nodiscard]] int route_safe_searches() const noexcept {
        return route_safe_searches_;
    }
    /// Legs that fell back to the direct line after 3 partial passes.
    [[nodiscard]] int route_fallbacks() const noexcept {
        return route_fallbacks_;
    }

    /// C4: the ATM pipeline's telemetry (requests, packages, escorts,
    /// slot snaps, recoveries) — null when the pipeline is not armed
    /// (the legacy ladder's own counters above stand in).
    [[nodiscard]] const AtmStats* atm_stats() const noexcept {
        return atm_ ? &atm_->stats() : nullptr;
    }

    /// C4: the flights the ATM has booked (in-flight, awaiting
    /// recovery) — null when the pipeline is not armed.
    [[nodiscard]] const std::vector<FlightTasking>* atm_booked_flights()
        const noexcept {
        return atm_ ? &atm_->booked_flights() : nullptr;
    }

    /// DOM-4: the ATM's airbase schedule books (the airfields query's
    /// source — the grid, its anchor, and its denial books per base,
    /// wire order) — null when the pipeline is not armed.
    [[nodiscard]] const std::vector<AirbaseSchedule>* atm_schedules()
        const noexcept {
        return atm_ ? &atm_->schedules() : nullptr;
    }

    // --- CAMP-CMD-2 — the command-driven booking interventions ---------
    //
    // The Campaign is the tasking owner (it books draws and recoveries
    // — one writer, one clock), so the session's retask/abort commands
    // reach the ATM's bookings THROUGH it.

    /// Retask one booked flight (flight_retask's bookkeeping): the
    /// booking follows the flight — new mission byte, target, TOT, and
    /// mission-over deadline (the recovery clock moves with the new
    /// plan; the ledger books stay untouched — the draw stands, the
    /// recovery books when the retasked mission closes). False when no
    /// booking carries the flight id (save-carried flights have none).
    bool reschedule_flight(std::uint32_t flight_id, std::uint8_t mission,
                           std::uint32_t target_vu, CampaignTime tot,
                           CampaignTime mission_over) {
        return atm_ && atm_->reschedule_flight(flight_id, mission,
                                               target_vu, tot,
                                               mission_over);
    }

    /// Scrub one booked flight (flight_abort's bookkeeping — the
    /// mission scrub): the booking closes NOW and the survivors'
    /// recovery books into the ledger at the CURRENT clock (the same
    /// apply_mission_recovery recover_missions_ rides — one booking
    /// site). Returns the release (nullopt when no booking carries the
    /// flight id — a save-carried flight's books closed in the save's
    /// own history; nothing to close here and nothing books).
    [[nodiscard]] std::optional<RecoveryRelease>
    scrub_flight(std::uint32_t flight_id) {
        if (!atm_) return std::nullopt;
        auto rel = atm_->scrub_flight(flight_id, clock_);
        if (rel.has_value() && result_ledger_ != nullptr) {
            result_ledger_->apply_mission_recovery(
                static_cast<double>(clock_), rel->team, rel->squadron_vu,
                rel->flight_id, rel->survivors);
        }
        return rel;
    }

    /// Attach the C1 result ledger — the war-loop feedback. While
    /// attached, the LEDGER IS THE TASKING POOL (C2): the cycle's
    /// availability gates read the ledger's one-pool numbers
    /// (squadron_tasking_available: snapshot − draws − non-drawn
    /// losses + reinforcements) and every generated mission debits it
    /// (apply_mission_draw) — cycles and combat deplete ONE pool, and
    /// the reinforcement cadence refills it in place. The Campaign's
    /// own pool_/available counters are UNTOUCHED in this mode (the
    /// no-ledger path keeps them — B.3's behavior, byte-identical
    /// goldens).
    ///
    /// The ledger is BORROWED and MUTABLE (the set_brain_archetype /
    /// set_weapon_table pattern; C1 read it, C2 also writes draws and
    /// fires reinforcement into it): null detaches, and a ledger
    /// whose slots carry no events reports the same numbers the
    /// Campaign's own pool would — attaching a fresh ledger changes
    /// nothing until draws or losses land in it (the golden identity).
    void set_result_ledger(CampaignResultLedger* ledger) noexcept {
        result_ledger_ = ledger;
        if (atm_) atm_->set_ledger(ledger);   // C4: keep the ATM current
    }

    /// Attach the C3 route planner — generation-to-spawn. While
    /// attached, every generated mission carries a target (the
    /// enemy-owned objective with the highest priority, ties in wire
    /// order — deterministic; the strategy layer that feeds
    /// FreeFalcon's mission requests is the C4 ATM tranche) and the
    /// route to fly it. The planner owns the threat map built from
    /// the SAME sources (built once — static dispositions this slice);
    /// `objectives` resolves targets and airbases. Null detaches.
    /// Without a planner the cycle generates exactly as before (the
    /// B.3/C2 goldens — byte-identical; this is an attachment, not a
    /// mode switch).
    void set_route_planner(const RouteBuilder* planner,
                           const f4::world::IObjectiveSource* objectives)
                           noexcept {
        route_planner_ = planner;
        objectives_ = objectives;
        if (atm_) {   // C4: keep the ATM's threat/target view current
            atm_->set_threat_map(planner ? &planner->threat_map()
                                         : nullptr);
            atm_->set_objectives(objectives);
        }
    }

    /// Slots of the teams currently at war (lowest slot first) — the
    /// stance-matrix belligerence rule (FreeFalcon ID_HOSTILE sign test
    /// over NAMED slots), shared with the sim-side team resolution.
    [[nodiscard]] std::vector<int> belligerent_teams() const;

    /// Deterministic summary of the run so far — the recorder's
    /// to_summary_json pattern. Byte-stable across identical runs; the
    /// golden test compares two executions byte-for-byte.
    [[nodiscard]] std::string to_summary_json() const;

private:
    /// One tasking cycle: per belligerent team, evaluate profiles and
    /// publish intents. Returns the intents generated THIS cycle.
    void run_tasking_cycle_();

    /// The legacy ladder (B.3/C2/C3's own walk — role gate, optional
    /// fallback, one flight per package). Kept verbatim: the
    /// goldens-pinned path when cfg_.atm_pipeline is off.
    void run_tasking_cycle_legacy_();

    /// C4: the ATM cycle — phases 1-5 via the ATM, route planning (6)
    /// and slot scheduling (7) here, one intent per flight.
    void run_tasking_cycle_atm_();

    /// C4: mission recovery — every completed flight's survivors
    /// return to the ledger's tasking pool (rides the tick, after the
    /// cycles — same position as the reinforcement cadence).
    void recover_missions_();

    /// C2: the reinforcement cadence — fire while now (epoch + clock)
    /// has passed last_reinforce_ + period. Each fire delivers into
    /// the ledger (deficits refilled from the wire's per-squadron
    /// budgets) and advances the anchor to now (catch-up-once — a
    /// stale .cmp timer fires ONE tick, not the months it is behind).
    /// No-op without a ledger (the refill is write-model state) or
    /// with reinforcement disabled.
    void fire_reinforcements_();

    /// Aircraft pool snapshot for one team (roster-backed when nonzero,
    /// else the campaign source's per-team count).
    [[nodiscard]] int team_aircraft_pool_(int team_slot) const;

    /// C3: select the target objective for `team` — enemy-owned
    /// (either stance direction hostile), ranked priority-desc then
    /// wire-order-asc. 0 when no enemy objective exists.
    [[nodiscard]] std::uint32_t select_target_(std::uint8_t team) const;

    /// G2: select the interdiction target for `team` — an enemy
    /// battalion, front-line ranked (rank_battalion_targets over the
    /// shared FLOT), rotation-spread by unit_target_cursor_. 0 when
    /// no ranked target exists (no war pair, no front, no battalions).
    [[nodiscard]] std::uint32_t select_unit_target_(
        std::uint8_t team);

    const f4::world::ICampaignSource& camp_;
    const f4::world::ITeamSource& teams_;
    const f4::world::IUnitCoreSource& units_;
    const MissionProfileTable& profiles_;
    f4::messaging::MessageBus& bus_;
    CampaignConfig cfg_;

    CampaignTime clock_{0};
    int cycles_fired_{0};
    CampaignTime next_cycle_{0};   ///< due time of the next tasking cycle

    /// Live aircraft pool per team slot, deducted by each generated
    /// mission (the attrition ledger B.3 builds on). Index = slot.
    /// NOT touched while a result ledger is attached (the ledger owns
    /// the tasking pool then — one pool, not two).
    std::vector<int> pool_;

    /// C2 reinforcement cadence state (absolute campaign times — the
    /// save's own epoch, bridged through ICampaignSource):
    ///   epoch_            — current_time at construction (the anchor's
    ///                       time base; the relative clock + epoch = now)
    ///   last_reinforce_  — the anchor from the .cmp header
    ///                       (last_reinforcement), advanced to "now" on
    ///                       each fire (FreeFalcon's catch-up-once shape
    ///                       — a stale anchor fires once, not N times).
    CampaignTime epoch_{0};
    CampaignTime last_reinforce_{0};
    int reinforcement_fires_{0};

    /// C1/C2 result ledger (optional, non-owning, MUTABLE — tasking
    /// writes draws into it) — when set, it IS the tasking pool. See
    /// set_result_ledger().
    CampaignResultLedger* result_ledger_ = nullptr;

    /// Aircraft available per squadron (parallel to squadrons_), updated
    /// as missions draw aircraft down.
    struct SquadronRef {
        std::uint32_t id_num;
        std::uint8_t owner;
        std::uint8_t specialty;
        std::string name;
        int available;             ///< aircraft still available this run
        std::uint32_t airbase;     ///< home airbase objective VU_ID.num
    };
    std::vector<SquadronRef> squadrons_;

    /// C3 route planner (optional, non-owning — the set_result_ledger
    /// pattern): when set, generated intents carry target + route.
    const RouteBuilder* route_planner_ = nullptr;
    const f4::world::IObjectiveSource* objectives_ = nullptr;

    /// G2 — per-team rotation cursor over the ranked enemy BATTALION
    /// list (the legacy ladder's unit-target spread; the ATM keeps its
    /// own — the two ladders never share cursors).
    std::array<int, 8> unit_target_cursor_{};

    /// C4: the ATM pipeline (constructed when cfg_.atm_pipeline — the
    /// set_result_ledger/set_route_planner attachments below keep its
    /// ledger/threat pointers current).
    std::unique_ptr<AirTaskingManager> atm_;

    /// Route QC counters (the summary block reads these).
    int routes_built_ = 0;
    int routes_failed_ = 0;
    int route_safe_searches_ = 0;
    int route_fallbacks_ = 0;

    /// C4: the flight-id counter (distinct from the package counter —
    /// multi-flight packages share package_id; flight ids stay unique
    /// for the spawner's duplicate guard).
    std::uint32_t next_flight_id_ = 0;

    /// All intents in publish order (mirrors what the bus saw).
    std::vector<MissionIntent> intents_;
};

// ============================================================================
// emit_flight_intents — MissionIntents from LIVE saved flights (B.3 tranche)
// ============================================================================
//
// The M4.7 Campaign::tick generates intents synthetically (profile ladder
// over squadron availability). Real campaign saves — TestCamp.cam et al. —
// already carry the output of FreeFalcon's ATM: 449 tasked Flight units
// with mission bytes, TOTs, targets, packages, squadrons and callsigns.
// This function turns those live flights into the SAME MissionIntent
// contract, so B.3's sim-side spawner consumes one message shape whether
// the tasking source is the synthetic ladder or a decoded save.
//
// Semantics (differences from the synthetic path, all deliberate):
//   * issued_time  — the `now` argument (callers pass the save's
//                    campaign.current_time; the flights predate it).
//   * time_on_target — the flight's ABSOLUTE CampaignTime as stored in the
//                    save (not rebased to campaign-relative). The spawner
//                    anchors campaign time itself; keeping the save's epoch
//                    intact preserves round-trip fidelity.
//   * flight_id / package_id — the flight's and its package's VU_ID.nums
//                    (the synthetic path's counter ids never appear).
//   * aircraft_count — the flight's roster, decoded as the same 2-bit
//                    group packing used for battalions (sum of groups;
//                    0xA0 = 4 aircraft). 0 when the save carries none.
//   * squadron_name / team_name — resolved from the unit + team data when
//                    the respective source pointers are non-null.
//
// Determinism: pure function of the sources — no RNG, no state.
[[nodiscard]] std::vector<MissionIntent>
emit_flight_intents(const f4::world::IUnitCoreSource& units,
                    const f4::world::IFlightSource& flights,
                    f4::messaging::MessageBus& bus,
                    CampaignTime now,
                    const f4::world::ITeamSource* teams = nullptr);

} // namespace f4::campaign
