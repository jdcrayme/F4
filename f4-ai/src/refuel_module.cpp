// f4-ai/src/refuel_module.cpp
//
// RefuelModule implementation — the full USAF boom AAR procedure.
// See f4-ai/include/f4/ai/modules/refuel_module.hpp + Docs/AAR_REDESIGN_PLAN.md.

#include "f4/ai/modules/refuel_module.hpp"

#include <algorithm>
#include <cmath>

namespace f4::ai::modules {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 6.28318530717958647692;
constexpr double kTasToVcasAt20k = 0.73;   // sqrt(rho/rho0) at 20000 ft
constexpr double kCruiseThrottle = 0.22;  // F-16 level flight at 20000 ft / 260 kts TAS

inline double wrap_heading_err(double e) {
    while (e >  kPi)  e -= kTwoPi;
    while (e < -kPi)  e += kTwoPi;
    return e;
}
} // namespace

// ============================================================================
// Construction + SM build
// ============================================================================
RefuelModule::RefuelModule()
    : sm_(build_sm())
{
    // AAR-specific air_steering tuning (see AAR_REDESIGN_PLAN.md §2.5).
    air_steering.max_bank_rad      = 0.10;
    air_steering.max_vs_fpm        = 300.0;
    air_steering.vs_gain           = 0.5;
    air_steering.alt_integral_gain  = 0.1;
    air_steering.alt_integral_max   = 50.0;
    air_steering.attitude_gain      = 1.0;
    air_steering.pitch_rate_damp   = 1.0;
    air_steering.bank_gain         = 3.0;
    air_steering.roll_gain         = 4.0;
    air_steering.roll_damp         = 4.0;
    air_steering.throttle_min      = 0.05;
}

