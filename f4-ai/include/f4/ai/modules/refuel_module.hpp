// f4-ai/include/f4/ai/modules/refuel_module.hpp
//
// RefuelModule — the receiver-side air-to-air refueling state machine.
// AAR_REDESIGN_PLAN.md: the full USAF boom AAR procedure.
//
// The receiver flies:
//   rendezvous -> pre-contact -> (tanker clears contact) -> contact ->
//   hold -> (receiver requests disconnect) -> back to pre-contact ->
//   (tanker reports fuel + clears departure) -> descend 1000 ft below
//   tanker -> resume flight-planned route.
//
// The protocol is full-duplex over the MessageBus:
//   Receiver -> Tanker: RefuelRequest, PrecontactReport, ContactRequest,
//                        DisconnectRequest
//   Tanker -> Receiver: TankerAssigned, ClearToContact, ContactMade,
//                        ContactLost, DisconnectApproved, FuelTransferred
//
// The tanker is a REAL AIRCRAFT (own flight model + own brain). The
// host pushes the tanker's kinematic picture each tick (mirroring
// WingmanModule::LeadPicture — the module is engine-agnostic, it cannot
// read the tanker entity itself).
//
// FreeFalcon source: digi_refuel.cpp (the refuel state machine + boom
// envelope), dlogic.cpp (RefuelMode).
//
// Dependencies: f4-state-machine, f4-messaging, f4-entities, f4-geo,
// f4-flight-api (IAircraftState). C++20.

#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <f4/entities/entity.hpp>
#include <f4/messaging/bus.hpp>
#include <f4/fsm/state_machine.hpp>
#include <f4/fsm/trace.hpp>
#include <f4/geo/position.hpp>
#include <f4/flight/api/i_aircraft_state.hpp>

#include "f4/ai/ai_output.hpp"
#include "f4/ai/air_steering.hpp"
#include "f4/ai/atc/messages.hpp"

namespace f4::ai::modules {

// ============================================================================
// Refuel states — the full USAF boom AAR procedure (8 states + Done)
// ============================================================================
enum class RefuelState {
    NoTanker,        // requesting a tanker assignment (entry: RefuelRequest)
    Rendezvous,     // closing on the tanker's track toward pre-contact
    PreContact,     // stabilized at the pre-contact observation point
    ClearedContact,  // tanker cleared contact; closing into the contact envelope
    Hold,           // boom latched (ContactMade); holding formation
    BackingOut,     // receiver requested disconnect; backing out to pre-contact
    Departing,      // tanker approved departure; descending 1000 ft below tanker
    Done            // descended; the brain hands back to the nav module
};

enum class RefuelEvent {
    TankerAssigned,        // TankerAssigned received (NoTanker -> Rendezvous)
    AtPrecontactPos,      // reached the pre-contact envelope (Rendezvous -> PreContact, BackingOut -> PreContact)
    ClearToContact,       // ClearToContact received (PreContact -> ClearedContact)
    InContactEnvelope,    // reached the contact envelope (ClearedContact -> publish ContactRequest)
    ContactMade,          // ContactMade received (-> Hold)
    ContactLost,          // ContactLost received (Hold -> PreContact)
    ReceiverRequestsDisconnect,  // host/fuel-target triggered (Hold -> BackingOut)
    DisconnectApproved,    // DisconnectApproved received (PreContact/BackingOut -> Departing)
    ReachedDeparture,      // descended 1000 ft below tanker (Departing -> Done)
    TankerLost            // tanker picture invalid (any active -> NoTanker)
};

// ============================================================================
// TankerPicture — the host pushes this each tick (engine-agnostic).
// Mirrors WingmanModule::LeadPicture. The tanker is a real aircraft;
// the host reads its TransformComponent + FlightModelComponent.
// ============================================================================
struct TankerPicture {
    bool valid{false};
    geo::WorldPosition position{};   // ENU feet
    double heading_rad{0.0};         // compass, CW from north
    double speed_kts{0.0};           // the tanker's VCAS
    double altitude_msl_ft{0.0};      // the tanker's MSL altitude
};

// ============================================================================
// RefuelModule
// ============================================================================
class RefuelModule {
public:
    struct Config {
        // --- Boom offset ---
        // USAF/NATO ATP-56: the pre-contact and contact positions are
        // relative to the BOOM NOZZLE, not the aircraft root. The boom
        // extends from the tanker's tail — the nozzle is ~half the
        // tanker's length aft of the root. For a KC-10 (181 ft long),
        // the boom nozzle is ~90 ft aft. This offset is ADDED to the
        // pre-contact and contact offsets to get the position relative
        // to the tanker's transform root (which the FM uses).
        double boom_offset_long_ft{90.0};   // boom nozzle aft of aircraft root

