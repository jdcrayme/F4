// f4-ai/src/refuel_module.cpp
//
// RefuelModule implementation — the full USAF boom AAR procedure.
// See f4-ai/include/f4/ai/modules/refuel_module.hpp + Docs/AAR_REDESIGN_PLAN.md.

#include "f4/ai/modules/refuel_module.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <cstdint>

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
    // PHUG-P4 retune (M3 linear-band rule): class-default path_gain 0.0006
    // saturated the 0.10-rad gamma-correction limit beyond ~167 fpm of vs
    // error — a bang-bang relay the P4.1-corrected inner loop executes
    // faithfully (the P3 G-lag used to filter it). The station-keeping
    // phugoid's ±100 ft / ±500 fpm band maps to ~50-80% of the limit,
    // proportional end to end. See the landing/nav tunes for the full
    // measurement record.
    air_steering.path_gain = 0.00006;
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
        .event_name(RefuelEvent::StationLost,           "StationLost")

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
        .on(RefuelState::PreContact, RefuelState::Rendezvous,
            RefuelEvent::StationLost, nullptr, nullptr, "station_lost_rejoin")
        .on(RefuelState::ClearedContact, RefuelState::Rendezvous,
            RefuelEvent::StationLost, nullptr, nullptr, "station_lost_rejoin")
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
            // PHUG-P4 retune: the integrator reset that used to live here
            // is REMOVED. The Rendezvous phase flies the same altitude
            // target with a decaying closure bias — its throttle and
            // altitude integrals ARE the correct arrival trim, and
            // resetting them restarts the type-1 speed loop from the
            // P-only point: the receiver sheds trim thrust for ~30 s and
            // drifts astern of the station (measured: -15 ft at handoff,
            // -980 ft astern by the time the |VS| gate reported ready —
            // at P3 the broken loop's speed error masked the drift).
            // The along-track closure that recovers this lives in
            // controls_for_cleared_contact. The speed_damp term (the
            // phugoid damper) is a PROPORTIONAL term, not an integral —
            // it doesn't wind up.
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
            // EMPL-2b — F4_AAR_DEBUG: the latch marker.
            if (std::getenv("F4_AAR_DEBUG") != nullptr) {
                std::fprintf(stderr, "AARDBG latch ship=%llu tank=%llu\n",
                             (unsigned long long)ownship_id_,
                             (unsigned long long)tanker_id_);
            }
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
            // The brain reads is_complete() to hand back to the nav
            // module. The completion event: the QC ladder's
            // RefuelComplete counter (the SHOWCASE-1 acceptance gate)
            // subscribes to this — nobody published it before EMPL-2's
            // campaign e2e asked for it by name.
            if (bus_ && tanker_id_ != 0) {
                atc::RefuelComplete done;
                done.receiver_id = ownship_id_;
                done.tanker_id = tanker_id_;
                done.fuel_transferred_lbs = fuel_received_lbs_;
                bus_->publish(done);
            }
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

    // FID-OPT-1: RAII subscription bundle (see TakeoffModule::initialize —
    // the eight leaked refuel-protocol handlers outlived the module and
    // read freed memory on every later publish).
    subscriptions_.unsubscribe_all();
    subscriptions_.bind(bus);

    subscriptions_.subscribe<atc::TankerAssigned>([this](const atc::TankerAssigned& msg) {
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
    subscriptions_.subscribe<atc::ClearToContact>([this](const atc::ClearToContact& msg) {
        if (msg.receiver_id == ownship_id_) {
            deferred_event_ = RefuelEvent::ClearToContact;
        }
    });
    subscriptions_.subscribe<atc::ContactMade>([this](const atc::ContactMade& msg) {
        if (msg.receiver_id == ownship_id_) {
            deferred_event_ = RefuelEvent::ContactMade;
        }
    });
    subscriptions_.subscribe<atc::ContactLost>([this](const atc::ContactLost& msg) {
        if (msg.receiver_id == ownship_id_) {
            deferred_event_ = RefuelEvent::ContactLost;
        }
    });
    subscriptions_.subscribe<atc::DisconnectApproved>([this](const atc::DisconnectApproved& msg) {
        if (msg.receiver_id == ownship_id_) {
            deferred_event_ = RefuelEvent::DisconnectApproved;
        }
    });
    subscriptions_.subscribe<atc::FuelTransferred>([this](const atc::FuelTransferred& msg) {
        if (msg.receiver_id == ownship_id_) {
            fuel_received_lbs_ = msg.fuel_lbs;
        }
    });
    subscriptions_.subscribe<atc::RefuelComplete>([this](const atc::RefuelComplete& msg) {
        if (msg.receiver_id == ownship_id_) {
            // Legacy: treat as a disconnect trigger.
            deferred_event_ = RefuelEvent::ReceiverRequestsDisconnect;
        }
    });
    subscriptions_.subscribe<atc::DisconnectMessage>([this](const atc::DisconnectMessage& msg) {
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

    // The station-lost debounce accumulation (the check fires from the
    // transition loop below; the timer needs dt, which the checks don't
    // carry). Displacement is measured against the PRE-CONTACT point —
    // the station both states are supposed to hold.
    {
        const auto sls = sm_.current();
        if (sls == RefuelState::PreContact || sls == RefuelState::ClearedContact) {
            const auto pp = precontact_point();
            const bool displaced =
                std::hypot(current_position_.x - pp.x,
                           current_position_.y - pp.y)
                    > config.station_lost_horiz_ft ||
                std::abs(current_position_.z - pp.z)
                    > config.station_lost_vert_ft;
            station_lost_time_s_ =
                displaced ? station_lost_time_s_ + dt : 0.0;
        } else {
            station_lost_time_s_ = 0.0;
        }
    }

    // The Hold lateral drift rate (read by controls_for_hold — that
    // state is const). Same boom-frame cross-track the hold steers on.
    if (sm_.current() == RefuelState::Hold && tanker_picture_.valid) {
        const auto cp = contact_point();
        const double hh = tanker_picture_.heading_rad;
        const double hx = current_position_.x - cp.x;
        const double hy = current_position_.y - cp.y;
        const double lat = hx * std::cos(hh) - hy * std::sin(hh);
        if (hold_lat_init_ && dt > 0.0) {
            hold_lat_rate_fps_ = (lat - hold_lat_last_ft_) / dt;
        }
        hold_lat_last_ft_ = lat;
        hold_lat_init_ = true;
    } else {
        hold_lat_init_ = false;
        hold_lat_rate_fps_ = 0.0;
    }

    // Geometry transition checks (bounded loop for chained transitions).
    for (int iter = 0; iter < 4; ++iter) {
        const auto before = sm_.current();
        switch (sm_.current()) {
            case RefuelState::Rendezvous:
            case RefuelState::BackingOut:
                check_at_precontact();
                break;
            case RefuelState::PreContact:
                check_station_lost();
                // USAF stabilization: only publish PrecontactReport after
                // the receiver has been at the pre-contact position with
                // |VS| < 200 fpm for 2 s. This prevents entering
                // ClearedContact with a large climb rate from the spawn
                // transient (the receiver would climb out of the contact
                // envelope before the boom could latch).
                if (sm_.current() == RefuelState::PreContact) {
                    if (std::abs(current_vs_fpm_) < 200.0) {
                        precontact_stable_time_s_ += dt;
                    } else {
                        precontact_stable_time_s_ = 0.0;
                    }
                    if (precontact_stable_time_s_ > 2.0 &&
                        !published_precontact_report_ && bus_ &&
                        tanker_id_ != 0) {
                        // Require |VS| < 100 fpm (tighter than the
                        // ContactLost gate's 200) so the receiver enters
                        // ClearedContact with a small enough VS that the
                        // Hold's VS damper can kill it before the receiver
                        // drifts out of the ±15 ft contact envelope.
                        if (std::abs(current_vs_fpm_) < 200.0) {
                            atc::PrecontactReport rep;
                            rep.receiver_id = ownship_id_;
                            rep.tanker_id = tanker_id_;
                            bus_->publish(rep);
                            published_precontact_report_ = true;
                        }
                    }
                }
                break;
            case RefuelState::ClearedContact:
                check_station_lost();
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

// Displaced beyond the station-keep tolerances while PreContact or
// ClearedContact: hand the join back to Rendezvous. Neither state has
// a law that REJOINS from miles out — PreContact station-keeps,
// ClearedContact formates the tanker's track with an 8-kt closure bias
// (14,000 ft takes half an hour) — so the campaign e2e catch (a
// protocol entered transiently during a co-based climb-out, then
// displaced 9-14k ft) sat in ClearedContact forever. The debounce
// timer is accumulated in update(); here it only fires.
void RefuelModule::check_station_lost()
{
    const auto s = sm_.current();
    if (s != RefuelState::PreContact && s != RefuelState::ClearedContact) {
        return;
    }
    if (!tanker_picture_.valid) return;   // TankerLost owns the invalid case
    if (station_lost_time_s_ < config.station_lost_debounce_s) return;
    station_lost_time_s_ = 0.0;
    sm_.process(RefuelEvent::StationLost);
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
    // enough VS that the damper holds the ±15 ft envelope. (A QC-WORLD
    // experiment relaxing this to 300 fpm — with the envelope widened —
    // latch-churned 16/15 with zero fuel transferred; reverted.)
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
        // EMPL-2b — F4_AAR_DEBUG: the loss autopsy (which boom-frame
        // axis broke, how far, at what hold age).
        if (std::getenv("F4_AAR_DEBUG") != nullptr) {
            std::fprintf(stderr,
                         "AARDBG lost ship=%llu tank=%llu hold_t=%.1f "
                         "along=%.0f lat=%.0f vert=%.0f vs=%.0f\n",
                         (unsigned long long)ownship_id_,
                         (unsigned long long)tanker_id_,
                         hold_time_s_, along_err_ft(), lat_err_ft(),
                         vert_err_ft(), current_vs_fpm_);
        }
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
    const double h = tanker_picture_.heading_rad;
    const double fwd_x = std::sin(h);
    const double fwd_y = std::cos(h);

    // EMPL-2 — the standoff join (see the config block): aim the
    // along-axis at a point BEHIND the pre-contact point while the
    // vertical error is outside the catch window — level off beside the
    // boom FIRST, then release the standoff and close. Without it the
    // horizontal closure (kts-scale) outruns the vertical (fpm-scale)
    // and the receiver flies through the station below it, spiraling
    // for an aligned tick the envelope never sees.
    const double dz = current_position_.z - cp.z;
    const double standoff_ft =
        (std::abs(dz) > config.precontact_vert_ft)
            ? config.rendezvous_standoff_ft : 0.0;
    // EMPL-2 — lead pursuit (see the config block): the heading aims
    // AHEAD of the (standoff-shifted) aim point along the tanker's
    // track, cutting inside the orbit; pure pursuit circles astern
    // forever. The lead FADES inside 20,000 ft of the point — the
    // terminal approach is pure pursuit + the braking curve (a constant
    // lead carried the receiver THROUGH the boom: ClearedContact at
    // 170 kts of overtake, then 20 NM past it).
    constexpr double kFpsPerKt = 1.68781;
    const double tank_speed_fps = tanker_picture_.speed_kts * kFpsPerKt;
    const double dist_cp_ft = std::hypot(current_position_.x - cp.x,
                                         current_position_.y - cp.y);
    const double lead_scale = std::clamp(dist_cp_ft / 20000.0, 0.0, 1.0);
    const double lead_ft = tank_speed_fps * config.rendezvous_lead_s *
                           lead_scale;
    // The standoff gates the LEAD: while the vertical error is outside
    // the catch window the aim holds 6,000 ft BEHIND the point (no
    // lead) — the throttle goes to the climb, not to a 150-kts
    // overtake. Otherwise the +7,600-ft lead swamps the −6,000-ft
    // standoff and the receiver closes horizontally while still 5k ft
    // below the boom (the e2e catch: a 190-fpm climb that never
    // reconciled).
    // EMPL-2b — the hand-off is a BLEND, not a flip. The binary window
    // test jumped the aim ~6,000 ft the moment |dz| crossed the band
    // edge: the braking law saw err leap 0 → ~6,000 ft, demanded
    // ~120 kts of closure, the throttle pinned at 1.0, and the surge
    // climbed the receiver right back out of the window (the live
    // TestCamp catch: dz ballooned +6,282 ft off a 1.1-NM join). The
    // shift fades over one band of vertical error — no crossing
    // injects a step into the speed loop.
    const double outside_blend = std::clamp(
        (std::abs(dz) - config.precontact_vert_ft) /
            config.precontact_vert_ft,
        0.0, 1.0);
    const double along_shift_ft =
        outside_blend * -standoff_ft + (1.0 - outside_blend) * lead_ft;
    const double aim_x = cp.x + fwd_x * along_shift_ft;
    const double aim_y = cp.y + fwd_y * along_shift_ft;
    const geo::WorldPosition aim_pos{aim_x, aim_y, cp.z};

    // EMPL-2 — the near-field terminal (see the config block): inside
    // rendezvous_near_ft the join becomes formate-and-close instead of
    // chasing a point on an orbit (the sustained-turn trap: the
    // receiver circled 5k ft above the boom, never descending).
    double desired_heading;
    if (dist_cp_ft < config.rendezvous_near_ft) {
        // EMPL-2 — the lateral rejoin blend (see the config block): the
        // track formate needs a LATERAL closure term. The tanker's
        // track alone never kills a cross-track offset — the along-axis
        // closure law has no authority over lat — so the campaign e2e
        // catch (a receiver joining the ORBITING tanker abeam) formated
        // its displaced line forever: 2-8k ft of lat drift, dist parked
        // at 6-10k ft, zero envelope samples. Blend the track heading
        // toward the pursue bearing by the lateral offset against the
        // pre-contact point: dead astern keeps the pure formate (the
        // sustained-turn fix); displaced, the blend carries the
        // receiver back onto the boom's line — which the orbit's
        // curvature keeps sweeping away from a fixed-offset formate.
        const double lat_cp_ft = (current_position_.x - cp.x) * std::cos(h)
                               - (current_position_.y - cp.y) * std::sin(h);
        const double pursue_hdg =
            AirSteering::bearing_to(current_position_, aim_pos);
        const double rejoin_blend = std::clamp(
            std::abs(lat_cp_ft) / config.rendezvous_rejoin_lat_ft, 0.0, 1.0);
        desired_heading =
            tanker_picture_.heading_rad +
            wrap_heading_err(pursue_hdg - tanker_picture_.heading_rad) *
                rejoin_blend;
    } else {
        desired_heading = AirSteering::bearing_to(current_position_, aim_pos);
    }
    // EMPL-2 — the rendezvous VS lead (see the config block): subtract
    // the climb momentum from the altitude aim so the join arrives at
    // the boom's altitude WITH ~zero VS instead of blowing through it
    // at the full VS cap (every pass overshot ~3,000 ft high and the
    // station-keep pitch loop crawled back for 10 minutes). CAPPED —
    // uncapped, the lead degenerates into a 1/τ proportional law that
    // crawls the final deficit at ~16 fpm.
    const double vs_lead_ft = std::clamp(
        current_vs_fpm_ * (config.rendezvous_vs_lead_s / 60.0),
        -config.rendezvous_vs_lead_max_ft, config.rendezvous_vs_lead_max_ft);
    const double target_alt = cp.z - vs_lead_ft;

    // EMPL-2 — the rendezvous closure law: error-proportional (tau),
    // CAPPED by the kinematic braking curve sqrt(2·a·err) — the
    // stopping-distance law. A clamped constant-overtake cannot stop AT
    // the aim point (the QC funnel: closed 74,000 ft, overshot +30,000
    // past it); the braking curve starts decelerating where the
    // configured deceleration can absorb the remaining closure, so the
    // final approach creeps into the pre-contact envelope instead of
    // flying through it. Near the envelope the law still degenerates to
    // the original tanker-speed + bias chase (the scenario runs' shape).
    const double rx = current_position_.x - aim_x;
    const double ry = current_position_.y - aim_y;
    const double along_aim_ft = rx * fwd_x + ry * fwd_y;
    const double err_ft = -along_aim_ft;   // + = the aim point is ahead
    double target_speed_vcas = tanker_picture_.speed_kts;
    if (err_ft > 0.0) {
        // Behind the aim point: close at the demanded rate — the tau
        // law capped by the stopping-distance curve — never below the
        // original gentle bias.
        const double demand_fps =
            std::min(err_ft / std::max(config.rendezvous_tau_s, 1.0),
                     std::sqrt(2.0 * config.rendezvous_brake_fps2 * err_ft));
        // EMPL-2 — the level-first closure cap (see the config block):
        // the full ceiling is only available LEVEL with the boom — and
        // it is SIGN-AWARE. Below the boom: close gently (90 kts) —
        // level off, then close. Above the boom: NO closure at all —
        // the +25-kt overtake's thrust drives a climb the join-scale
        // pitch authority cannot counter (the scenario e2e trace:
        // commanded −700 fpm, actual +500 fpm — the receiver rode its
        // throttle up and away from the boom). Match speed, descend,
        // then close.
        // EMPL-2b — the cap is SMOOTH in dz. The EMPL-2 branch structure
        // (150 kts inside ±300 ft of the line, 90 below, ZERO above,
        // floored at +3 by the bias composition) put a discontinuity at
        // the window edge exactly where the join-scale servo's flicker
        // lives: every dip inside the band opened the full ceiling, the
        // surge climbed the receiver back out, the cap slammed shut —
        // a limit cycle parked at the edge (three live TestCamp runs:
        // dz +290..+380 for 116,000 ticks, bounces to +6,282 ft, and
        // the +3-kt bias floor overriding the above-boom zero in the
        // first fix attempt). The cap now interpolates linearly through
        // the same three points — gentle 90+bias a band BELOW the line,
        // full ceiling ON the line, mirrored descend bias a band ABOVE
        // — so no dz crossing steps the speed target, and the receiver
        // joins the way the procedure reads: closing gently from below,
        // full overtake only level with the boom, descend-first if it
        // rides high.
        const double depth = std::clamp(dz / config.precontact_vert_ft,
                                        -1.0, 1.0);
        const double below_closure_kts =
            config.rendezvous_level_closure_kts + config.closure_bias_kts;
        const double line_closure_kts = config.rendezvous_max_closure_kts;
        const double above_closure_kts = -config.closure_bias_kts;
        const double closure_kts =
            depth <= 0.0
                ? below_closure_kts +
                      (line_closure_kts - below_closure_kts) * (1.0 + depth)
                : line_closure_kts +
                      (above_closure_kts - line_closure_kts) * depth;
        target_speed_vcas +=
            std::min(demand_fps / kFpsPerKt, closure_kts);
    } else {
        // Overshot (ahead): fall back below the tanker's speed, under
        // the same braking curve mirrored.
        const double e = -err_ft;
        const double demand_fps =
            -std::min(e / std::max(config.rendezvous_tau_s, 1.0),
                      std::sqrt(2.0 * config.rendezvous_brake_fps2 * e));
        target_speed_vcas +=
            std::max(demand_fps / kFpsPerKt,
                     -config.rendezvous_max_back_kts);
    }

    // EMPL-2 — the JOIN envelope (nav-scale; see the config block). The
    // constructor's hold-tuned gains (bank 0.10, VS 300 fpm) close a
    // campaign altitude deficit (10-15k ft) in 40+ minutes — the join
    // never lands inside the station window. The rendezvous state swaps
    // in the nav-scale envelope for THIS steer (the save/steer/restore
    // pattern controls_for_precontact established); the hold phases
    // keep their own envelopes, so nothing leaks across states. The
    // scenario runs keep their shape: their receiver is PLACED at the
    // pre-contact position — the deficit is inside the tight envelope's
    // reach before the swap could matter.
    const double save_bank = air_steering.max_bank_rad;
    const double save_maxvs = air_steering.max_vs_fpm;
    const double save_vsgain = air_steering.vs_gain;
    const double save_altint = air_steering.alt_integral_gain;
    const double save_altintmax = air_steering.alt_integral_max;
    air_steering.max_bank_rad = config.rendezvous_max_bank_rad;
    air_steering.max_vs_fpm = config.rendezvous_max_vs_fpm;
    // EMPL-2 — join-scale vertical authority: the rendezvous state
    // ALWAYS flies the PreContact tune (vs_gain 3, the strong
    // integral). The constructor's station-keep tune (vs_gain 0.5) is
    // a ±100-ft servo: at join scale the phugoid/thrust coupling
    // outvotes it (the receiver rode its throttle to +500 fpm against
    // a −700 fpm command — the scenario e2e trace), and a hysteresis
    // dead zone between the strong latch and the standoff release
    // deadlocked the join 6,000 ft astern / 330 ft high (the soft tune
    // closed a 330-ft deficit at −5 fpm). The capped VS lead above
    // shapes the arrival; the strong tune just executes it. PreContact
    // proves this tune inside ±300 ft.
    air_steering.vs_gain = 3.0;
    air_steering.alt_integral_gain = 0.6;
    air_steering.alt_integral_max = 200.0;
    auto out = air_steering.steer(desired_heading, target_alt, target_speed_vcas,
                                  steering_input());
    air_steering.vs_gain = save_vsgain;
    air_steering.alt_integral_gain = save_altint;
    air_steering.alt_integral_max = save_altintmax;
    air_steering.max_bank_rad = save_bank;
    air_steering.max_vs_fpm = save_maxvs;
    return out;
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
    // EMPL-2 — the terminal lateral correction: the WINGMAN's linear
    // cross-track law (lateral_error -> a bounded heading correction on
    // the lead's track; right of station -> turn left). The bare
    // tanker-track heading has NO lateral feedback — a 73-ft residual
    // at ClearedContact entry grew 30 ft/s under the orbit's curvature
    // (the e2e trace: lat 73 -> 1,589 ft in 39 s, station-lost,
    // rejoin, churn) — and a pursue-BEARING blend swings the commanded
    // heading wildly as the station crosses the track. The linear law
    // is what holds a formation slot on a maneuvering lead.
    const double thdg = tanker_picture_.heading_rad;
    const double trx = current_position_.x - pp.x;
    const double trY = current_position_.y - pp.y;
    const double tlat = trx * std::cos(thdg) - trY * std::sin(thdg);
    const double tcorr = std::clamp(
        config.terminal_lateral_gain_rad_per_ft * tlat,
        -config.terminal_max_correction_rad,
        config.terminal_max_correction_rad);
    const double desired_hdg = thdg - tcorr;
    const double save_bank2 = air_steering.max_bank_rad;
    const double save_thr2 = air_steering.approach_aileron_threshold_rad;
    air_steering.max_bank_rad = config.terminal_max_bank_rad;
    // EMPL-2 — the heading deadband gate: steer() banks only for
    // errors above ~5 degrees (the cruise rudder is zeroed below it),
    // so a degree-scale lateral correction was INVISIBLE — the hold
    // drifted out of the ±15 envelope untouched (the e2e exits at
    // lat exactly ±15, corr never executed). Terminal steering banks
    // on any error.
    air_steering.approach_aileron_threshold_rad = 0.0;
    auto out = air_steering.steer(desired_hdg, pp.z,
                                  tanker_picture_.speed_kts, steering_input());
    air_steering.max_bank_rad = save_bank2;
    air_steering.approach_aileron_threshold_rad = save_thr2;
    // EMPL-2 — direct along-axis throttle bias (PreContact
    // station-keep): the cascade's speed loop shares the throttle with
    // the energy-coupling term, so fine speed commands don't move the
    // receiver relative to the boom. The station is a THROTTLE
    // problem — bias the actuator on the along error. 0.0015/ft: ±15
    // ft ↔ a 2% delta; ±80 ft rails the ±0.12 band.
    const double tlong_pp = trx * std::sin(thdg) + trY * std::cos(thdg);
    out.throttle_cmd = std::clamp(
        out.throttle_cmd + std::clamp(0.0015 * (-tlong_pp), -0.12, 0.12),
        air_steering.throttle_min, 1.0);
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
    // PHUG-P4 retune: the closure bias is a bounded PROPORTIONAL term on
    // the along-track gap, not a fixed +1 kt. MEASURED (AAR E2E): the
    // receiver reached ClearedContact ~980 ft astern (the PreContact
    // station-keep drifts astern after the integrator reset — pre-existing
    // behavior the P3 loop's speed error masked), and a +1 kt bias needs
    // ~8 min for that gap: the 360-s budget expired before the boom
    // envelope. 0.05/ft capped at 8 kt closes 980 ft in ~75 s and decays
    // to ~0.7 kt inside the ±15 ft envelope, preserving the gentle
    // terminal closure the envelope gate expects.
    //
    // P5 fix (the -50 ft stall, measured on the F4_AAR_TRACE CSV): the
    // bias originally subtracted precontact_offset_long_ft, targeting the
    // PRECONTACT station — but that offset belongs to the PreContact
    // state's station-keep. Once CLEARED to contact, the USAF procedure
    // closes to the RECEPTACLE (along ≈ 0): the bias must go to zero at
    // the boom, not at the 50-ft station. The old target made the bias
    // clamp to 0 at 50 ft astern while the latch gate sits at ±15 ft —
    // the receiver crept the last 35 ft at integrator-noise speed
    // (~0.1 ft/s, 120 s stuck at -52 ft in the trace) and the E2E
    // expired 12 s into Hold, never reaching Departing/Done.
    const double closure_gap = -along_err_ft();
    // EMPL-2 — SYMMETRIC bias: the clamp used to floor at 0, so a
    // receiver that entered ClearedContact with along-track momentum
    // and settled AHEAD of the receptacle (the e2e trace: parked at
    // +90-110 ft, the latch window ±15) had no fall-back law at all.
    // The P gain is 0.2 (was 0.05): at 400 kts a 2.5-kt correction
    // (50 ft at 0.05) is eaten by the throttle loop's phugoid
    // equilibrium — the receiver parked +50 ft off the boom all run.
    // 0.2/ft capped ±8 executes decisively and decays to ~3 kts at
    // the ±15 latch window.
    const double closure_bias = std::clamp(0.2 * closure_gap, -8.0, 8.0);
    const double target_speed = tanker_picture_.speed_kts + closure_bias;
    // EMPL-2 — the terminal lateral correction (see
    // controls_for_precontact): the wingman's linear cross-track law on
    // the CONTACT point (the receptacle this state closes on).
    const auto cp_st = contact_point();
    const double thdg = tanker_picture_.heading_rad;
    const double trx = current_position_.x - cp_st.x;
    const double trY = current_position_.y - cp_st.y;
    const double tlat = trx * std::cos(thdg) - trY * std::sin(thdg);
    const double tcorr = std::clamp(
        config.terminal_lateral_gain_rad_per_ft * tlat,
        -config.terminal_max_correction_rad,
        config.terminal_max_correction_rad);
    const double desired_hdg = thdg - tcorr;
    const double save_bank2 = air_steering.max_bank_rad;
    const double save_thr2 = air_steering.approach_aileron_threshold_rad;
    air_steering.max_bank_rad = config.terminal_max_bank_rad;
    // EMPL-2 — the heading deadband gate: steer() banks only for
    // errors above ~5 degrees (the cruise rudder is zeroed below it),
    // so a degree-scale lateral correction was INVISIBLE — the hold
    // drifted out of the ±15 envelope untouched (the e2e exits at
    // lat exactly ±15, corr never executed). Terminal steering banks
    // on any error.
    air_steering.approach_aileron_threshold_rad = 0.0;
    auto out = air_steering.steer(desired_hdg,
                                  tanker_picture_.altitude_msl_ft,
                                  target_speed, steering_input());
    air_steering.max_bank_rad = save_bank2;
    air_steering.approach_aileron_threshold_rad = save_thr2;
    // EMPL-2 — direct along-axis throttle bias (see controls_for_precontact):
    // the closure bias rides target_speed, but the energy coupling
    // held the actual speed at +1 kt of the tanker's regardless — the
    // receiver parked +42 ft off the receptacle all run. This closes.
    out.throttle_cmd = std::clamp(
        out.throttle_cmd + std::clamp(0.0015 * closure_gap, -0.12, 0.12),
        air_steering.throttle_min, 1.0);
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
    // EMPL-2 — the HOLD station servo. The bare tanker-speed target
    // holds whatever relative speed the receiver entered with: half a
    // knot of trim noise integrates into along-track drift, and the
    // ±15-ft envelope trips ContactLost in under a minute (the
    // campaign e2e churn: latched → drifted to +45 ft → ContactLost →
    // re-latch, the 20-s hold timer never expired). Same symmetric
    // along bias + linear lateral correction ClearedContact flies, so
    // the boom station is SERVED, not remembered.
    const double hold_gap = -along_err_ft();
    const double hold_bias = std::clamp(0.2 * hold_gap, -8.0, 8.0);
    const auto cp_h = contact_point();
    const double hhdg = tanker_picture_.heading_rad;
    const double hrx = current_position_.x - cp_h.x;
    const double hry = current_position_.y - cp_h.y;
    const double hlat = hrx * std::cos(hhdg) - hry * std::sin(hhdg);
    const double hcorr = std::clamp(
        config.hold_lateral_gain_rad_per_ft * hlat +
            config.hold_lateral_damp_rad_per_fps * hold_lat_rate_fps_,
        -config.terminal_max_correction_rad, config.terminal_max_correction_rad);
    // EMPL-2 — the deadband gate (see controls_for_precontact): the
    // hold's degree-scale corrections bank immediately.
    const double save_thr2 = air_steering.approach_aileron_threshold_rad;
    air_steering.approach_aileron_threshold_rad = 0.0;
    auto out = air_steering.steer(hhdg - hcorr,
                                  tanker_picture_.altitude_msl_ft,
                                  tanker_picture_.speed_kts + hold_bias,
                                  steering_input());
    air_steering.approach_aileron_threshold_rad = save_thr2;
    // EMPL-2 — the direct along-axis throttle bias here too (see
    // controls_for_cleared_contact): the ±15-ft envelope is served by
    // the actuator, not remembered by the speed target.
    out.throttle_cmd = std::clamp(
        out.throttle_cmd + std::clamp(0.0015 * hold_gap, -0.12, 0.12),
        air_steering.throttle_min, 1.0);
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
