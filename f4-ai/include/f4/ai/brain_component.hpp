// f4-ai/include/f4/ai/brain_component.hpp
//
// BrainComponent — the mission sequencer, as a BehavioralComponent.
//
// Phase DIGI-1: the brain sequences the mission's flight phases through
// their modules:
//
//   Ground   TakeoffModule       (taxi -> lineup -> takeoff -> departure)
//      |  is_complete() and route non-empty
//   Enroute  NavigationModule    (fly the scenario route; last wp = entry fix)
//      |  is_complete()
//   Approach LandingModule       (approach -> land -> rollout -> taxi-in -> park)
//      |  is_complete()
//   Complete (hold brakes)
//
// With an empty route the mission ends after takeoff (Phase A behavior).
// This is the sequential-handoff stepping stone toward the documented
// DigitalBrain (AI_IMPLEMENTATION_PLAN.md §5): a priority ladder
// arbitrates the modules instead of a fixed sequence. The M3 tactics
// landing adds the ladder's first rungs on top of the sequence —
// while Enroute, a visible incoming missile (MissileModule defeat)
// preempts a visible hostile fighter (BVRModule, handing off to
// WVRModule inside the 3 NM WVR entry band), which preempts the
// navigation module. The arbiter session completes the ladder's TOP:
// ground avoid and collision avoid preempt everything (FreeFalcon
// priorities 1-2, always armed — safety is not a tactic), and the
// fuel check (FrameExec step 2) stands the engagement rungs down at
// bingo. The mission sequence underneath is untouched.
//
// The brain runs in pass 1 (priority 100), reads the parent entity's
// aircraft state through the IAircraftState interface, calls the active
// module's update() to get an AIControlOutput, converts it to a PilotInput,
// and writes it to the IPilotInputSink interface. The flight model
// component then runs in pass 2 and integrates the FlightModel with that
// input.
//
// The brain resolves the interfaces LAZILY in update() and stores the
// owning EntityHandle BY VALUE (EntityHandle is a cheap value type: id +
// world pointer + cookie — see the regression note in entity.hpp's
// on_attached contract).
//
// If no IAircraftState or IPilotInputSink is present on the entity,
// update() is a no-op.
//
// Dependencies: f4-entities, f4-messaging, f4-flight-api. NOT
// f4-flight-model. C++20.

#pragma once

#include <f4/entities/entity.hpp>
#include <f4/messaging/bus.hpp>
#include <f4/flight/api/i_aircraft_state.hpp>
#include <f4/flight/api/i_pilot_input_sink.hpp>
#include <f4/flight/api/pilot_input.hpp>

#include <f4/geo/position.hpp>

#include "f4/ai/ai_output.hpp"
#include "f4/ai/modules/landing_module.hpp"
#include "f4/ai/modules/navigation_module.hpp"
#include "f4/ai/modules/takeoff_module.hpp"
#include "f4/ai/modules/bvr_module.hpp"
#include "f4/ai/modules/missile_module.hpp"
#include "f4/ai/modules/wvr_module.hpp"
#include "f4/ai/modules/wingman_module.hpp"
#include "f4/ai/modules/ground_avoid_module.hpp"
#include "f4/ai/modules/collision_avoid_module.hpp"
#include "f4/ai/sensor_fusion.hpp"

#include <optional>
#include <vector>

namespace f4::ai {

// ============================================================================
// MissionPlan — what the host (Simulation) injects into the brain at spawn.
// ============================================================================
struct MissionPlan {
    /// Air-phase route. Empty = no enroute/landing phases (takeoff-only
    /// mission). The LAST waypoint is the approach entry fix handed to
    /// LandingModule when NavigationModule completes.
    std::vector<modules::NavigationModule::Waypoint> route;

    /// Taxi-in route after landing rollout (runway exit -> parking).
    /// Empty = the aircraft parks on the runway.
    std::vector<geo::WorldPosition> taxi_in_route;

    /// Approach style at the end of the route: false = straight-in final
    /// (default), true = full traffic pattern (downwind/base/final).
    bool fly_traffic_pattern{false};

    /// Starting mission phase. Default Ground (taxi -> takeoff -> ...).
    /// Set to Approach to skip directly to the landing module — used by
    /// the isolated `landing_only` diagnostic scenario which spawns the
    /// aircraft already on final (see FLIGHT_CONTROL_NEXT_STEPS.md §3.2).
    /// When set to Approach, the host MUST also set the route's last
    /// waypoint at the approach entry fix — the brain uses that position
    /// to configure the LandingModule.
    /// Set to Enroute to skip taxi/takeoff and hand the route straight to
    /// the NavigationModule — used by the `course_intercept` and
    /// `standard_rate_turn` LNAV diagnostic scenarios which spawn the
    /// aircraft airborne, trimmed, and (for course_intercept) offset from
    /// the course to test intercept geometry (NAV-D1).
    enum class StartPhase { Ground, Approach, Enroute };
    StartPhase start_phase{StartPhase::Ground};
};

// ============================================================================
// CombatIntent — what the brain wants done to the real combat hardware
// this tick (read by the host's combat driver; see combat_bridge.hpp).
//
// The brain's tactic modules are engine-agnostic: they cannot lock a radar
// or drop a missile off a rail (those are f4-sensors / f4-weapons calls,
// which f4-ai must never link). Instead the modules set INTENTS, the brain
// collects them here, and the HOST layer converts them into
// RadarSimComponent::command_track() / weapons::launch_missile() through
// the simulation's own weapon table.
// ============================================================================
struct CombatIntent {
    /// Hold STT on this target (EntityId::value; 0 = no lock wanted).
    bool   radar_lock{false};
    std::uint64_t lock_target_id{0};
    /// Release the selected weapon this tick (one-tick pulse).
    bool   weapon_release{false};
    std::uint64_t release_target_id{0};
    /// GUNS: begin a burst at the aim target this tick (one-tick pulse —
    /// the rising edge of the trigger; the host driver converts it into
    /// GunStream::start_burst + a gun-station debit). The trigger is
    /// held through the burst's flight time by the GunModule's state
    /// machine — this pulse is the host's only required signal.
    bool   gun_trigger{false};
    std::uint64_t gun_target_id{0};
};

// ============================================================================
// BrainComponent
// ============================================================================
class BrainComponent : public entities::BehavioralComponent<BrainComponent> {
public:
    /// Mission phase — which module currently flies the aircraft.
    enum class Phase {
        Ground,     // TakeoffModule
        Enroute,    // NavigationModule (or the combat ladder, see below)
        Approach,   // LandingModule
        Complete    // parked / no further control
    };