fsm::StateMachine<RefuelState, RefuelEvent>
RefuelModule::build_sm()
{
    return typename fsm::StateMachine<RefuelState, RefuelEvent>::Builder()
        .initial(RefuelState::NoTanker)
        .state(RefuelState::NoTanker,       "NoTanker")
        .state(RefuelState::Rendezvous,      "Rendezvous")
        .state(RefuelState::PreContact,     "PreContact")
        .state(RefuelState::ClearedContact, "ClearedContact")
        .state(RefuelState::Hold,           "Hold")
        .state(RefuelState::BackingOut,     "BackingOut")
        .state(RefuelState::Departing,      "Departing")
        .state(RefuelState::Done,           "Done")

        .event_name(RefuelEvent::TankerAssigned,       "TankerAssigned")
        .event_name(RefuelEvent::AtPrecontactPos,      "AtPrecontactPos")
        .event_name(RefuelEvent::ClearToContact,       "ClearToContact")
        .event_name(RefuelEvent::InContactEnvelope,    "InContactEnvelope")
        .event_name(RefuelEvent::ContactMade,          "ContactMade")
        .event_name(RefuelEvent::ContactLost,          "ContactLost")
        .event_name(RefuelEvent::ReceiverRequestsDisconnect, "ReceiverRequestsDisconnect")
        .event_name(RefuelEvent::DisconnectApproved,    "DisconnectApproved")
        .event_name(RefuelEvent::ReachedDeparture,      "ReachedDeparture")
        .event_name(RefuelEvent::TankerLost,            "TankerLost")

        // --- Transitions ---
        .on(RefuelState::NoTanker, RefuelState::Rendezvous,
            RefuelEvent::TankerAssigned, nullptr, nullptr, "tanker_assigned")
        .on(RefuelState::Rendezvous, RefuelState::PreContact,
            RefuelEvent::AtPrecontactPos, nullptr, nullptr, "reached_precontact")
        .on(RefuelState::PreContact, RefuelState::ClearedContact,
            RefuelEvent::ClearToContact, nullptr, nullptr, "tanker_cleared_contact")
        .on(RefuelState::ClearedContact, RefuelState::Hold,
            RefuelEvent::ContactMade, nullptr, nullptr, "boom_latched")
        .on(RefuelState::Hold, RefuelState::BackingOut,
            RefuelEvent::ReceiverRequestsDisconnect, nullptr, nullptr, "receiver_requests_disconnect")
        .on(RefuelState::Hold, RefuelState::PreContact,
            RefuelEvent::ContactLost, nullptr, nullptr, "boom_disconnected")
        .on(RefuelState::BackingOut, RefuelState::PreContact,
            RefuelEvent::AtPrecontactPos, nullptr, nullptr, "backed_out_to_precontact")
        .on(RefuelState::PreContact, RefuelState::Departing,
            RefuelEvent::DisconnectApproved, nullptr, nullptr, "tanker_cleared_departure")
        .on(RefuelState::BackingOut, RefuelState::Departing,
            RefuelEvent::DisconnectApproved, nullptr, nullptr, "tanker_cleared_departure")
        .on(RefuelState::Departing, RefuelState::Done,
            RefuelEvent::ReachedDeparture, nullptr, nullptr, "descended_to_departure_alt")
        .on(RefuelState::Rendezvous, RefuelState::NoTanker,
            RefuelEvent::TankerLost, nullptr, nullptr, "tanker_picture_invalid")
        .on(RefuelState::PreContact, RefuelState::NoTanker,
            RefuelEvent::TankerLost, nullptr, nullptr, "tanker_picture_invalid")
        .on(RefuelState::ClearedContact, RefuelState::NoTanker,
            RefuelEvent::TankerLost, nullptr, nullptr, "tanker_picture_invalid")
        .on(RefuelState::Hold, RefuelState::NoTanker,
            RefuelEvent::TankerLost, nullptr, nullptr, "tanker_picture_invalid")
        .on(RefuelState::BackingOut, RefuelState::NoTanker,
            RefuelEvent::TankerLost, nullptr, nullptr, "tanker_picture_invalid")
        .on(RefuelState::Departing, RefuelState::NoTanker,
            RefuelEvent::TankerLost, nullptr, nullptr, "tanker_picture_invalid")

        // --- Entry actions ---
        .on_enter(RefuelState::NoTanker, [this](const RefuelEvent&) {
            if (bus_) {
                atc::RefuelRequest req;
                req.aircraft_id = ownship_id_;
                bus_->publish(req);
            }
        })
        .on_enter(RefuelState::Rendezvous, [this](const RefuelEvent&) {
            published_precontact_report_ = false;
            published_contact_request_ = false;
            hold_time_s_ = 0.0;
        })
        .on_enter(RefuelState::PreContact, [this](const RefuelEvent&) {
            published_precontact_report_ = false;
            published_contact_request_ = false;
            hold_time_s_ = 0.0;
            precontact_stable_time_s_ = 0.0;
            // Reset air_steering integrators to prevent windup from the
            // spawn transient (the receiver enters PreContact with a
            // large VS; the altitude/speed integrals accumulate during
            // the stabilization, then over-correct on the next state
            // transition). The speed_damp term (the phugoid damper) is
            // a PROPORTIONAL term, not an integral — it doesn't wind up.
            air_steering.reset_integrators();
            // Do NOT publish PrecontactReport yet — the USAF procedure
            // requires the receiver to STABILIZE at the pre-contact
            // position before calling "Precontact." The update loop
            // monitors |VS| < 200 fpm for 2 s, then publishes.
        })
        .on_enter(RefuelState::ClearedContact, [this](const RefuelEvent&) {
            published_contact_request_ = false;
            hold_time_s_ = 0.0;
            air_steering.reset_integrators();
        })
        .on_enter(RefuelState::Hold, [this](const RefuelEvent&) {
            hold_time_s_ = 0.0;
            air_steering.reset_integrators();
        })
        .on_enter(RefuelState::BackingOut, [this](const RefuelEvent&) {
            published_precontact_report_ = false;
            // Publish DisconnectRequest (the USAF "Disconnect" call).
            if (bus_ && tanker_id_ != 0) {
                atc::DisconnectRequest req;
                req.receiver_id = ownship_id_;
                req.tanker_id = tanker_id_;
                bus_->publish(req);
            }
        })
        .on_enter(RefuelState::Done, [this](const RefuelEvent&) {
            // The brain reads is_complete() to hand back to the nav module.
        })
        .build();
}

