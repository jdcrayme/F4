// f4-ai/src/tower_atc.cpp
//
// TowerATC implementation — the sequencing tower. See tower_atc.hpp for
// the design contract (runway as a resource, FIFO sequencing, report-driven
// releases, occupancy timeout, drop-in stub equivalence).
//
// RE-ENTRANCY DISCIPLINE: the tower publishes clearances synchronously
// inside bus handlers, exactly like the StubATC always did. This is safe
// because the AI modules LATCH clearance events and drain them outside any
// state-machine frame (STAB-E9); a synchronous grant never re-enters the
// tower's own FSM or the module's.

#include "f4/ai/atc/tower_atc.hpp"

#include <algorithm>
#include <utility>

namespace f4::ai::atc {

// ============================================================================
// RunwayController
// ============================================================================

TowerATC::RunwayController::RunwayController(TowerATC& owner,
                                             std::uint64_t airbase_id_in,
                                             double occupancy_timeout_s)
    : airbase_id(airbase_id_in),
      sm(make_sm(owner, this)),
      timeout_limit(occupancy_timeout_s)
{
    sm.set_trace(&trace);
}

f4::fsm::StateMachine<TowerATC::RunwayController::State,
                      TowerATC::RunwayController::Event>
TowerATC::RunwayController::make_sm(TowerATC& owner, RunwayController* self)
{
    // `self` is the controller under construction: the actions/guards read
    // and write its payload members (occupant, pending_grant,
    // pending_reporter, occupied_time, timeout_limit, airbase_id). It is
    // captured by pointer and never dereferenced during construction — only
    // when process() fires a transition later. The transition table IS the
    // tower's runway protocol; ids ride the members, the FSM carries the
    // discrete states (the same payload discipline the AI modules use).
    return fsm::StateMachine<State, Event>::Builder()
        .initial(State::Vacant)
        .state(State::Vacant,    "Vacant")
        .state(State::Departing, "Departing")
        .state(State::Arriving,  "Arriving")

        .event_name(Event::GrantDeparture, "GrantDeparture")
        .event_name(Event::GrantArrival,   "GrantArrival")
        .event_name(Event::Release,        "Release")
        .event_name(Event::Abort,          "Abort")
        .event_name(Event::Timeout,        "Timeout")

        // Vacant --GrantDeparture--> Departing
        // Entry publishes the clearance; the requester becomes the occupant.
        .on(State::Vacant, State::Departing, Event::GrantDeparture,
            [self, &owner](const Event&) {
                self->occupant      = self->pending_grant;
                self->occupied_time = 0.0;
                owner.publish_takeoff_clearance(self->occupant,
                                                self->airbase_id);
            },
            nullptr, "departure_cleared")

        // Departing --Release--> Vacant   (the occupant reported wheels-up)
        .on(State::Departing, State::Vacant, Event::Release,
            [self](const Event&) { self->occupant = 0; },
            [self]() { return self->pending_reporter == self->occupant; },
            "departure_reported")

        // Vacant --GrantArrival--> Arriving
        .on(State::Vacant, State::Arriving, Event::GrantArrival,
            [self, &owner](const Event&) {
                self->occupant      = self->pending_grant;
                self->occupied_time = 0.0;
                owner.publish_cleared_to_land(self->occupant,
                                              self->airbase_id);
            },
            nullptr, "arrival_cleared")

        // Arriving --Release--> Vacant   (the occupant vacated the runway)
        .on(State::Arriving, State::Vacant, Event::Release,
            [self](const Event&) { self->occupant = 0; },
            [self]() { return self->pending_reporter == self->occupant; },
            "runway_vacated")

        // Arriving --Abort--> Vacant     (granted arrival went around)
        .on(State::Arriving, State::Vacant, Event::Abort,
            [self](const Event&) { self->occupant = 0; },
            [self]() { return self->pending_reporter == self->occupant; },
            "go_around")

        // Occupied --Timeout--> Vacant   (the occupant stopped reporting —
        // killed, despawned, aborted without a report). tick() ages
        // occupied_time; the guard keeps an explicit process(Timeout) from
        // firing early. This is the resource's self-healing edge.
        .on(State::Departing, State::Vacant, Event::Timeout,
            [self](const Event&) { self->occupant = 0; },
            [self]() { return self->occupied_time >= self->timeout_limit; },
            "occupancy_timeout")
        .on(State::Arriving, State::Vacant, Event::Timeout,
            [self](const Event&) { self->occupant = 0; },
            [self]() { return self->occupied_time >= self->timeout_limit; },
            "occupancy_timeout")

        .build();
}

// ============================================================================
// Construction + IAirTrafficControl
// ============================================================================

TowerATC::TowerATC(messaging::MessageBus& bus, double occupancy_timeout_s)
    : bus_(bus), occupancy_timeout_s_(occupancy_timeout_s)
{
    // --- Ground / Taxi ---
    // Taxiing never touches the runway resource: answer like the stub.
    bus_.subscribe<TaxiRequest>([this](const TaxiRequest& msg) {
        on_taxi_request(msg);
    });

    // Hold-short is answered with the hold position (last taxi waypoint),
    // like the stub. The runway queue forms at TakeoffRequest, not here —
    // holding short is precisely the state of NOT claiming the runway.
    bus_.subscribe<HoldShortRequest>([this](const HoldShortRequest& msg) {
        on_hold_short_request(msg);
    });

    // --- Takeoff / arrival sequencing ---
    bus_.subscribe<TakeoffRequest>([this](const TakeoffRequest& msg) {
        on_takeoff_request(msg);
    });

    // LandingRequest is informational (approach data) — answer like the
    // stub every time (the module re-publishes after every go-around).
    bus_.subscribe<LandingRequest>([this](const LandingRequest& msg) {
        on_landing_request(msg);
    });

    // ApproachClearance doubles as the pilot's "established on final,
    // requesting clearance to land" (the LandingModule publishes it on the
    // OnFinal entry). THIS is where an arrival joins the runway queue.
    bus_.subscribe<ApproachClearance>([this](const ApproachClearance& msg) {
        on_approach_request(msg);
    });

    // --- Runway release edges ---
    bus_.subscribe<DepartureReport>([this](const DepartureReport& msg) {
        on_departure_report(msg);
    });
    bus_.subscribe<RunwayVacatedReport>([this](const RunwayVacatedReport& msg) {
        on_runway_vacated(msg);
    });
    bus_.subscribe<GoAroundMessage>([this](const GoAroundMessage& msg) {
        on_go_around(msg);
    });

    // --- Air refueling ---
    // Tier 3 scope is the runway. The AR duplex protocol keeps the stub's
    // immediate-grant policy (the tanker-side SM drives the real protocol;
    // sequencing tanker contacts is future work). Same handlers, same
    // answers — tower mode is a drop-in for AAR scenarios.
    bus_.subscribe<RefuelRequest>([this](const RefuelRequest& msg) {
        TankerAssigned assigned;
        assigned.receiver_id = msg.aircraft_id;
        assigned.tanker_id = tanker_.tanker_entity_id;
        assigned.tanker_position = tanker_.position;
        assigned.tanker_heading_rad = tanker_.heading_rad;
        assigned.ar_altitude_ft = tanker_.altitude_ft;
        bus_.publish(assigned);
    });

    bus_.subscribe<ContactRequest>([this](const ContactRequest& msg) {
        ContactMade contact;
        contact.receiver_id = msg.receiver_id;
        contact.tanker_id = msg.tanker_id;
        bus_.publish(contact);
    });

    bus_.subscribe<PrecontactReport>([this](const PrecontactReport& msg) {
        ClearToContact clear;
        clear.receiver_id = msg.receiver_id;
        clear.tanker_id = msg.tanker_id;
        bus_.publish(clear);
    });

    bus_.subscribe<DisconnectRequest>([this](const DisconnectRequest& msg) {
        DisconnectApproved approved;
        approved.receiver_id = msg.receiver_id;
        approved.tanker_id = msg.tanker_id;
        bus_.publish(approved);
        FuelTransferred fuel;
        fuel.receiver_id = msg.receiver_id;
        fuel.tanker_id = msg.tanker_id;
        fuel.fuel_lbs = 5000.0;   // stub policy: 5000 lbs offloaded
        bus_.publish(fuel);
    });
}

void TowerATC::tick(double dt) {
    if (dt <= 0.0) return;
    for (auto& [/*airbase*/ id, rc_ptr] : controllers_) {
        (void)id;
        auto& rc = *rc_ptr;
        if (rc.vacant()) continue;
        rc.occupied_time += dt;
        if (rc.occupied_time >= rc.timeout_limit) {
            rc.sm.process(RunwayController::Event::Timeout);
            promote_if_waiting(rc);
        }
    }
}

void TowerATC::set_airfield(const AirfieldConfig& config) {
    airfield_ = config;
}

void TowerATC::set_airbase_airfield(std::uint64_t airbase_id,
                                    const AirfieldConfig& config) {
    airbase_airfields_[airbase_id] = config;
}

void TowerATC::set_tanker(const TankerConfig& config) {
    tanker_ = config;
}

void TowerATC::set_occupancy_timeout(double seconds) {
    occupancy_timeout_s_ = seconds;
    for (auto& [/*airbase*/ id, rc_ptr] : controllers_) {
        (void)id;
        rc_ptr->timeout_limit = seconds;
    }
}

// ============================================================================
// Request handlers
// ============================================================================

void TowerATC::on_taxi_request(const TaxiRequest& msg) {
    const AirfieldConfig& af = resolve_airfield(msg.airbase_id);
    TaxiClearance clearance;
    clearance.aircraft_id = msg.aircraft_id;
    clearance.airbase_id = msg.airbase_id;
    clearance.taxi_route = af.taxi_route;
    clearance.runway_id = af.active_runway_id;
    clearance.runway_name = af.active_runway_name;
    bus_.publish(clearance);
}

void TowerATC::on_hold_short_request(const HoldShortRequest& msg) {
    const AirfieldConfig& af = resolve_airfield(msg.airbase_id);
    HoldShortClearance clearance;
    clearance.aircraft_id = msg.aircraft_id;
    clearance.runway_id = msg.runway_id;
    if (!af.taxi_route.empty()) {
        clearance.hold_position = af.taxi_route.back();
    }
    bus_.publish(clearance);
}

void TowerATC::on_takeoff_request(const TakeoffRequest& msg) {
    RunwayController& rc = controller_for(msg.airbase_id);

    // Already the occupant: the module re-publishes TakeoffRequest when its
    // Wait -> HoldShort bounce re-enters HoldShort after a promotion was
    // latched. Answer idempotently (the runway is its own); never queue an
    // occupant behind itself.
    if (rc.occupant == msg.aircraft_id) {
        publish_takeoff_clearance(msg.aircraft_id, msg.airbase_id);
        return;
    }

    if (rc.vacant()) {
        rc.pending_grant = msg.aircraft_id;
        rc.sm.process(RunwayController::Event::GrantDeparture);
        return;
    }

    // Occupied: queue (FIFO), deduped. Silence = hold — the module waits in
    // HoldShort until its TakeoffClearance arrives (its documented
    // deferral contract). The queue entry is consumed by grant_head().
    if (!queued(rc, msg.aircraft_id)) {
        rc.queue.emplace_back(
            msg.aircraft_id, RunwayController::WaiterKind::Departure);
    }
}

void TowerATC::on_landing_request(const LandingRequest& msg) {
    const AirfieldConfig& af = resolve_airfield(msg.airbase_id);
    LandingClearance clearance;
    clearance.aircraft_id = msg.aircraft_id;
    clearance.runway_id = af.active_runway_id;
    clearance.runway_name = af.active_runway_name;
    clearance.runway_heading_rad = af.runway_heading_rad;
    clearance.threshold_position = af.threshold_position;
    clearance.threshold_altitude_ft = af.threshold_altitude_ft;
    clearance.glide_slope_angle_rad = af.glide_slope_angle_rad;
    clearance.pattern_altitude_ft = af.pattern_altitude_ft;
    clearance.decision_height_ft = af.decision_height_ft;
    clearance.runway_width_ft = af.runway_width_ft;
    clearance.runway_length_ft = af.runway_length_ft;
    bus_.publish(clearance);
}

void TowerATC::on_approach_request(const ApproachClearance& msg) {
    RunwayController& rc = controller_for(msg.airbase_id);

    // Re-established while already holding the claim (defensive; a granted
    // arrival that re-requests without going around): re-answer.
    if (rc.occupant == msg.aircraft_id) {
        publish_cleared_to_land(msg.aircraft_id, msg.airbase_id);
        return;
    }

    if (rc.vacant()) {
        rc.pending_grant = msg.aircraft_id;
        rc.sm.process(RunwayController::Event::GrantArrival);
        return;
    }

    // Occupied: queue. Silence = the approach continues uncleared; the
    // LandingModule fires its own "not_cleared" go-around at the decision
    // height, which dequeues it (on_go_around) — it re-requests on the
    // next pass. No clearance ever arrives while the runway is held.
    if (!queued(rc, msg.aircraft_id)) {
        rc.queue.emplace_back(
            msg.aircraft_id, RunwayController::WaiterKind::Arrival);
    }
}

void TowerATC::on_departure_report(const DepartureReport& msg) {
    RunwayController& rc = controller_for(msg.airbase_id);
    rc.pending_reporter = msg.aircraft_id;
    rc.sm.process(RunwayController::Event::Release);
    promote_if_waiting(rc);
}

void TowerATC::on_runway_vacated(const RunwayVacatedReport& msg) {
    RunwayController& rc = controller_for(msg.airbase_id);
    rc.pending_reporter = msg.aircraft_id;
    rc.sm.process(RunwayController::Event::Release);
    promote_if_waiting(rc);
}

void TowerATC::on_go_around(const GoAroundMessage& msg) {
    RunwayController& rc = controller_for(msg.airbase_id);
    // Two cases, one handler:
    //   - the aborter HOLDS the claim (went around after being cleared —
    //     "threshold_overflown"): Abort releases it;
    //   - the aborter was WAITING ("not_cleared" at the decision height):
    //     the Abort guard fails (occupant unchanged) and the dequeue below
    //     removes it from the line — it re-requests on the next pass.
    rc.pending_reporter = msg.aircraft_id;
    rc.sm.process(RunwayController::Event::Abort);
    dequeue(rc, msg.aircraft_id);
    promote_if_waiting(rc);
}

// ============================================================================
// Grant / release plumbing
// ============================================================================

void TowerATC::grant_head(RunwayController& rc) {
    if (rc.queue.empty()) return;
    const auto [id, kind] = rc.queue.front();
    rc.queue.pop_front();
    rc.pending_grant = id;
    rc.sm.process(kind == RunwayController::WaiterKind::Departure
                      ? RunwayController::Event::GrantDeparture
                      : RunwayController::Event::GrantArrival);
}

void TowerATC::promote_if_waiting(RunwayController& rc) {
    while (rc.vacant() && !rc.queue.empty()) {
        grant_head(rc);
    }
}

void TowerATC::publish_takeoff_clearance(std::uint64_t aircraft_id,
                                         std::uint64_t airbase_id) {
    const AirfieldConfig& af = resolve_airfield(airbase_id);
    TakeoffClearance clearance;
    clearance.aircraft_id = aircraft_id;
    clearance.runway_id = af.active_runway_id;
    clearance.runway_heading_rad = af.runway_heading_rad;
    clearance.threshold_position = af.threshold_position;
    clearance.departure_altitude_ft = af.departure_altitude_ft;
    bus_.publish(clearance);
}

void TowerATC::publish_cleared_to_land(std::uint64_t aircraft_id,
                                       std::uint64_t airbase_id) {
    const AirfieldConfig& af = resolve_airfield(airbase_id);
    ClearedToLand cleared;
    cleared.aircraft_id = aircraft_id;
    cleared.runway_id = af.active_runway_id;
    bus_.publish(cleared);
}

bool TowerATC::queued(const RunwayController& rc, std::uint64_t id) {
    return std::any_of(rc.queue.begin(), rc.queue.end(),
                       [id](const auto& entry) { return entry.first == id; });
}

void TowerATC::dequeue(RunwayController& rc, std::uint64_t id) {
    rc.queue.erase(
        std::remove_if(rc.queue.begin(), rc.queue.end(),
                       [id](const auto& entry) { return entry.first == id; }),
        rc.queue.end());
}

// ============================================================================
// Resolution + inspection
// ============================================================================

const AirfieldConfig& TowerATC::resolve_airfield(
    std::uint64_t airbase_id) const noexcept {
    const auto it = airbase_airfields_.find(airbase_id);
    return (it != airbase_airfields_.end()) ? it->second : airfield_;
}

TowerATC::RunwayController& TowerATC::controller_for(std::uint64_t airbase_id) {
    const auto key = resolve_controller_key(airbase_id);
    auto it = controllers_.find(key);
    if (it == controllers_.end()) {
        it = controllers_
                 .emplace(key,
                          std::make_unique<RunwayController>(
                              *this, key, occupancy_timeout_s_))
                 .first;
    }
    return *it->second;
}

std::uint64_t TowerATC::resolve_controller_key(
    std::uint64_t airbase_id) const noexcept {
    // Per-airbase registration wins; everything else (unknown id, 0) is
    // sequenced on the default field's controller — mirroring the stub's
    // config resolution so a mis-tagged request still gets served.
    const bool registered =
        airbase_airfields_.find(airbase_id) != airbase_airfields_.end();
    return registered ? airbase_id : std::uint64_t{0};
}

TowerATC::RunwayController::State TowerATC::runway_state(
    std::uint64_t airbase_id) const {
    const auto it = controllers_.find(resolve_controller_key(airbase_id));
    return (it != controllers_.end())
               ? it->second->sm.current()
               : RunwayController::State::Vacant;
}

std::size_t TowerATC::queue_depth(std::uint64_t airbase_id) const {
    const auto it = controllers_.find(resolve_controller_key(airbase_id));
    return (it != controllers_.end()) ? it->second->queue.size() : 0;
}

std::uint64_t TowerATC::occupant(std::uint64_t airbase_id) const {
    const auto it = controllers_.find(resolve_controller_key(airbase_id));
    return (it != controllers_.end()) ? it->second->occupant : 0;
}

const f4::fsm::Trace<TowerATC::RunwayController::State,
                     TowerATC::RunwayController::Event>*
TowerATC::controller_trace(std::uint64_t airbase_id) const {
    const auto it = controllers_.find(resolve_controller_key(airbase_id));
    return (it != controllers_.end()) ? &it->second->trace : nullptr;
}

} // namespace f4::ai::atc