    /// Combat mode — the first rungs of the DigitalBrain priority ladder
    /// (AI_IMPLEMENTATION_PLAN.md §5 Step 12):
    ///   Defensive > WVR > BVR > Wingman-formation > mission module.
    /// The BVR/WVR split is the plan's range-band rule (Step 9): inside
    /// the WVR entry band (3 NM) WVRModule owns the fight; past the WVR
    /// exit ring (wvr config, 4.5 NM hysteresis) BVRModule takes it
    /// back. The Formation rung (Step 11) fills the None slot for
    /// wingmen with a live lead picture — it runs with combat OFF too
    /// (a wingman follows its lead regardless of ROE). Collision/
    /// ground-avoid rungs arrive with their modules; every rung only
    /// ever PREEMPTS, never replaces, the mission sequence below it.
    enum class CombatMode {
        None,        // combat off, or nothing visible — mission module flies
        BVR,         // BVRModule engaged (Entering/Employing/Separating)
        WVR,         // WVRModule engaged (Merge/Offensive/Defensive/BugOut)
        Defensive,   // MissileModule beaming an incoming hostile missile
        Formation    // WingmanModule keeping formation on the flight lead
    };

    /// Safety mode — the ladder's TOP rungs (FreeFalcon priorities 1-2:
    /// GroundAvoidMode, CollisionAvoidMode). They run REGARDLESS of the
    /// combat flag (safety is not a tactic) and preempt every combat
    /// rung — FreeFalcon's digi puts both above even MissileDefeat. Run
    /// while Enroute only: a landing aircraft's last 1,500 ft belong to
    /// the LandingModule (DecisionLogic's "GroundCheck if not
    /// LandingMode" rule), and Ground aircraft don't fly.
    enum class SafetyMode {
        None,
        GroundAvoid,      // GroundAvoidModule flying the terrain pull-up
        CollisionAvoid    // CollisionAvoidModule flying the break
    };

    /// Fuel state — FreeFalcon FrameExec step 2 (FuelCheck): joker =
    /// "time to start thinking about home" (reporting only), bingo =
    /// "fight's over, bug out" (the engagement rungs stand down; a
    /// bingoing jet still DEFENDS an incoming missile — MissileDefeat
    /// outranks RTB in the FreeFalcon ladder — and it still keeps
    /// formation). No refueling exists yet, so state only ever moves
    /// down (fuel monotonically burns); RTB itself is the Navigation
    /// Module continuing the scenario route (its last waypoints ARE the
    /// way home — an airbase-divert model arrives with the campaign).
    enum class FuelState {
        Normal,   // above joker (or no policy set)
        Joker,    // below joker — reported, fight continues
        Bingo     // below bingo — engagement stands down
    };

    BrainComponent() = default;

    // --- BehavioralComponent overrides ---
    int priority() const noexcept override {
        return entities::update_phase::BRAIN_PRIORITY;  // 100, pass 1
    }

    // Capture the owning EntityHandle so we can look up sibling
    // components on demand. EntityHandle is a lightweight VALUE type
    // (id + world pointer + cookie) — we store it BY VALUE. Storing a
    // pointer to the `self` reference would dangle: `self` aliases the
    // caller's handle (usually a stack local in spawn code) that dies as
    // soon as the spawning function returns.
    void on_attached(entities::EntityHandle& self) override {
        owner_ = self;
    }

    // --- Dormancy (parked inventory) ------------------------------------
    // Set by the squadron spawner for un-tasked parked airframes. A
    // dormant brain publishes nothing and flies nothing; the entity is
    // still rendered (Transform/VisualModel are passive).
    void set_dormant(bool d) noexcept { dormant_ = d; }
    [[nodiscard]] bool is_dormant() const noexcept { return dormant_; }