// ============================================================================
// Initialization — subscribe to the refuel response messages.
// ============================================================================
void RefuelModule::initialize(
    std::uint64_t ownship_id,
    entities::EntityWorld& world,
    messaging::MessageBus& bus)
{
    ownship_id_ = ownship_id;
    world_ = &world;
    bus_ = &bus;

    bus.subscribe<atc::TankerAssigned>([this](const atc::TankerAssigned& msg) {
        if (msg.receiver_id == ownship_id_) {
            tanker_id_      = msg.tanker_id;
            tanker_picture_.valid       = true;
            tanker_picture_.position     = msg.tanker_position;
            tanker_picture_.heading_rad  = msg.tanker_heading_rad;
            tanker_picture_.altitude_msl_ft = msg.ar_altitude_ft;
            tanker_picture_.speed_kts    = 0.0;
            deferred_event_ = RefuelEvent::TankerAssigned;
        }
    });
    bus.subscribe<atc::ClearToContact>([this](const atc::ClearToContact& msg) {
        if (msg.receiver_id == ownship_id_) {
            deferred_event_ = RefuelEvent::ClearToContact;
        }
    });
    bus.subscribe<atc::ContactMade>([this](const atc::ContactMade& msg) {
        if (msg.receiver_id == ownship_id_) {
            deferred_event_ = RefuelEvent::ContactMade;
        }
    });
    bus.subscribe<atc::ContactLost>([this](const atc::ContactLost& msg) {
        if (msg.receiver_id == ownship_id_) {
            deferred_event_ = RefuelEvent::ContactLost;
        }
    });
    bus.subscribe<atc::DisconnectApproved>([this](const atc::DisconnectApproved& msg) {
        if (msg.receiver_id == ownship_id_) {
            deferred_event_ = RefuelEvent::DisconnectApproved;
        }
    });
    bus.subscribe<atc::FuelTransferred>([this](const atc::FuelTransferred& msg) {
        if (msg.receiver_id == ownship_id_) {
            fuel_received_lbs_ = msg.fuel_lbs;
        }
    });
    bus.subscribe<atc::RefuelComplete>([this](const atc::RefuelComplete& msg) {
        if (msg.receiver_id == ownship_id_) {
            // Legacy: treat as a disconnect trigger.
            deferred_event_ = RefuelEvent::ReceiverRequestsDisconnect;
        }
    });
    bus.subscribe<atc::DisconnectMessage>([this](const atc::DisconnectMessage& msg) {
        if (msg.receiver_id == ownship_id_) {
            deferred_event_ = RefuelEvent::ReceiverRequestsDisconnect;
        }
    });

    sm_.reset();
    if (deferred_event_) {
        const auto ev = *deferred_event_;
        deferred_event_.reset();
        sm_.process(ev);
    }
}

// ============================================================================
// Per-tick update
// ============================================================================
AIControlOutput RefuelModule::update(double dt, const flight::IAircraftState* state)
{
    cache_aircraft_state(state);

    if (deferred_event_) {
        const auto ev = *deferred_event_;
        deferred_event_.reset();
        sm_.process(ev);
    }

    state_time_s_ += dt;

    // Geometry transition checks (bounded loop for chained transitions).
    for (int iter = 0; iter < 4; ++iter) {
        const auto before = sm_.current();
        switch (sm_.current()) {
            case RefuelState::Rendezvous:
            case RefuelState::BackingOut:
                check_at_precontact();
                break;
            case RefuelState::PreContact:
                // USAF stabilization: only publish PrecontactReport after
                // the receiver has been at the pre-contact position with
                // |VS| < 200 fpm for 2 s. This prevents entering
                // ClearedContact with a large climb rate from the spawn
                // transient (the receiver would climb out of the contact
                // envelope before the boom could latch).
                if (std::abs(current_vs_fpm_) < 200.0) {
                    precontact_stable_time_s_ += dt;
                } else {
                    precontact_stable_time_s_ = 0.0;
                }
                if (precontact_stable_time_s_ > 2.0 &&
                    !published_precontact_report_ && bus_ && tanker_id_ != 0) {
                    // Require |VS| < 100 fpm (tighter than the ContactLost
                    // gate's 200) so the receiver enters ClearedContact with
                    // a small enough VS that the Hold's VS damper can kill
                    // it before the receiver drifts out of the ±15 ft
                    // contact envelope.
                    if (std::abs(current_vs_fpm_) < 200.0) {
                        atc::PrecontactReport rep;
                        rep.receiver_id = ownship_id_;
                        rep.tanker_id = tanker_id_;
                        bus_->publish(rep);
                        published_precontact_report_ = true;
                    }
                }
                break;
            case RefuelState::ClearedContact:
                check_in_contact_envelope();
                break;
            case RefuelState::Hold:
                check_contact_lost();
                if (sm_.current() == RefuelState::Hold) check_auto_disconnect();
                break;
            case RefuelState::Departing:
                check_reached_departure();
                break;
            case RefuelState::NoTanker:
            case RefuelState::Done:
                break;
        }
        check_tanker_lost();
        if (sm_.current() == before) break;
    }

    if (sm_.current() == RefuelState::Hold) {
        hold_time_s_ += dt;
    }

    switch (sm_.current()) {
        case RefuelState::NoTanker:       return controls_for_no_tanker();
        case RefuelState::Rendezvous:     return controls_for_rendezvous();
        case RefuelState::PreContact:     return controls_for_precontact();
        case RefuelState::ClearedContact: return controls_for_cleared_contact();
        case RefuelState::Hold:           return controls_for_hold();
        case RefuelState::BackingOut:     return controls_for_backing_out();
        case RefuelState::Departing:      return controls_for_departing();
        case RefuelState::Done:           return controls_for_done();
    }
    return {};
}

