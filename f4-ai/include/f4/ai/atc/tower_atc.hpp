// f4-ai/include/f4/ai/atc/tower_atc.hpp
//
// TowerATC — a real air traffic controller for ONE airfield pattern.
//
// Tier 3 of the ATC roadmap: the sequencing tower that replaces the
// grant-everything StubATC. The StubATC answers every request immediately,
// which means two aircraft can be cleared onto the same runway at the same
// time — the exact behavior a tower exists to prevent. TowerATC owns the
// runway as a RESOURCE:
//
//   - At most ONE aircraft holds the runway claim at a time (the occupant).
//   - Requests that arrive while the runway is occupied are QUEUED (FIFO by
//     request time, departures and arrivals in ONE combined queue — the
//     Tier 3 priority policy; documented extension point below).
//   - The runway is released by the aircraft's own reports:
//       DepartureReport      (TakeoffModule, liftoff — Takeoff -> FlyOut)
//       RunwayVacatedReport  (LandingModule, Rollout -> TaxiIn)
//     by a go-around (GoAroundMessage releases a granted claim and removes
//     a queued arrival), or by an occupancy TIMEOUT against occupants that
//     vanish without reporting (combat kill, despawn, aborted roll).
//
// SWAP-IN CONTRACT (identical to the stub's): TowerATC implements the SAME
// message protocol over the MessageBus and the same configuration surface
// (set_airfield / set_airbase_airfield / set_tanker). The AI modules never
// know the difference — the swap happens at the MessageBus wiring level
// (Simulation::wire_atc), never in AI code. Clearances carry the same
// fields the stub fills, so module behavior, RadioLog transcripts, and all
// clearance-driven state machines run unchanged.
//
// DEFERRAL CONTRACT (the one behavioral difference): when the runway is
// occupied, TowerATC answers a request with SILENCE — no message — and the
// requesting aircraft keeps holding. This is already the modules' contract:
// TakeoffModule holds in HoldShort/Wait until a TakeoffClearance arrives,
// and the LandingModule goes around at the decision height ("not_cleared")
// if no ClearedToLand arrived, then re-flies the procedure and re-requests.
// When the runway frees, the head of the queue is promoted immediately and
// the promoted aircraft's re-requests are answered at once (at promotion
// time the runway is free by construction — see handle_re{-request}).
//
// FSM (f4-state-machine toolchain, one RunwayController per airbase):
//
//     Vacant --GrantDeparture--> Departing   (publish TakeoffClearance)
//     Vacant --GrantArrival--  -> Arriving   (publish ClearedToLand)
//     Departing --Release------> Vacant      (guard: reporter == occupant)
//     Arriving  --Release------> Vacant      (guard: reporter == occupant)
//     Arriving  --Abort--------> Vacant      (guard: aborter == occupant)
//     Departing --Timeout------> Vacant      (guard: occupied too long)
//     Arriving  --Timeout------> Vacant      (guard: occupied too long)
//
// Every transition lands in an fsm::Trace ring buffer (greppable, the
// repository's observability discipline) and is exposed to tests via the
// runway_state / queue_depth / occupant accessors.
//
// SCOPE NOTES (Tier 3):
//   - The AR (tanker) messages keep the stub's immediate-grant policy; the
//     AAR duplex protocol is driven by the TankerModule / ScriptedTanker on
//     the tanker side, and sequencing tanker contacts is future work.
//   - The FIFO policy is intentionally simple and total order deterministic.
//     Real tower priority rules (arrival-over-departure inside the gate,
//     wake turbulence spacing, SVFR) are documented extension points, not
//     Tier 3 behavior.
//
// Dependencies: f4-messaging, f4-geo, f4-state-machine. C++20.

#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

#include <f4/fsm/state_machine.hpp>
#include <f4/fsm/trace.hpp>
#include <f4/messaging/bus.hpp>

#include "f4/ai/atc/atc_interface.hpp"
#include "f4/ai/atc/messages.hpp"

namespace f4::ai::atc {

// ============================================================================
// TowerATC
// ============================================================================
class TowerATC final : public IAirTrafficControl {
public:
    // A runway resource controller: the occupancy FSM + the FIFO waiters.
    // Payload discipline matches the AI modules' pattern — the state
    // machine carries the DISCRETE protocol (states + events), the ids and
    // timers live in plain members that the message handlers set before
    // process() and the entry actions read inside process().
    struct RunwayController {
        enum class State { Vacant, Departing, Arriving };
        enum class Event { GrantDeparture, GrantArrival, Release, Abort, Timeout };

        enum class WaiterKind { Departure, Arrival };

        RunwayController(TowerATC& owner, std::uint64_t airbase_id,
                         double occupancy_timeout_s);

        // Builds the runway-occupancy transition table. Static because the
        // StateMachine member must be initialized in the constructor's
        // init list (it is not default-constructible); `self` is the
        // controller whose members the actions/guards drive.
        [[nodiscard]] static fsm::StateMachine<State, Event> make_sm(
            TowerATC& owner, RunwayController* self);

        // The airbase this runway belongs to (VU_ID.num; 0 = default field).
        // Clearances are built from the owner's resolved config for it.
        std::uint64_t airbase_id{0};