    void update(double dt, messaging::MessageBus& bus) override {
        if (!owner_.valid()) return;  // not attached to any entity — no-op

        // Dormant: parked squadron inventory, no mission to fly. The
        // brain of an un-tasked parked airframe would otherwise run the
        // full module ladder (interface resolution + module updates)
        // every tick for ~1,000 airframes in a populated campaign save
        // — pure waste, since a dormant brain has no taxi request to
        // publish and no route to follow. See FlightModelComponent's
        // dormant_ for the matching physics-side skip.
        if (dormant_) return;

        // Lazily resolve the flight model interfaces (see entity.hpp for
        // why resolution is per-tick rather than cached).
        auto* state = owner_.get_interface<flight::IAircraftState>();
        auto* sink  = owner_.get_interface<flight::IPilotInputSink>();
        if (!state || !sink) return;  // no flight model on this entity

        // The gauge reading for this tick's fuel check (FrameExec step 2).
        state_fuel_hint_ = state->fuel_lbs();

        // Initialize the TakeoffModule on first update (it needs the bus
        // to publish TaxiRequest / subscribe to clearances).
        if (!takeoff_initialized_) {
            auto* world = owner_.world();
            if (!world) return;
            takeoff_.initialize(owner_.id().value, *world, bus);
            takeoff_initialized_ = true;
        }

        // Phase 0c (isolated scenarios): if the mission plan says to start
        // in Approach, skip the takeoff/navigation phases entirely. The
        // host spawns the aircraft airborne on final, and the brain hands
        // off directly to the LandingModule (configured with the route's
        // last waypoint as the approach entry fix). Used by the
        // `landing_only` diagnostic scenario.
        if (phase_ == Phase::Ground &&
            plan_.start_phase == MissionPlan::StartPhase::Approach &&
            !plan_.route.empty()) {
            auto* world = owner_.world();
            if (world) {
                const auto& entry_fix = plan_.route.back().position;
                landing_.configure(entry_fix, plan_.taxi_in_route);
                landing_.fly_traffic_pattern = plan_.fly_traffic_pattern;
                landing_.air_steering.reset_integrators();
                landing_.pattern_steering.reset_integrators();
                landing_.initialize(owner_.id().value, *world, bus);
                phase_ = Phase::Approach;
            } else {
                phase_ = Phase::Complete;
            }
        }

        // NAV-D1: airborne-spawn Enroute start — hand the route straight
        // to the NavigationModule without waiting for a takeoff that
        // already "happened" before the scenario begins.
        if (phase_ == Phase::Ground &&
            plan_.start_phase == MissionPlan::StartPhase::Enroute &&
            !plan_.route.empty()) {
            nav_.set_route(plan_.route);
            nav_.air_steering.reset_integrators();
            phase_ = Phase::Enroute;
        }

        // Sequence the mission phases.
        if (phase_ == Phase::Ground && takeoff_.is_complete()) {
            if (!plan_.route.empty()) {
                nav_.set_route(plan_.route);
                nav_.air_steering.reset_integrators();
                phase_ = Phase::Enroute;
            } else {
                phase_ = Phase::Complete;
            }
        }
        if (phase_ == Phase::Enroute && nav_.is_complete()) {
            auto* world = owner_.world();
            if (world) {
                // The route's last waypoint is the approach entry fix.
                const auto& entry_fix = plan_.route.back().position;
                landing_.configure(entry_fix, plan_.taxi_in_route);
                landing_.fly_traffic_pattern = plan_.fly_traffic_pattern;
                // Reset the LandingModule's air_steering integrators BEFORE
                // initialize() — the NavigationModule's air_steering has
                // accumulated VS-rate state that, if carried into the
                // LandingModule, produces a massive transient on the first
                // tick (prev_vs_fpm_ = 0, actual VS = 4000+ → huge damping
                // spike that drives the aircraft into the ground).
                landing_.air_steering.reset_integrators();
                landing_.pattern_steering.reset_integrators();
                landing_.initialize(owner_.id().value, *world, bus);
                phase_ = Phase::Approach;
            } else {
                phase_ = Phase::Complete;
            }
        }
        if (phase_ == Phase::Approach && landing_.is_complete()) {
            phase_ = Phase::Complete;
        }

        // =====================================================================
        // SAFETY LADDER (the DigitalBrain arbiter's top rungs — FreeFalcon
        // priorities 1-2): GroundAvoid > CollisionAvoid. They run while
        // Enroute REGARDLESS of the combat flag (safety is not a tactic)
        // and preempt every combat rung — FreeFalcon's digi puts both
        // above even MissileDefeat. Approach-phase aircraft are excluded:
        // the landing module owns the last 1,500 ft (DecisionLogic's
        // "GroundCheck if not LandingMode" rule); Ground aircraft don't
        // fly. Both modules' pictures are pushed by the HOST each tick
        // (engine-agnostic: the modules cannot scan terrain or traffic).
        // =====================================================================
        AIControlOutput ai_out{};
        combat_intent_ = CombatIntent{};
        combat_mode_ = CombatMode::None;
        safety_mode_ = SafetyMode::None;

        // Fuel check (FrameExec step 2): before any rung, so a bingoing
        // jet's engagement stands down this tick. Enroute only — an
        // aircraft already on approach is committed, and the parked/
        // ground phases have no decisions left to gate.
        update_fuel_state();

        if (phase_ == Phase::Enroute) {
            const auto ga_out = ground_avoid_.update(dt, state,
                                                     terrain_picture_);
            if (ground_avoid_.pulling_up()) {
                safety_mode_ = SafetyMode::GroundAvoid;
                ai_out = ga_out;
            } else {
                // The committed firing pass: the intruder our WVR gun
                // fire control is fighting inside the employment band is
                // exempt (gun_pass_exempt_id_, written by the combat
                // ladder below — one tick stale; the band develops over
                // seconds, a tick is noise). See collision_avoid_module
                // .hpp for the doctrine + the documented midair trade.
                collision_avoid_.set_exempt_id(gun_pass_exempt_id_);
                const auto ca_out = collision_avoid_.update(dt, state);
                if (collision_avoid_.is_avoiding()) {
                    safety_mode_ = SafetyMode::CollisionAvoid;
                    ai_out = ca_out;
                }
            }
        }

        // =====================================================================
        // Combat ladder (M3 tactics — the DigitalBrain priority ladder's
        // first rungs; preempts the mission module, never replaces it):
        //   Defensive > WVR > BVR > mission module.
        // Runs only while Enroute: Ground aircraft do not fight, and an
        // aircraft on Approach has already disengaged. When combat ends
        // (target dead / lost) the navigation module resumes — with its
        // steering integrators reset, the same transient guard the phase
        // handoffs use (BVR's throttle/VS state is not nav state).
        // =====================================================================
        if (safety_mode_ == SafetyMode::None && combat_enabled_ &&
            phase_ == Phase::Enroute) {
            auto* world = owner_.world();
            // The firing-pass exemption starts each tick clean; the
            // engagement branch below may arm it (a WVR target inside
            // the gun band). A bingo/defending jet carries no exemption.
            gun_pass_exempt_id_ = 0;
            if (world) {
                if (!combat_initialized_) {
                    sensors_.initialize(owner_.id().value, *world, bus,
                                        SkillLevel::Veteran);
                    combat_initialized_ = true;
                }
                // Per the skill interval (Veteran = 5 s)...
                sensors_.update(dt);
                // ...but a beam fight needs a fresh picture: while an
                // incoming hostile missile is visible, refresh every tick
                // (the stale entry itself keeps the refresh armed — see
                // MissileModule's defeat-linger note for the tail end).
                if (sensors_.missile_threat() != nullptr) {
                    sensors_.force_refresh();
                }

                if (const auto* incoming = sensors_.missile_threat()) {
                    // Missile defense outranks even bingo (FreeFalcon's
                    // ladder: MissileDefeat(4) > RTB(19)) — a jet bug
                    // fuel home still dodges the missile chasing it.
                    combat_mode_ = CombatMode::Defensive;
                    ai_out = missile_defense_.update(dt, state, incoming);
                } else if (fuel_bingo_) {
                    // BINGO (FrameExec step 2's fight-gating half): the
                    // engagement rungs stand down — no new target
                    // selection, no employment. A running fight ends
                    // cleanly: reset the engagement state so the bvr/wvr
                    // modules do not resume stale geometry if fuel
                    // (impossibly) recovers; the nav-resume bookkeeping
                    // below handles the steering transient.
                    if (in_wvr_) {
                        in_wvr_ = false;
                        wvr_.reset();
                    }
                    bvr_.reset();
                } else {
                    // WINGMAN SORT (Step 11): a wingman engages through
                    // sorted_threat_target — prefer the bandit the LEAD has
                    // NOT taken; support the lead's kill when there is no
                    // free bandit. Non-wingmen keep the plain query. The
                    // lead's engagement arrives via set_lead_engagement()
                    // (the host reads the lead brain each tick).
                    const auto* tgt = is_wingman_
                        ? sensors_.sorted_threat_target(lead_engaged_id_)
                        : sensors_.threat_target();
                    // --- BVR <-> WVR band handoff (plan Step 9) ----------
                    // Entry is BVRModule's band constant (the range
                    // taxonomy lives there); exit is WVRModule's (the
                    // hysteresis ring). One source per boundary.
                    if (tgt != nullptr) {
                        if (in_wvr_) {
                            if (tgt->range_nm >
                                    wvr_.config().wvr_exit_range_nm) {
                                in_wvr_ = false;
                                wvr_.reset();  // clean handback to BVR
                            }
                        } else if (tgt->range_nm <
                                       bvr_.config().wvr_entry_range_nm) {
                            in_wvr_ = true;
                            bvr_.reset();  // fresh engagement on return
                        }
                    } else if (in_wvr_) {
                        in_wvr_ = false;
                        wvr_.reset();
                    }

                    if (tgt != nullptr && in_wvr_) {
                        combat_mode_ = CombatMode::WVR;
                        ai_out = wvr_.update(dt, state, tgt);
                        combat_intent_.radar_lock = wvr_.wants_lock();
                        combat_intent_.lock_target_id =
                            wvr_.lock_target_id();
                        combat_intent_.weapon_release =
                            wvr_.release_pulse() && !hold_fire_;
                        combat_intent_.release_target_id =
                            wvr_.release_target_id();
                        // Guns (Steps 11-12): the burst edge rides the
                        // same intent surface. hold_fire disarms it at the
                        // brain gate; the guns-tight doctrine (scenario
                        // guns_hold) is enforced down in the module's
                        // fire control, where it can also gate
                        // should_fire() and burn no phantom budget.
                        combat_intent_.gun_trigger =
                            wvr_.gun_pulse() && !hold_fire_;
                        combat_intent_.gun_target_id =
                            wvr_.gun_target_id();
                    } else if (tgt != nullptr) {
                        combat_mode_ = CombatMode::BVR;
                        ai_out = bvr_.update(dt, state, tgt);
                        // Intents out: the host driver executes these
                        // against the real radar / weapon store after
                        // update_all. BVR release honors the BVR hold
                        // (SPINS: radar missiles tight) separately from
                        // the all-weapons hold.
                        combat_intent_.radar_lock = bvr_.wants_lock();
                        combat_intent_.lock_target_id =
                            bvr_.lock_target_id();
                        combat_intent_.weapon_release =
                            bvr_.release_pulse() && !hold_fire_ &&
                            !bvr_hold_;
                        combat_intent_.release_target_id =
                            bvr_.release_target_id();
                    }

                    // The committed firing pass exemption source: the
                    // WVR's gun steering target (FRESH track-file range,
                    // via gun_pass_target_id — the fusion's range_nm is
                    // seconds stale at merge closure). Both sides of a
                    // fight compute it independently (each brain's WVR
                    // target is the other jet) — so a merge holds its
                    // geometry while the weapons resolve it, and
                    // everything OUTSIDE the band keeps full collision
                    // protection.
                    gun_pass_exempt_id_ =
                        (combat_mode_ == CombatMode::WVR)
                            ? wvr_.gun_pass_target_id()
                            : 0;
                }
            }
        }

        // =================================================================
        // Formation rung (Step 11): the wingman's slot in the ladder —
        // BELOW every combat rung (a fighting wingman stops forming and
        // fights: BVR/WVR/Defensive preempt the module), ABOVE the mission
        // modules (a wingman with a live lead picture never flies its own
        // route). Runs with combat disabled too — formation keeping is
        // not a combat behavior (and survives bingo — a wingman on the
        // way home still flies formation). When the picture drops (lead
        // dead / landed / unresolvable), the rung empties and the mission
        // module takes over: the wingman becomes a single-ship. Safety
        // rungs preempt it like everything else — you do not form on a
        // lead through a terrain ridge.
        // =================================================================
        if (safety_mode_ == SafetyMode::None &&
            combat_mode_ == CombatMode::None && is_wingman_ &&
            phase_ == Phase::Enroute && wingman_.has_live_picture()) {
            combat_mode_ = CombatMode::Formation;
            ai_out = wingman_.update(dt, state);
        }

        // Ladder bookkeeping (single source, every rung — safety,
        // combat, formation): falling to None from ANY active rung —
        // the pull-up flown, the break complete, BVR separated, target
        // lost, lead gone — hands the route back to the navigation
        // module with clean steering integrators (the same transient
        // guard the phase handoffs use; combat/formation/safety state
        // is not nav state).
        const bool ladder_active =
            safety_mode_ != SafetyMode::None ||
            combat_mode_ != CombatMode::None;
        if (!ladder_active && combat_was_active_) {
            nav_.air_steering.reset_integrators();
        }
        combat_was_active_ = ladder_active;

        // Run the active module: produce AIControlOutput from the state.
        // (Skipped whenever a combat OR safety rung produced an output
        // this tick — including the Defensive override and the safety
        // pull-up/break, which preempt everything.)
        if (safety_mode_ == SafetyMode::None &&
            combat_mode_ == CombatMode::None) {
            switch (phase_) {
                case Phase::Ground:
                    ai_out = takeoff_.update(dt, state);
                    break;
                case Phase::Enroute:
                    ai_out = nav_.update(dt, state);
                    break;
                case Phase::Approach:
                    ai_out = landing_.update(dt, state);
                    break;
                case Phase::Complete:
                    ai_out = landing_.hold_complete();  // brakes on, gear down
                    break;
            }
        }

        // Watchdog (Phase 5a): if the AI produced an empty output (no
        // module set anything), hold the last known good PilotInput
        // rather than letting the FM fly idle for that tick. The default
        // PilotInput{} has throttle=0 and gear=down — safe on the ground,
        // catastrophic in flight (a 1-tick idle transient at low altitude
        // is enough to drop the aircraft into the ground).
        //
        // An output is "empty" if NONE of the meaningful control fields
        // were set. The Complete phase sets brakes+gear explicitly, so
        // it's not affected. Empty outputs happen during phase transitions
        // (e.g. NavigationModule Done before BrainComponent sequences to
        // Approach) or if a module returns {} by mistake.
        const bool empty = (ai_out.pitch_cmd == 0.0 &&
                            ai_out.roll_cmd == 0.0 &&
                            ai_out.yaw_cmd == 0.0 &&
                            ai_out.throttle_cmd == 0.0 &&
                            !ai_out.gear_handle_down &&
                            !ai_out.wheel_brakes &&
                            !ai_out.parking_brake);
        if (empty && last_pilot_input_.has_value() && phase_ != Phase::Complete) {
            sink->set_pending_input(*last_pilot_input_);
            return;
        }

        // Write the AI output to the flight model's pending input slot
        // via the IPilotInputSink interface, and cache it for the watchdog.
        const flight::PilotInput pi = map_to_pilot_input(ai_out);
        last_pilot_input_ = pi;
        sink->set_pending_input(pi);
    }