// ============================================================================
// Transition checks
// ============================================================================
void RefuelModule::check_at_precontact()
{
    const auto s = sm_.current();
    if (s != RefuelState::Rendezvous && s != RefuelState::BackingOut) return;
    if (!tanker_picture_.valid) return;
    if (in_precontact_envelope()) {
        sm_.process(RefuelEvent::AtPrecontactPos);
    }
}

void RefuelModule::check_in_contact_envelope()
{
    if (sm_.current() != RefuelState::ClearedContact) return;
    if (!tanker_picture_.valid) return;
    // In the contact envelope: publish ContactRequest (one-shot).
    // The stub auto-acks ContactMade -> Hold.
    // Gate: only request contact when |VS| < 150 fpm — the closure from
    // pre-contact to contact can re-excite the VS (the speed change shifts
    // the trim). If the receiver enters Hold with a large VS, the VS
    // damper takes several seconds to kill it, during which the receiver
    // drifts out of the ±15 ft envelope. Waiting for |VS| < 150 before
    // requesting contact ensures the receiver enters Hold with a small
    // enough VS that the damper holds the ±15 ft envelope.
    if (in_contact_envelope() && !published_contact_request_ && bus_ && tanker_id_ != 0) {
            atc::ContactRequest req;
            req.receiver_id = ownship_id_;
            req.tanker_id = tanker_id_;
            bus_->publish(req);
            published_contact_request_ = true;
    }
}

void RefuelModule::check_contact_lost()
{
    if (sm_.current() != RefuelState::Hold) return;
    if (!tanker_picture_.valid) return;
    // Adaptive debounce: give the VS damper 3 s to kill the residual VS
    // before checking the contact envelope. The receiver enters Hold with
    // |VS| < 100 (the PreContact gate), but the FCS lag means the VS
    // takes ~2-3 s to damp. During that time the receiver drifts — the
    // 3 s debounce lets the damper work before the envelope check fires.
    if (hold_time_s_ < 1.0) return;
    if (std::abs(current_vs_fpm_) > 200.0) return;
    if (!in_contact_envelope()) {
        if (bus_ && tanker_id_ != 0) {
            atc::ContactLost lost;
            lost.receiver_id = ownship_id_;
            lost.tanker_id = tanker_id_;
            lost.reason = "drifted";
            bus_->publish(lost);
        }
        sm_.process(RefuelEvent::ContactLost);
    }
}

void RefuelModule::check_auto_disconnect()
{
    if (sm_.current() != RefuelState::Hold) return;
    // Auto-trigger disconnect after the configured hold time, OR when
    // fuel reaches the target. The host can also trigger it by publishing
    // DisconnectMessage/RefuelComplete (latched as ReceiverRequestsDisconnect).
    bool trigger = false;
    if (config.auto_disconnect_hold_s > 0.0 && hold_time_s_ >= config.auto_disconnect_hold_s) {
        trigger = true;
    }
    if (config.fuel_complete_lbs > 0.0 && fuel_lbs_ >= config.fuel_complete_lbs) {
        trigger = true;
    }
    if (trigger) {
        sm_.process(RefuelEvent::ReceiverRequestsDisconnect);
    }
}

void RefuelModule::check_reached_departure()
{
    if (sm_.current() != RefuelState::Departing) return;
    if (!tanker_picture_.valid) return;
    if (reached_departure()) {
        sm_.process(RefuelEvent::ReachedDeparture);
    }
}

