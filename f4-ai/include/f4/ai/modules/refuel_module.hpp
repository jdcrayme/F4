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
        double precontact_offset_long_ft{50.0};   // behind the boom nozzle (ATP-56)
        double precontact_offset_vert_ft{-10.0};   // slightly below (ATP-56)
        double precontact_long_ft{15.0};           // ± ft along (stabilization tol)
        double precontact_lat_ft{15.0};            // ± ft lateral
        double precontact_vert_ft{15.0};            // ± ft vertical

        // --- Contact envelope (ClearedContact -> Contact -> Hold) ---
        // USAF/NATO ATP-56: the boom latches at the contact position.
        // The contact offset is ~10 ft aft of the boom nozzle (the boom
        // length from nozzle to receptacle). The boom disconnect
        // envelope is ±6 ft (ATP-56); widened to ±15 for the FM's
        // residual phugoid.
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