    // --- Mission plan (set by the host at spawn, before first tick) ---
    void set_mission_plan(MissionPlan plan) { plan_ = std::move(plan); }
    [[nodiscard]] const MissionPlan& mission_plan() const noexcept { return plan_; }

    // --- Phase / state reporting (HUD + recorder) ---
    [[nodiscard]] Phase phase() const noexcept { return phase_; }
    [[nodiscard]] const char* phase_name() const noexcept {
        switch (phase_) {
            case Phase::Ground:   return "Ground";
            case Phase::Enroute:  return "Enroute";
            case Phase::Approach: return "Approach";
            case Phase::Complete: return "Complete";
        }
        return "?";
    }
    /// Active module's mode name (e.g. "TakeoffMode"); "Complete" at the end.
    /// While a safety rung is active its name is reported first (the
    /// mission module is dormant); then the COMBAT mode ("BVREngage" /
    /// "WVREngage" / "MissileDefeat" / "WingmanFormation"); a bingoing
    /// enroute jet with nothing else active reports "RTB".
    [[nodiscard]] std::string mode_name() const {
        if (safety_mode_ == SafetyMode::GroundAvoid)
            return "GroundAvoid";
        if (safety_mode_ == SafetyMode::CollisionAvoid)
            return "CollisionAvoid";
        if (combat_mode_ == CombatMode::BVR)         return "BVREngage";
        if (combat_mode_ == CombatMode::WVR)         return "WVREngage";
        if (combat_mode_ == CombatMode::Defensive)   return "MissileDefeat";
        if (combat_mode_ == CombatMode::Formation)   return "WingmanFormation";
        if (fuel_bingo_ && phase_ == Phase::Enroute) return "RTB";
        switch (phase_) {
            case Phase::Ground:   return takeoff_.mode_name();
            case Phase::Enroute:  return nav_.mode_name();
            case Phase::Approach: return landing_.mode_name();
            case Phase::Complete: return "MissionComplete";
        }
        return {};
    }
    /// Active module's state name (e.g. "OnFinal"); "Parked" at the end.
    /// While fighting: the BVR/WVR state / "Defending"; in formation:
    /// the wingman state ("Following" / "Rejoining"); in a safety
    /// recovery: "PullingUp" / "Breaking"; at bingo: the fuel state.
    [[nodiscard]] std::string state_name() const {
        if (safety_mode_ == SafetyMode::GroundAvoid)
            return "PullingUp";
        if (safety_mode_ == SafetyMode::CollisionAvoid)
            return "Breaking";
        if (combat_mode_ == CombatMode::BVR)         return bvr_.state_name();
        if (combat_mode_ == CombatMode::WVR)         return wvr_.state_name();
        if (combat_mode_ == CombatMode::Defensive)   return "Defending";
        if (combat_mode_ == CombatMode::Formation)   return wingman_.state_name();
        if (fuel_bingo_ && phase_ == Phase::Enroute) return fuel_state_name();
        switch (phase_) {
            case Phase::Ground:   return takeoff_.state_name();
            case Phase::Enroute:  return nav_.state_name();
            case Phase::Approach: return landing_.state_name();
            case Phase::Complete: return "Parked";
        }
        return {};
    }