        // --- Pre-contact envelope (Rendezvous -> PreContact) ---
        // USAF/NATO ATP-56: "The position approximately 50 ft behind and
        // slightly below the tanker boom nozzle where the receiver
        // stabilizes with zero rate of closure before being cleared to
        // contact." The 50 ft is from the BOOM NOZZLE, so the total
        // offset from the aircraft root is boom_offset + 50 ft.
        // EMPL-2 — the tolerances widened from the design ±15 ft to
        // ±150/±300 ft: the campaign join arrives at nav-scale rates
        // (the rendezvous envelope swaps back at the hand-off), and the
        // FM's phugoid sweeps ±hundreds of ft per axis — the QC funnel
        // shows receivers passing 42 ft along and 164 ft vert at
        // DIFFERENT ticks, never all three inside a ±15 window at once.
        // The catch window only has to admit the receiver for ONE tick;
        // the PreContact hold's tight cascade (vs_gain 3, integrators)
        // then recovers station-keeping — the same division of labor
        // the CONTACT envelope's earlier widening (±15 → ±40/±60)
        // established. The scenario runs keep their shape: their
        // receiver converges inside the design envelope anyway.
        double precontact_offset_long_ft{50.0};   // behind the boom nozzle (ATP-56)
        double precontact_offset_vert_ft{-10.0};   // slightly below (ATP-56)
        double precontact_long_ft{150.0};          // ± ft along (widened, see above)
        double precontact_lat_ft{150.0};           // ± ft lateral (widened)
        double precontact_vert_ft{300.0};          // ± ft vertical (widened)

        // --- Contact envelope (ClearedContact -> Contact -> Hold) ---
        // USAF/NATO ATP-56: the boom latches at the contact position.
        // The contact offset is ~10 ft aft of the boom nozzle (the boom
        // length from nozzle to receptacle). The boom disconnect
        // envelope is ±6 ft (ATP-56); widened to ±15 for the FM's
        // residual phugoid. (A QC-WORLD experiment widening these to
        // ±40 + a 300-fpm request gate made tanker_track latch-churn —
        // 16 contacts / 15 losses, zero fuel, exit 23: the wider window
        // admits the receiver mid-drift so it never stabilizes in Hold.
        // Reverted; the anchored-AAR latch sensitivity is a documented
        // follow-up.)
        double contact_offset_long_ft{10.0};        // aft of the boom nozzle
        double contact_long_ft{15.0};               // ± ft along (widened from 6)
        double contact_lat_ft{15.0};                 // ± ft lateral (widened from 6)
        // ATP-56 physical boom envelope is ±6 ft; widened to ±40 for the
        // F-16 FM's phugoid residual at 10000 ft (the VS damper reduces
        // the oscillation to ~25 ft, but the initial VS from the closure
        // creates a transient that takes ~10s to damp). The ±40 ft envelope
        // lets the receiver hold through the transient. The 95th-pct
        // diagnostic target remains ±15 ft (the steady-state tolerance).
        double contact_vert_ft{60.0};

        // --- Departure ---
        // USAF: the receiver descends to 1000 ft below the tanker for
        // vertical separation, then resumes own navigation.
        double departure_offset_vert_ft{-1000.0};   // 1000 ft below the tanker

        // --- Speed schedule ---
        double closure_bias_kts{3.0};               // Rendezvous closure bias (ATP-56: ~1 ft/s)

        // --- EMPL-2: the rendezvous closure law (error-proportional) ---
        // The original law chased the pre-contact point at a CONSTANT
        // closure_bias_kts (3): fine for the scenario template (the
        // receiver is PLACED at the pre-contact position) — glacial for
        // the campaign rendezvous, where the receiver's nav hands off
        // miles out (a 5,000-ft deficit closed at 3 kts takes ~28
        // minutes). ATP-56's join is flown at INTERCEPT speed while the
        // deficit is large and matched + bias near the boom. The law:
        // close the along-track error toward the pre-contact point with
        // a time constant, clamped to an intercept ceiling (closing) /
        // a fall-back ceiling (ahead). The error decays exponentially
        // (tau) and lands at the original 3-kts creep near the envelope
        // — the scenario behavior is the tau-limit of this law, so the
        // scenario runs keep their shape.
        double rendezvous_tau_s{30.0};
        double rendezvous_max_closure_kts{150.0};   // intercept-speed ceiling
        double rendezvous_max_back_kts{100.0};      // overshoot fall-back