void RefuelModule::check_tanker_lost()
{
    const auto s = sm_.current();
    if (s == RefuelState::NoTanker || s == RefuelState::Done) return;
    if (!tanker_picture_.valid) {
        sm_.process(RefuelEvent::TankerLost);
    }
}

// ============================================================================
// Per-state control logic
// ============================================================================
AIControlOutput RefuelModule::controls_for_no_tanker() const
{
    AIControlOutput out;
    out.roll_cmd = std::clamp(-2.0 * current_roll_rad_, -0.1, 0.1);
    out.pitch_cmd = std::clamp(-1.5 * current_pitch_rad_, -0.1, 0.1);
    out.throttle_cmd = 0.5;
    out.speed_brake_cmd = -1.0;
    return out;
}

AIControlOutput RefuelModule::controls_for_rendezvous() const
{
    // Lead-pursuit to the pre-contact point. When the receiver starts
    // near the pre-contact position (the scenario places it there), this
    // is a short closure handled by the AAR-tightened air_steering gains.
    // For a long-range rendezvous (thousands of ft), the scenario should
    // use a separate nav phase before the WP_REFUEL waypoint (the nav
    // module closes the distance; the refuel rung takes over at the
    // pre-contact area).
    if (!tanker_picture_.valid) return controls_for_no_tanker();
    const auto cp = precontact_point();
    const double desired_heading = AirSteering::bearing_to(current_position_, cp);
    const double target_alt = cp.z;
    const double target_speed_vcas = tanker_picture_.speed_kts + config.closure_bias_kts;
    return air_steering.steer(desired_heading, target_alt, target_speed_vcas,
                              steering_input());
}

AIControlOutput RefuelModule::controls_for_precontact() const
{
    // PreContact: stabilize at the pre-contact position. Use air_steering's
    // FULL cascade with moderate gains (vs_gain=3, max_vs=1500) — the
    // altitude loop has enough authority to pull the receiver back to the
    // pre-contact altitude, and the speed loop's integral finds the right
    // throttle for 10000 ft. The integrators are reset on state entry to
    // prevent the spawn-transient windup. The speed_damp term inside steer()
    // provides phugoid damping (nose-down when fast, nose-up when slow).
    if (!tanker_picture_.valid) return controls_for_no_tanker();
    const auto pp = precontact_point();
    const double save_vs = air_steering.vs_gain;
    const double save_maxvs = air_steering.max_vs_fpm;
    const double save_alt_int = air_steering.alt_integral_gain;
    const double save_alt_max = air_steering.alt_integral_max;
    const double save_att = air_steering.attitude_gain;
    const double save_prd = air_steering.pitch_rate_damp;
    air_steering.vs_gain = 3.0;
    air_steering.max_vs_fpm = 1500.0;
    air_steering.alt_integral_gain = 0.6;
    air_steering.alt_integral_max = 200.0;
    air_steering.attitude_gain = 1.5;
    air_steering.pitch_rate_damp = 0.8;
    auto out = air_steering.steer(tanker_picture_.heading_rad, pp.z,
                                  tanker_picture_.speed_kts, steering_input());
    air_steering.vs_gain = save_vs;
    air_steering.max_vs_fpm = save_maxvs;
    air_steering.alt_integral_gain = save_alt_int;
    air_steering.alt_integral_max = save_alt_max;
    air_steering.attitude_gain = save_att;
    air_steering.pitch_rate_damp = save_prd;
    return out;
}