    // --- Combat chain (M3 tactics; see CombatIntent above) ----------------
    /// Enable the combat ladder. The host sets this at spawn when the
    /// scenario's combat block is on, then installs a detection policy
    /// on sensors() (the radar-backed adapter lives in f4-simulation).
    void set_combat_enabled(bool on) noexcept { combat_enabled_ = on; }
    [[nodiscard]] bool combat_enabled() const noexcept { return combat_enabled_; }
    [[nodiscard]] CombatMode combat_mode() const noexcept { return combat_mode_; }
    [[nodiscard]] SafetyMode safety_mode() const noexcept { return safety_mode_; }
    [[nodiscard]] const char* safety_mode_name() const noexcept {
        switch (safety_mode_) {
            case SafetyMode::None:            return "None";
            case SafetyMode::GroundAvoid:     return "GroundAvoid";
            case SafetyMode::CollisionAvoid:  return "CollisionAvoid";
        }
        return "?";
    }
    [[nodiscard]] const char* combat_mode_name() const noexcept {
        switch (combat_mode_) {
            case CombatMode::None:      return "None";
            case CombatMode::BVR:       return "BVREngage";
            case CombatMode::WVR:       return "WVREngage";
            case CombatMode::Defensive: return "MissileDefeat";
            case CombatMode::Formation: return "WingmanFormation";
        }
        return "?";
    }
    /// EntityId::value of the hostile this brain's combat ladder is
    /// CURRENTLY engaging (the target BVR/WVR is steering against), 0 when
    /// not fighting. Read by the host each tick to build WINGMAN sort
    /// hints (the #2 engages the bandit the lead has not taken — see
    /// SensorFusion::sorted_threat_target).
    [[nodiscard]] std::uint64_t combat_engagement_id() const noexcept {
        if (combat_mode_ == CombatMode::BVR) return bvr_.lock_target_id();
        if (combat_mode_ == CombatMode::WVR) return wvr_.lock_target_id();
        return 0;
    }
    /// ROE: weapons hold (all rungs — the aircraft fights geometry only,
    /// never releases). The scenario's per-aircraft "hold_fire".
    void set_hold_fire(bool on) noexcept { hold_fire_ = on; }
    [[nodiscard]] bool hold_fire() const noexcept { return hold_fire_; }
    /// ROE: radar-missile tight — BVR employment suppressed, WVR heaters
    /// still employ. The scenario combat block's "bvr_hold" (SPINS-style
    /// weapons tight for the BVR fight only).
    void set_bvr_hold(bool on) noexcept { bvr_hold_ = on; }
    [[nodiscard]] bool bvr_hold() const noexcept { return bvr_hold_; }
    /// This tick's combat intents (lock + weapon release) — the host's
    /// combat driver reads this AFTER world.update_all() each tick.
    [[nodiscard]] const CombatIntent& combat_intent() const noexcept {
        return combat_intent_;
    }