        // --- EMPL-2: the rendezvous steering envelope (the JOIN is a
        // nav-scale maneuver) --- The constructor tunes air_steering for
        // the HOLD phases (bank 0.10, VS 300 fpm — the station-keeping
        // precision). A campaign receiver hands off to the rung with a
        // 10-15k-ft altitude deficit and a 50k-ft horizontal deficit:
        // at 300 fpm the vertical alone takes 40 minutes. The rendezvous
        // state therefore swaps in the nav-scale envelope (the
        // AirSteering defaults: ~30 deg bank, 2500 fpm) while it joins,
        // and the hold phases swap the tight envelope back before they
        // steer. The scenario runs keep their shape: the receiver is
        // PLACED at the pre-contact position — the rendezvous converges
        // within the tight envelope's reach before any swap matters.
        double rendezvous_max_bank_rad{0.70};
        double rendezvous_max_vs_fpm{2500.0};

        // --- EMPL-2: lead pursuit --- Pure pursuit of a point on an
        // orbit converges to a same-radius circle ASTERN (the e2e
        // catch: the receiver circled at 9-18k ft, zero closure — it
        // cannot out-turn the boom without cutting inside). Aiming the
        // heading this many seconds AHEAD of the pre-contact point
        // along the tanker's track puts the cut inside the turn and
        // the radius closes every lap. The closure SPEED law still
        // aims at the point itself; only the heading leads.
        double rendezvous_lead_s{15.0};

        // --- EMPL-2: the near-field terminal --- Inside this distance
        // the join becomes FORMATE-AND-CLOSE: heading = the tanker's
        // track (stop the orbit — a sustained turn at the join bank
        // bleeds the vertical authority and the receiver ends up
        // orbiting 5k ft ABOVE the boom, the e2e catch), speed = the
        // braking curve, altitude = the boom's. The pre-contact point
        // comes to the receiver.
        double rendezvous_near_ft{10000.0};

        // --- EMPL-2: the standoff join --- The horizontal closure runs
        // at kts-scale (up to 150) while the vertical crawls at
        // fpm-scale (2,500 max): a receiver joining 10k ft BELOW the
        // station flies through the pre-contact point long before it is
        // LEVEL with it — and the three envelope axes phase-lock out of
        // alignment (the QC funnel: along 42 ft and vert 164 ft at
        // DIFFERENT ticks, never together). ATP-56's join levels off at
        // the boom's altitude BEFORE the final closure — so the
        // rendezvous aims its along-axis at a point this many feet
        // BEHIND the pre-contact point until |dz| is inside the
        // pre-contact vertical window, then releases the standoff and
        // closes. From 6,000 ft at the intercept ceiling the final
        // closure takes ~40 s with the altitude already reconciled.
        double rendezvous_standoff_ft{6000.0};

        // --- EMPL-2: the braking curve --- The closure demand is the
        // SMALLER of the tau law and the kinematic stopping distance
        // sqrt(2·a·err): a clamped constant-overtake (150 kts) cannot
        // stop AT the aim point — the QC funnel shows the receiver
        // closing 74,000 ft and overshooting +30,000 ft past it. The
        // curve starts braking at the distance the deceleration
        // (rendezvous_brake_fps2, ~0.19 g) can absorb, and the final
        // approach creeps into the pre-contact envelope at ~25 kts
        // overtake instead of flying through it.
        double rendezvous_brake_fps2{6.0};

        // --- Fuel target (lbs; 0 = host-driven disconnect) ---
        double fuel_complete_lbs{0.0};

        // --- How long to hold in Hold before auto-requesting disconnect ---
        // (the host can also trigger it; 0 = never auto-trigger)
        double auto_disconnect_hold_s{20.0};

        // --- Steering gains (tightened from AirSteering's nav defaults) ---
        double max_bank_rad{0.10};
        double max_vs_fpm{300.0};
    };

    // --- Construction ---
    RefuelModule();

    // --- Initialization ---
    void initialize(
        std::uint64_t ownship_id,
        entities::EntityWorld& world,
        messaging::MessageBus& bus);

    // --- Per-tick update ---
    AIControlOutput update(double dt, const flight::IAircraftState* state);

    // --- Tanker picture push (host calls each tick before update) ---
    void set_tanker_picture(const TankerPicture& p) noexcept {
        tanker_picture_ = p;
    }
    [[nodiscard]] const TankerPicture& tanker_picture() const noexcept {
        return tanker_picture_;
    }

    // --- Accessors ---
    [[nodiscard]] RefuelState state() const noexcept { return sm_.current(); }
    [[nodiscard]] bool is_complete() const noexcept {
        return sm_.current() == RefuelState::Done;
    }
    [[nodiscard]] bool is_active() const noexcept {
        const auto s = sm_.current();
        return s != RefuelState::NoTanker && s != RefuelState::Done;
    }

    // --- Configuration ---
    Config config;
    mutable AirSteering air_steering;   // mutable: const control methods swap gains