AIControlOutput RefuelModule::controls_for_cleared_contact() const
{
    // USAF: close from pre-contact to contact. Same air_steering cascade
    // as PreContact with moderate gains. The speed target includes a
    // closure bias so the receiver closes the 40 ft gap. The integrators
    // are reset on state entry.
    if (!tanker_picture_.valid) return controls_for_no_tanker();
    const double save_vs = air_steering.vs_gain;
    const double save_maxvs = air_steering.max_vs_fpm;
    const double save_alt_int = air_steering.alt_integral_gain;
    const double save_alt_max = air_steering.alt_integral_max;
    const double save_att = air_steering.attitude_gain;
    const double save_prd = air_steering.pitch_rate_damp;
    air_steering.vs_gain = 3.0;
    air_steering.max_vs_fpm = 1500.0;
    air_steering.alt_integral_gain = 0.6;
    air_steering.alt_integral_max = 200.0;
    air_steering.attitude_gain = 1.5;
    air_steering.pitch_rate_damp = 0.8;
    const double target_speed = tanker_picture_.speed_kts + 1.0;
    auto out = air_steering.steer(tanker_picture_.heading_rad,
                                  tanker_picture_.altitude_msl_ft,
                                  target_speed, steering_input());
    air_steering.vs_gain = save_vs;
    air_steering.max_vs_fpm = save_maxvs;
    air_steering.alt_integral_gain = save_alt_int;
    air_steering.alt_integral_max = save_alt_max;
    air_steering.attitude_gain = save_att;
    air_steering.pitch_rate_damp = save_prd;
    return out;
}

AIControlOutput RefuelModule::controls_for_hold() const
{
    // USAF: hold on the boom receptacle during fuel transfer. The boom
    // disconnect envelope is ±15 ft. The receiver must stay within this
    // envelope for the full hold duration (30s).
    //
    // Control law: air_steering for SPEED (the integral finds the right
    // throttle for the altitude) + a VS damper for PITCH (kills the
    // phugoid). The altitude loop is NOT used — it fights the speed
    // loop (the phugoid speed/altitude exchange). The VS damper keeps
    // the VS near zero so the altitude drifts slowly (the receiver
    // enters Hold at the contact altitude; if VS ≈ 0, the drift is
    // minimal). The key: the receiver enters Hold with |VS| < 200 (the
    // PreContact stabilization gate), so the VS damper only needs to
    // kill a small residual.
    //
    // The VS damper gain is 0.0005/fpm (stronger than PreContact's 0.0002)
    // because the ±15 ft envelope is tight — at 200 fpm, the receiver
    // drifts 3.3 ft/s, exceeding the envelope in 5s. The 0.0005 gain
    // gives 0.10 correction at 200 fpm, killing the VS within 2-3s.
    if (!tanker_picture_.valid) return controls_for_no_tanker();
    // Air_steering for speed only — gentle altitude (for trim), override pitch.
    const double save_vs = air_steering.vs_gain;
    const double save_maxvs = air_steering.max_vs_fpm;
    const double save_alt_int = air_steering.alt_integral_gain;
    const double save_alt_max = air_steering.alt_integral_max;
    const double save_att = air_steering.attitude_gain;
    const double save_prd = air_steering.pitch_rate_damp;
    air_steering.vs_gain = 0.5;
    air_steering.max_vs_fpm = 100.0;
    air_steering.alt_integral_gain = 0.0;
    air_steering.alt_integral_max = 0.0;
    air_steering.attitude_gain = 0.3;
    air_steering.pitch_rate_damp = 0.5;
    auto out = air_steering.steer(tanker_picture_.heading_rad,
                                  tanker_picture_.altitude_msl_ft,
                                  tanker_picture_.speed_kts, steering_input());
    air_steering.vs_gain = save_vs;
    air_steering.max_vs_fpm = save_maxvs;
    air_steering.alt_integral_gain = save_alt_int;
    air_steering.alt_integral_max = save_alt_max;
    air_steering.attitude_gain = save_att;
    air_steering.pitch_rate_damp = save_prd;
    // Override pitch with a STRONG VS damper.
    const double vs_damp = std::clamp(-current_vs_fpm_ * 0.0005, -0.15, 0.15);
    out.pitch_cmd = std::clamp(vs_damp, -0.15, 0.15);
    return out;
}

AIControlOutput RefuelModule::controls_for_backing_out() const
{
    // USAF: after requesting disconnect, the receiver backs out to the
    // pre-contact position (50 ft behind, 10 ft below). The receiver
    // reduces throttle to fall back, then stabilizes at pre-contact.
    // Same neutral pitch/roll + proportional speed law, but with a
    // REVERSE along correction (ahead → reduce throttle to fall back).
    if (!tanker_picture_.valid) return controls_for_no_tanker();
    const double along = along_err_ft();
    AIControlOutput out;
    out.speed_brake_cmd = -1.0;
    out.roll_cmd = std::clamp(-2.0 * current_roll_rad_, -0.1, 0.1);
    out.pitch_cmd = std::clamp(-1.0 * current_pitch_rad_, -0.15, 0.15);
    // Proportional speed law matching the tanker + a REVERSE along
    // correction: ahead of pre-contact (along > -50) → reduce throttle
    // to fall back to pre-contact.
    const double speed_err = tanker_picture_.speed_kts - current_vcas_kts_;
    const double speed_corr = std::clamp(0.01 * speed_err, -0.08, 0.08);
    // The target along is -50 (pre-contact). If along > -50 (too close),
    // reduce throttle. If along < -50 (too far), add throttle.
    const double along_to_precontact = along + 50.0;  // + = too close
    const double along_corr = std::clamp(-0.001 * along_to_precontact, -0.05, 0.05);
    out.throttle_cmd = std::clamp(kCruiseThrottle + speed_corr + along_corr, 0.05, 0.40);
    return out;
}