    // --- Module access (host configuration + test inspection) ---
    [[nodiscard]] modules::TakeoffModule&       takeoff()       noexcept { return takeoff_; }
    [[nodiscard]] const modules::TakeoffModule& takeoff() const noexcept { return takeoff_; }
    [[nodiscard]] modules::NavigationModule&       navigation()       noexcept { return nav_; }
    [[nodiscard]] const modules::NavigationModule& navigation() const noexcept { return nav_; }
    [[nodiscard]] modules::LandingModule&       landing()       noexcept { return landing_; }
    [[nodiscard]] const modules::LandingModule& landing() const noexcept { return landing_; }

    // --- Combat module access (host configuration + test inspection) ---
    [[nodiscard]] SensorFusion&       sensors()       noexcept { return sensors_; }
    [[nodiscard]] const SensorFusion& sensors() const noexcept { return sensors_; }
    [[nodiscard]] modules::BVRModule&       bvr()       noexcept { return bvr_; }
    [[nodiscard]] const modules::BVRModule& bvr() const noexcept { return bvr_; }
    [[nodiscard]] modules::WVRModule&       wvr()       noexcept { return wvr_; }
    [[nodiscard]] const modules::WVRModule& wvr() const noexcept { return wvr_; }
    [[nodiscard]] modules::MissileModule&       missile_defense()       noexcept { return missile_defense_; }
    [[nodiscard]] const modules::MissileModule& missile_defense() const noexcept { return missile_defense_; }