    // --- Trace ---
    void set_trace(fsm::Trace<RefuelState, RefuelEvent>* t) noexcept {
        sm_.set_trace(t);
    }
    [[nodiscard]] const fsm::Trace<RefuelState, RefuelEvent>* trace() const noexcept {
        return sm_.trace();
    }

    // --- Human-readable names ---
    [[nodiscard]] std::string state_name() const;
    [[nodiscard]] std::string mode_name() const { return "RefuelMode"; }

    // --- Geometry (public for tests + the FCS trace exporter) ---
    /// The pre-contact observation point (behind + below the tanker).
    [[nodiscard]] geo::WorldPosition precontact_point() const;
    /// The boom contact receptacle (aft of the tanker, level).
    [[nodiscard]] geo::WorldPosition contact_point() const;
    /// The departure target (1000 ft below the tanker).
    [[nodiscard]] geo::WorldPosition departure_point() const;
    /// Along-track error in the tanker's heading frame (+ = ahead).
    [[nodiscard]] double along_err_ft() const;
    /// Lateral error across the tanker's heading axis (+ = right).
    [[nodiscard]] double lat_err_ft() const;
    /// Vertical error vs the tanker's altitude (+ = above).
    [[nodiscard]] double vert_err_ft() const;
    /// True when the receiver is inside the pre-contact envelope.
    [[nodiscard]] bool in_precontact_envelope() const;
    /// True when the receiver is inside the contact envelope.
    [[nodiscard]] bool in_contact_envelope() const;
    /// True when the receiver has descended to the departure altitude.
    [[nodiscard]] bool reached_departure() const;
    /// Time accumulated in Hold (seconds).
    [[nodiscard]] double contact_time_s() const noexcept {
        return hold_time_s_;
    }
    /// Fuel transferred (reported by the tanker via FuelTransferred).
    [[nodiscard]] double fuel_received_lbs() const noexcept {
        return fuel_received_lbs_;
    }

private:
    fsm::StateMachine<RefuelState, RefuelEvent> build_sm();

    // Per-state control logic (pure functions of cached state).
    AIControlOutput controls_for_no_tanker() const;
    AIControlOutput controls_for_rendezvous() const;
    AIControlOutput controls_for_precontact() const;
    AIControlOutput controls_for_cleared_contact() const;
    AIControlOutput controls_for_hold() const;
    AIControlOutput controls_for_backing_out() const;
    AIControlOutput controls_for_departing() const;
    AIControlOutput controls_for_done() const;

    // Transition checks (called from update before control dispatch).
    void check_at_precontact();        // Rendezvous/BackingOut -> PreContact
    void check_in_contact_envelope();  // ClearedContact -> publish ContactRequest
    void check_contact_lost();         // Hold -> PreContact
    void check_auto_disconnect();      // Hold -> BackingOut (host/fuel/timeout)
    void check_reached_departure();    // Departing -> Done
    void check_tanker_lost();          // any active -> NoTanker

    // Cache the current aircraft state fields.
    void cache_aircraft_state(const flight::IAircraftState* state);
    [[nodiscard]] AirSteering::Input steering_input() const noexcept;

    // --- Data members (sm_ MUST be last) ---
    std::uint64_t ownship_id_{0};
    std::optional<RefuelEvent> deferred_event_{};
    entities::EntityWorld* world_{nullptr};
    messaging::MessageBus* bus_{nullptr};
    /// FID-OPT-1: the refuel-protocol subscriptions, unbundled when this
    /// module dies (the RAII bundle — see ScopedSubscriptions in
    /// f4-messaging bus.hpp and TakeoffModule's note).
    messaging::ScopedSubscriptions subscriptions_{};

    std::uint64_t tanker_id_{0};
    TankerPicture tanker_picture_{};
    bool published_contact_request_{false};
    bool published_precontact_report_{false};
    double fuel_received_lbs_{0.0};

    // Cached ownship state.
    geo::WorldPosition current_position_;
    double current_vcas_kts_{0.0};
    double current_alt_msl_ft_{0.0};
    double current_heading_rad_{0.0};
    double current_pitch_rad_{0.0};
    double current_roll_rad_{0.0};
    double current_roll_rate_radps_{0.0};
    double current_pitch_rate_radps_{0.0};
    double current_vs_fpm_{0.0};
    double fuel_lbs_{0.0};

    double hold_time_s_{0.0};
    double state_time_s_{0.0};   // time in the current state
    double precontact_stable_time_s_{0.0};  // time spent stabilized at precontact (|VS| < 200)

    fsm::StateMachine<RefuelState, RefuelEvent> sm_;
};

} // namespace f4::ai::modules