AIControlOutput RefuelModule::controls_for_departing() const
{
    // USAF: the receiver descends to 1000 ft below the tanker for
    // vertical separation, then resumes own navigation. The descent is
    // a gentle power reduction — reduce throttle + hold a slight nose-
    // down attitude. No altitude-chase law (that would excite the
    // phugoid). The check_reached_departure transition fires when within
    // 50 ft of the target altitude (1000 ft below the tanker).
    if (!tanker_picture_.valid) return controls_for_no_tanker();
    AIControlOutput out;
    out.speed_brake_cmd = -1.0;
    out.roll_cmd = std::clamp(-2.0 * current_roll_rad_, -0.1, 0.1);
    out.pitch_cmd = std::clamp(-1.0 * current_pitch_rad_ - 0.02, -0.15, 0.05);
    out.throttle_cmd = 0.15;  // reduce power for the descent
    return out;
}

AIControlOutput RefuelModule::controls_for_done() const
{
    AIControlOutput out;
    out.roll_cmd = std::clamp(-2.0 * current_roll_rad_, -0.1, 0.1);
    out.pitch_cmd = std::clamp(-1.5 * current_pitch_rad_, -0.1, 0.1);
    out.throttle_cmd = 0.5;
    out.speed_brake_cmd = -1.0;
    return out;
}

// ============================================================================
// Geometry helpers
// ============================================================================
geo::WorldPosition RefuelModule::precontact_point() const
{
    if (!tanker_picture_.valid) return current_position_;
    const double h = tanker_picture_.heading_rad;
    const double fwd_x = std::sin(h);
    const double fwd_y = std::cos(h);
    // Total aft offset = boom_offset (aircraft root → boom nozzle)
    // + precontact_offset (boom nozzle → pre-contact position).
    const double total_aft = config.boom_offset_long_ft +
                             config.precontact_offset_long_ft;
    geo::WorldPosition p;
    p.x = tanker_picture_.position.x - fwd_x * total_aft;
    p.y = tanker_picture_.position.y - fwd_y * total_aft;
    p.z = tanker_picture_.altitude_msl_ft + config.precontact_offset_vert_ft;
    return p;
}

geo::WorldPosition RefuelModule::contact_point() const
{
    if (!tanker_picture_.valid) return current_position_;
    const double h = tanker_picture_.heading_rad;
    const double fwd_x = std::sin(h);
    const double fwd_y = std::cos(h);
    // Total aft offset = boom_offset (aircraft root → boom nozzle)
    // + contact_offset (boom nozzle → contact position).
    const double total_aft = config.boom_offset_long_ft +
                             config.contact_offset_long_ft;
    geo::WorldPosition p;
    p.x = tanker_picture_.position.x - fwd_x * total_aft;
    p.y = tanker_picture_.position.y - fwd_y * total_aft;
    p.z = tanker_picture_.altitude_msl_ft;
    return p;
}

geo::WorldPosition RefuelModule::departure_point() const
{
    if (!tanker_picture_.valid) return current_position_;
    // The departure target: 1000 ft below the tanker, matching its
    // track. The receiver descends to this altitude, then the brain
    // hands back to the nav module.
    geo::WorldPosition p;
    p.x = tanker_picture_.position.x;
    p.y = tanker_picture_.position.y;
    p.z = tanker_picture_.altitude_msl_ft + config.departure_offset_vert_ft;
    return p;
}