    // --- Safety modules (host picture pushes + test inspection) ---
    /// Per-tick terrain picture for the ground-avoid rung (the host's
    /// eyes: elevation under + ahead of the jet, from the same
    /// TerrainSource the FM's ground plane uses). No push = no picture
    /// = the rung idles (standalone brains never pull up).
    void update_terrain_picture(
        const modules::GroundAvoidModule::TerrainPicture& p) {
        terrain_picture_ = p;
    }
    /// Per-tick traffic picture for the collision-avoid rung: intruders
    /// within the host's gate (ALL airborne aircraft — friendlies
    /// included) + the OWN velocity from the same transform snapshot.
    void update_traffic(
        std::vector<modules::CollisionAvoidModule::Intruder> traffic,
        std::optional<geo::WorldPosition> own_velocity) {
        collision_avoid_.set_traffic(std::move(traffic), own_velocity);
    }
    [[nodiscard]] modules::GroundAvoidModule&       ground_avoid()       noexcept { return ground_avoid_; }
    [[nodiscard]] const modules::GroundAvoidModule& ground_avoid() const noexcept { return ground_avoid_; }
    [[nodiscard]] modules::CollisionAvoidModule&       collision_avoid()       noexcept { return collision_avoid_; }
    [[nodiscard]] const modules::CollisionAvoidModule& collision_avoid() const noexcept { return collision_avoid_; }

    // --- Fuel policy (FrameExec step 2; the host sets it at spawn) ---
    /// Jokers/bingos in pounds of usable fuel (0 = that threshold is
    /// disabled; a 0 bingo means the brain never gates on fuel). Set
    /// from the scenario's fuel block by the host.
    void set_fuel_policy(double joker_lbs, double bingo_lbs) noexcept {
        fuel_joker_lbs_ = joker_lbs;
        fuel_bingo_lbs_ = bingo_lbs;
    }
    [[nodiscard]] FuelState fuel_state() const noexcept {
        return fuel_state_;
    }
    [[nodiscard]] const char* fuel_state_name() const noexcept {
        switch (fuel_state_) {
            case FuelState::Normal: return "Normal";
            case FuelState::Joker:  return "Joker";
            case FuelState::Bingo:  return "Bingo";
        }
        return "?";
    }
    /// The last fuel reading (lbs; negative = never measured).
    [[nodiscard]] double fuel_lbs() const noexcept { return fuel_lbs_; }

    // --- Wingman role (Step 11; the host drives this at spawn + per tick) ---
    /// Make this brain a WINGMAN of the flight lead `lead_entity_id`
    /// (0 clears the role). Marks the aircraft: the ladder gains the
    /// Formation rung and the combat rungs pick targets through the SORT
    /// (sorted_threat_target) instead of the plain threat query.
    void set_flight_lead(std::uint64_t lead_entity_id) noexcept {
        is_wingman_ = lead_entity_id != 0;
        lead_id_ = lead_entity_id;
        if (!is_wingman_) wingman_.reset();
    }
    [[nodiscard]] bool is_wingman() const noexcept { return is_wingman_; }
    [[nodiscard]] std::uint64_t flight_lead_id() const noexcept { return lead_id_; }
    /// Push the lead's kinematic picture for THIS tick (the host calls it
    /// every tick BEFORE world update: the module is engine-agnostic, it
    /// cannot read the lead's transform itself). An invalid picture
    /// (dead / landed lead) drops the Formation rung for that tick.
    void update_lead_picture(
        const modules::WingmanModule::LeadPicture& p) {
        wingman_.set_lead_picture(p);
    }
    /// Push the lead's current combat engagement target (the host reads
    /// the LEAD brain's combat_engagement_id() each tick). Feeds the sort:
    /// the wingman prefers the bandit the lead has NOT taken.
    void set_lead_engagement(std::uint64_t engaged_id) noexcept {
        lead_engaged_id_ = engaged_id;
    }
    [[nodiscard]] modules::WingmanModule&       wingman()       noexcept { return wingman_; }
    [[nodiscard]] const modules::WingmanModule& wingman() const noexcept { return wingman_; }

    /// Legacy alias for the Phase A API (tests + hosts configure the
    /// takeoff module through this).
    [[nodiscard]] modules::TakeoffModule&       module()       noexcept { return takeoff_; }
    [[nodiscard]] const modules::TakeoffModule& module() const noexcept { return takeoff_; }

private:
    // Fuel check (FrameExec step 2). Pure function of the gauge: compare
    // the total usable fuel against the policy, worst state wins. Called
    // from update() every tick; no policy (bingo=0) leaves Normal.
    void update_fuel_state() {
        if (phase_ != Phase::Enroute) return;  // committed or parked
        fuel_lbs_ = state_fuel_hint_;
        fuel_bingo_ = fuel_bingo_lbs_ > 0.0 && fuel_lbs_ <= fuel_bingo_lbs_;
        fuel_state_ = fuel_bingo_ ? FuelState::Bingo
            : (fuel_joker_lbs_ > 0.0 && fuel_lbs_ <= fuel_joker_lbs_)
                ? FuelState::Joker
                : FuelState::Normal;
    }