        fsm::StateMachine<State, Event> sm;

        // FIFO of aircraft waiting for the runway (both kinds, one queue).
        std::deque<std::pair<std::uint64_t, WaiterKind>> queue;

        std::uint64_t occupant{0};          // who currently holds the runway
        std::uint64_t pending_grant{0};     // id read by the Grant* entry actions
        std::uint64_t pending_reporter{0};  // id read by the Release/Abort guards
        double occupied_time{0.0};          // aged by TowerATC::tick
        double timeout_limit{300.0};        // seconds of occupancy allowed
        fsm::Trace<State, Event> trace;     // greppable transition history

        [[nodiscard]] bool vacant() const noexcept {
            return sm.current() == State::Vacant;
        }
    };

    explicit TowerATC(messaging::MessageBus& bus,
                      double occupancy_timeout_s = 300.0);

    // --- IAirTrafficControl -------------------------------------------------
    void tick(double dt) override;
    void set_airfield(const AirfieldConfig& config) override;
    void set_airbase_airfield(std::uint64_t airbase_id,
                              const AirfieldConfig& config) override;
    void set_tanker(const TankerConfig& config) override;

    // Adjust the occupancy timeout (applies to existing controllers and to
    // ones created later). Test knob; the scenario JSON sets it at build.
    void set_occupancy_timeout(double seconds);

    // --- Inspection (tests + hosts) ----------------------------------------
    [[nodiscard]] const AirfieldConfig& airfield() const noexcept {
        return airfield_;
    }
    [[nodiscard]] const TankerConfig& tanker() const noexcept {
        return tanker_;
    }
    // State of the runway serving `airbase_id` (unknown ids fall back to the
    // default controller, exactly like the clearance path).
    [[nodiscard]] RunwayController::State runway_state(std::uint64_t airbase_id) const;
    [[nodiscard]] std::size_t queue_depth(std::uint64_t airbase_id) const;
    [[nodiscard]] std::uint64_t occupant(std::uint64_t airbase_id) const;
    [[nodiscard]] std::size_t controller_count() const noexcept {
        return controllers_.size();
    }
    // Greppable transition history of the controller serving `airbase_id`
    // (nullptr when no controller exists yet — nothing sequenced there).
    [[nodiscard]] const fsm::Trace<RunwayController::State,
                                   RunwayController::Event>*
    controller_trace(std::uint64_t airbase_id) const;

private:
    // Resolve the controller for a request's airbase: per-airbase when
    // registered, else the default (key 0). Created lazily on first use.
    // (Const inspection paths resolve absent controllers as vacant/empty
    // directly — see runway_state/queue_depth/occupant.)
    RunwayController& controller_for(std::uint64_t airbase_id);
    [[nodiscard]] std::uint64_t resolve_controller_key(
        std::uint64_t airbase_id) const noexcept;

    // The airfield data a controller's clearances are built from.
    [[nodiscard]] const AirfieldConfig& resolve_airfield(
        std::uint64_t airbase_id) const noexcept;

    // --- Request handlers (bus subscriptions) -------------------------------
    void on_taxi_request(const TaxiRequest& msg);
    void on_hold_short_request(const HoldShortRequest& msg);
    void on_takeoff_request(const TakeoffRequest& msg);
    void on_landing_request(const LandingRequest& msg);
    void on_approach_request(const ApproachClearance& msg);  // "established"
    void on_departure_report(const DepartureReport& msg);
    void on_runway_vacated(const RunwayVacatedReport& msg);
    void on_go_around(const GoAroundMessage& msg);

    // --- Grant / release plumbing ------------------------------------------
    // Grant the head of the queue (caller guarantees the runway is vacant
    // and the queue is non-empty): publishes the right clearance for the
    // head's kind and drives the FSM into the matching occupied state.
    void grant_head(RunwayController& rc);
    // After any release edge: promote the next waiter if one is queued.
    void promote_if_waiting(RunwayController& rc);
    // Publish helpers (same field-filling rules as the stub's answers).
    void publish_takeoff_clearance(std::uint64_t aircraft_id,
                                   std::uint64_t airbase_id);
    void publish_cleared_to_land(std::uint64_t aircraft_id,
                                 std::uint64_t airbase_id);

    // True when `id` is already queued at `rc` (re-request dedupe).
    [[nodiscard]] static bool queued(const RunwayController& rc,
                                     std::uint64_t id);
    // Remove every queue entry for `id` (a go-around leaves the line).
    static void dequeue(RunwayController& rc, std::uint64_t id);

    messaging::MessageBus& bus_;
    AirfieldConfig airfield_{};
    TankerConfig tanker_{};
    double occupancy_timeout_s_{300.0};

    // Per-airbase airfield registry (campaign path — same shape and
    // resolution rules as the stub's). Key: airbase VU_ID.num.
    std::unordered_map<std::uint64_t, AirfieldConfig> airbase_airfields_;

    // Key: airbase VU_ID.num; 0 = the default airfield's controller.
    std::unordered_map<std::uint64_t, std::unique_ptr<RunwayController>>
        controllers_;
};

} // namespace f4::ai::atc