double RefuelModule::along_err_ft() const
{
    if (!tanker_picture_.valid) return 0.0;
    const auto cp = contact_point();
    const double h = tanker_picture_.heading_rad;
    const double fwd_x = std::sin(h);
    const double fwd_y = std::cos(h);
    return (current_position_.x - cp.x) * fwd_x +
           (current_position_.y - cp.y) * fwd_y;
}

double RefuelModule::lat_err_ft() const
{
    if (!tanker_picture_.valid) return 0.0;
    const auto cp = contact_point();
    const double h = tanker_picture_.heading_rad;
    const double right_x = std::cos(h);
    const double right_y = -std::sin(h);
    return (current_position_.x - cp.x) * right_x +
           (current_position_.y - cp.y) * right_y;
}

double RefuelModule::vert_err_ft() const
{
    if (!tanker_picture_.valid) return 0.0;
    return current_alt_msl_ft_ - tanker_picture_.altitude_msl_ft;
}

bool RefuelModule::in_precontact_envelope() const
{
    if (!tanker_picture_.valid) return false;
    const auto pp = precontact_point();
    const double h = tanker_picture_.heading_rad;
    const double fwd_x = std::sin(h), fwd_y = std::cos(h);
    const double right_x = std::cos(h), right_y = -std::sin(h);
    const double dx = current_position_.x - pp.x;
    const double dy = current_position_.y - pp.y;
    const double dz = current_position_.z - pp.z;
    const double along = dx * fwd_x + dy * fwd_y;
    const double lat   = dx * right_x + dy * right_y;
    return std::abs(along) < config.precontact_long_ft &&
           std::abs(lat)   < config.precontact_lat_ft &&
           std::abs(dz)    < config.precontact_vert_ft;
}

bool RefuelModule::in_contact_envelope() const
{
    return std::abs(along_err_ft()) < config.contact_long_ft &&
           std::abs(lat_err_ft())   < config.contact_lat_ft &&
           std::abs(vert_err_ft())  < config.contact_vert_ft;
}

bool RefuelModule::reached_departure() const
{
    if (!tanker_picture_.valid) return false;
    const double target_alt = tanker_picture_.altitude_msl_ft +
                              config.departure_offset_vert_ft;
    // Reached when within 50 ft of the departure altitude.
    return std::abs(current_alt_msl_ft_ - target_alt) < 50.0;
}

// ============================================================================
// State caching + steering input
// ============================================================================
void RefuelModule::cache_aircraft_state(const flight::IAircraftState* state)
{
    if (state == nullptr) return;
    current_position_ = geo::WorldPosition(
        state->position_east_ft(),
        state->position_north_ft(),
        state->altitude_msl_ft());
    current_vcas_kts_       = state->vcas_kts();
    current_alt_msl_ft_     = state->altitude_msl_ft();
    current_heading_rad_   = state->heading_rad();
    current_pitch_rad_      = state->pitch_angle_rad();
    current_roll_rad_       = state->roll_angle_rad();
    current_roll_rate_radps_ = state->roll_rate_radps();
    current_pitch_rate_radps_ = state->pitch_rate_radps();
    current_vs_fpm_        = state->vertical_speed_fpm();
    fuel_lbs_              = state->fuel_lbs();
}

AirSteering::Input RefuelModule::steering_input() const noexcept
{
    AirSteering::Input in;
    in.position         = current_position_;
    in.heading_rad      = current_heading_rad_;
    in.pitch_rad        = current_pitch_rad_;
    in.roll_rad         = current_roll_rad_;
    in.roll_rate_radps  = current_roll_rate_radps_;
    in.pitch_rate_radps = current_pitch_rate_radps_;
    in.vs_fpm           = current_vs_fpm_;
    in.vcas_kts         = current_vcas_kts_;
    in.alt_msl_ft       = current_alt_msl_ft_;
    return in;
}

// ============================================================================
// Human-readable name
// ============================================================================
std::string RefuelModule::state_name() const {
    switch (sm_.current()) {
        case RefuelState::NoTanker:       return "NoTanker";
        case RefuelState::Rendezvous:     return "Rendezvous";
        case RefuelState::PreContact:     return "PreContact";
        case RefuelState::ClearedContact: return "ClearedContact";
        case RefuelState::Hold:           return "Refueling";
        case RefuelState::BackingOut:     return "BackingOut";
        case RefuelState::Departing:      return "Departing";
        case RefuelState::Done:           return "RefuelDone";
    }
    return {};
}

} // namespace f4::ai::modules