    // Map AIControlOutput to PilotInput. The brain is self-contained —
    // no external runner is needed to translate AI output to pilot input.
    static flight::PilotInput map_to_pilot_input(const AIControlOutput& ai_out) {
        flight::PilotInput pi;
        pi.pstick    = ai_out.pitch_cmd;
        pi.rstick    = ai_out.roll_cmd;
        pi.ypedal    = ai_out.yaw_cmd;
        pi.throttle  = ai_out.throttle_cmd;
        pi.speedBrake = ai_out.speed_brake_cmd;
        pi.gearHandle = ai_out.gear_handle_down ? 1.0 : -1.0;
        pi.wheelBrakes = ai_out.wheel_brakes;
        pi.parkingBrake = ai_out.parking_brake;
        // Phase C1: forward flap commands to the FM. The FM already actuates
        // tefPos/lefPos from these fields (flight_model.cpp:453-454); before
        // this wiring the AI never set them so the aircraft always flew with
        // flaps retracted — causing 60+ kt excess approach speed and
        // doubling the landing roll distance.
        pi.tefCmd = ai_out.tef_cmd;
        pi.lefCmd = ai_out.lef_cmd;
        pi.noseSteerOn = true;  // always on for AI
        pi.validate();
        return pi;
    }

    entities::EntityHandle owner_{};

    // Dormant: skip update() entirely (parked squadron inventory).
    bool dormant_{false};

    // Mission plan + sequencing.
    MissionPlan plan_;
    Phase phase_{Phase::Ground};

    // Phase modules. Only the takeoff module initializes up front; the
    // navigation module takes its route at handoff, and the landing module
    // initializes (bus subscriptions) at its handoff.
    modules::TakeoffModule takeoff_;
    bool takeoff_initialized_{false};
    modules::NavigationModule nav_;
    modules::LandingModule landing_;

    // Combat ladder (M3 tactics): SensorFusion (the eyes — the host
    // installs the detection policy), the BVR engagement module, the WVR
    // merge module (Step 9 — owns the fight inside the 3 NM entry band;
    // in_wvr_ is the band handoff flag with the wvr config's exit ring as
    // the hysteresis), and the defensive MissileModule. SensorFusion
    // initializes lazily on the first Enroute update (it needs the world
    // + bus).
    bool combat_enabled_{false};
    bool combat_initialized_{false};
    bool combat_was_active_{false};
    bool hold_fire_{false};
    bool bvr_hold_{false};
    bool in_wvr_{false};
    CombatMode combat_mode_{CombatMode::None};
    CombatIntent combat_intent_{};
    SensorFusion sensors_{};
    modules::BVRModule bvr_{};
    modules::WVRModule wvr_{};
    modules::MissileModule missile_defense_{};

    // Wingman role (Step 11): the formation module + the lead linkage the
    // host feeds (is_wingman_/lead_id_ at spawn; the lead picture + the
    // lead's engagement id per tick). lead_engaged_id_ is 0 whenever the
    // lead is not fighting — the sort then degenerates to the plain
    // threat query.
    bool is_wingman_{false};
    std::uint64_t lead_id_{0};
    std::uint64_t lead_engaged_id_{0};
    modules::WingmanModule wingman_{};

    // Safety ladder (the arbiter's top rungs): the terrain pull-up + the
    // mid-air break, and the pictures the host pushes each tick (the
    // modules are engine-agnostic — they cannot scan terrain or traffic).
    // No terrain picture ever pushed => GroundAvoid idles; no traffic
    // push => CollisionAvoid sees an empty sky (a brain the host never
    // feeds flies exactly as it did before these rungs existed).
    // gun_pass_exempt_id_: the committed firing pass (see the CA module
    // header) — the WVR gun target inside the employment band, written
    // by the combat ladder, read by the CA rung one tick later.
    SafetyMode safety_mode_{SafetyMode::None};
    modules::GroundAvoidModule::TerrainPicture terrain_picture_{};
    modules::GroundAvoidModule ground_avoid_{};
    modules::CollisionAvoidModule collision_avoid_{};
    std::uint64_t gun_pass_exempt_id_{0};

    // Fuel policy (FrameExec step 2): thresholds from the scenario (lbs,
    // 0 = off), the gauge reading each tick, and the derived state.
    // state_fuel_hint_ is set at the top of update() from the resolved
    // IAircraftState (kept as a member so the private helper reads it).
    double fuel_joker_lbs_{0.0};
    double fuel_bingo_lbs_{0.0};
    double fuel_lbs_{-1.0};
    double state_fuel_hint_{1.0e9};
    bool fuel_bingo_{false};
    FuelState fuel_state_{FuelState::Normal};

    // Watchdog cache: the last non-empty PilotInput the brain produced.
    // Held for one tick if a module returns an empty AIControlOutput
    // (e.g. during a phase transition) so the FM doesn't see a 1-tick
    // idle transient. See update() for the rationale.
    std::optional<flight::PilotInput> last_pilot_input_;
};

} // namespace f4::ai
