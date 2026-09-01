// f4-ai/src/wingman_module.cpp
//
// WingmanModule implementation — see modules/wingman_module.hpp for design
// notes.
//
// FreeFalcon reference: winglogic.cpp (wing state decisions), wingactions.cpp
// (AiFollowLead — the formation steering this module modernizes), formdata.cpp
// (the formation offset table below is its 2-ship subset).

#include "f4/ai/modules/wingman_module.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <tuple>

namespace f4::ai::modules {

// ============================================================================
// Construction + FSM
// ============================================================================

WingmanModule::WingmanModule()
    : sm_(build_sm())
{
    // Formation tune: the nav cascade's comfort limits are right for the
    // following half (30 deg bank, MIL rail) — a wingman that out-banks
    // its lead is not forming, it's chasing. Rejoin keeps the same bank
    // limit; the speed law, not the bank, closes a blowout.
    air_steering_.max_bank_rad = 0.52;
}

fsm::StateMachine<WingState, WingEvent>
WingmanModule::build_sm() {
    return typename fsm::StateMachine<WingState, WingEvent>::Builder()
        .initial(WingState::None)
        .state(WingState::None,      "None")
        .state(WingState::Following, "Following")
        .state(WingState::Rejoining, "Rejoining")
        .event_name(WingEvent::LeadAcquired, "LeadAcquired")
        .event_name(WingEvent::StationLost,  "StationLost")
        .event_name(WingEvent::InStation,    "InStation")
        .event_name(WingEvent::LeadLost,     "LeadLost")
        .on(WingState::None, WingState::Following, WingEvent::LeadAcquired,
            nullptr, nullptr, "first_valid_lead_picture")
        .on(WingState::Following, WingState::Rejoining,
            WingEvent::StationLost,
            nullptr, nullptr, "blew_past_rejoin_ring")
        .on(WingState::Rejoining, WingState::Following,
            WingEvent::InStation,
            nullptr, nullptr, "converged_to_station_band")
        .on(WingState::Following, WingState::None, WingEvent::LeadLost,
            nullptr, nullptr, "lead_picture_invalid")
        .on(WingState::Rejoining, WingState::None, WingEvent::LeadLost,
            nullptr, nullptr, "lead_picture_invalid")
        .build();
}

// ============================================================================
// Public API
// ============================================================================

void WingmanModule::set_lead_picture(const LeadPicture& p) {
    const bool was_live = picture_.valid;
    picture_ = p;

    if (!p.valid) {
        // Dead / landed / unresolvable lead: drop off the ladder. The
        // wingman becomes a single-ship (brain flies the mission module).
        if (was_live) {
            sm_.process(WingEvent::LeadLost);
        }
        return;
    }

    if (sm_.current() == WingState::None) {
        // First picture. Acquire straight into the state the geometry
        // says — the hysteresis transitions below keep it honest after.
        const double dist = [this, &p]() {
            const auto st = formation_position();
            const double dx = current_position_.x - st.x;
            const double dy = current_position_.y - st.y;
            return std::sqrt(dx * dx + dy * dy);
        }();
        // current_position_ may still be zero (picture pushed before the
        // first update) — treat that as "start at station" (the host
        // spawns wingmen near their slots); the first update's StationLost
        // reclassifies a wingman that spawned far.
        const bool far = (current_position_.x != 0.0 ||
                          current_position_.y != 0.0) &&
                         dist > cfg_.rejoin_range_ft;
        sm_.process(WingEvent::LeadAcquired);
        if (far) {
            // The FSM has no conditional edge; emulate it by immediately
            // blowing the station (the guard re-runs next update anyway).
            sm_.process(WingEvent::StationLost);
        }
    }
}

void WingmanModule::command_formation(FormationType form) {
    if (form == form_) return;
    form_ = form;
    // Crossing to a new slot: if the new station is far, the next update's
    // StationLost fires Rejoining on its own. Nothing else to reset —
    // AirSteering state is position-independent.
}

void WingmanModule::reset() {
    sm_ = build_sm();
    picture_ = LeadPicture{};
    prev_along_ft_ = 0.0;
    along_rate_ftps_ = 0.0;
    prev_lead_range_ft_ = 0.0;
    lead_range_rate_ftps_ = 0.0;
    along_valid_ = false;
    desired_heading_rad_ = 0.0;
}

AIControlOutput WingmanModule::update(double dt,
                                      const flight::IAircraftState* state) {
    AIControlOutput out{};

    if (state == nullptr) return out;
    if (!picture_.valid) {
        desired_heading_rad_ = 0.0;
        return out;  // no lead — empty output, brain flies the mission
    }

    // Cache ownship state for the helpers (same pattern as BVRModule).
    current_position_ = geo::WorldPosition(
        state->position_east_ft(), state->position_north_ft(),
        state->altitude_msl_ft());

    // --- Hysteresis: Following <-> Rejoining ---------------------------
    const geo::WorldPosition station = formation_position();
    const double dx = current_position_.x - station.x;
    const double dy = current_position_.y - station.y;
    const double station_dist = std::sqrt(dx * dx + dy * dy);

    // Range to the LEAD itself (rotation-free — unlike the station
    // distance, which swings with the lead's heading during a turn).
    const double ldx = current_position_.x - picture_.position.x;
    const double ldy = current_position_.y - picture_.position.y;
    const double lead_range = std::sqrt(ldx * ldx + ldy * ldy);

    if (sm_.current() == WingState::Following &&
        station_dist > cfg_.rejoin_range_ft) {
        sm_.process(WingEvent::StationLost);
    } else if (sm_.current() == WingState::Rejoining &&
               lead_range < cfg_.rejoin_capture_ft) {
        // CAPTURE on lead range, not station distance: the slot sweeps
        // ~3200 ft around a turning lead, and a station-distance
        // threshold kept missing the flyby (the 2v2 rejoin never
        // captured). Inside the ring, Following's gentle law closes
        // the residual — a join, not an orbit.
        sm_.process(WingEvent::InStation);
    }

    // --- Rate estimates (the speed laws' damping terms) --------------------
    // Along-track (station frame; Following) + range-to-lead (Rejoining).
    const double along_now = station_error_ft(*state);
    if (along_valid_) {
        const double alpha = dt / (dt + 0.75);   // ~0.75 s smoothing
        const double along_raw = (along_now - prev_along_ft_) / dt;
        along_rate_ftps_ += alpha * (along_raw - along_rate_ftps_);
        const double range_raw = (lead_range - prev_lead_range_ft_) / dt;
        lead_range_rate_ftps_ += alpha * (range_raw - lead_range_rate_ftps_);
    }
    prev_along_ft_ = along_now;
    prev_lead_range_ft_ = lead_range;
    along_valid_ = true;

    // --- Lateral: heading law --------------------------------------------
    // Two regimes:
    //   FAR  (station_dist > 3 * tolerance): the rejoin cone — LEAD
    //        pursuit of the slot (the intercept; see below).
    //   NEAR (inside): formation keeping — hold the LEAD's heading plus a
    //        proportional correction toward the slot's lateral offset.
    //        Pure pursuit at zero error would orbit the co-moving slot;
    //        a fixed lead-heading would freeze a lateral offset in place
    //        forever; the blend (lead heading + clamped correction) does
    //        neither — it forms.
    if (station_dist > 3.0 * cfg_.formation_tolerance_ft) {
        const double sx = station.x - current_position_.x;   // east
        const double sy = station.y - current_position_.y;   // north
        // LEAD PURSUIT, not pure: at equal speeds a tail chase of the
        // slot diverges against a crossing lead — the bearing sweeps
        // faster than the bank can turn, and pointing straight AT the
        // slot can OPEN the range (measured in the 2v2 post-fight
        // rejoin: 5.6 kft out, 42 deg off the lead's track, +347 ft/s
        // while aimed directly at the slot, stuck in Rejoining for the
        // rest of the run). Advance the aim along the lead's velocity
        // by the time-to-go: a real rejoin flies the formation's flight
        // path and merges from behind. A stationary lead (velocity ~ 0)
        // degrades to pure pursuit, which is correct for a parked lead.
        const double v_fps = std::sqrt(
            picture_.velocity.x * picture_.velocity.x +
            picture_.velocity.y * picture_.velocity.y);
        const double t_go = station_dist / std::max(300.0, 0.7 * v_fps);
        const double ax = sx + picture_.velocity.x * t_go;
        const double ay = sy + picture_.velocity.y * t_go;
        desired_heading_rad_ = (ax == 0.0 && ay == 0.0)
            ? state->heading_rad()
            : std::atan2(ax, ay);
    } else {
        const double lat = lateral_error_ft(*state);
        const double corr = std::clamp(
            cfg_.lateral_gain_rad_per_ft * lat,
            -cfg_.max_lateral_correction_rad,
            cfg_.max_lateral_correction_rad);
        // Right of the slot (+ lat) -> turn left (negative correction).
        desired_heading_rad_ = picture_.heading_rad - corr;
    }

    // --- Longitudinal: speed law (signed along-track error) ---------------
    const double speed_kts = desired_speed_kts(*state);

    // --- Vertical + bank through the shared cascade ----------------------
    out = air_steering_.steer(desired_heading_rad_, station.z,
                              speed_kts, steering_input(*state));
    return out;
}

// ============================================================================
// Queries
// ============================================================================

std::tuple<double, double, double>
WingmanModule::formation_offsets() const noexcept {
    // FreeFalcon formdata.cpp, 2-ship subset. (lateral, longitudinal,
    // vertical) in multiples of cfg_ spacing; lateral + = right side.
    switch (form_) {
        case FormationType::FightingWing:
            // ~40 deg aft of the beam, 2.5 kft: the BVR default.
            return {1.0, 1.0, 0.0};
        case FormationType::EchelonRight:
            return {1.5, 0.4, 0.0};
        case FormationType::EchelonLeft:
            return {-1.5, 0.4, 0.0};
        case FormationType::Trail:
            return {0.0, 1.6, 0.0};
        case FormationType::LineAbreast:
            return {2.5, 0.0, 0.0};
    }
    return {1.0, 1.0, 0.0};
}

geo::WorldPosition WingmanModule::formation_position() const {
    if (!picture_.valid) return {};

    const auto [lat_m, long_m, vert_m] = formation_offsets();
    const double lateral = lat_m * cfg_.lateral_spacing_ft;
    const double longitudinal = long_m * cfg_.longitudinal_spacing_ft;
    const double vertical = vert_m * cfg_.lateral_spacing_ft +
                            cfg_.vertical_offset_ft;

    // Lead's heading frame: forward = heading vector, right = +90 deg.
    const double fwd_x = std::sin(picture_.heading_rad);   // east
    const double fwd_y = std::cos(picture_.heading_rad);   // north
    const double right_x = std::cos(picture_.heading_rad);
    const double right_y = -std::sin(picture_.heading_rad);

    geo::WorldPosition st;
    // AFT = -forward * longitudinal; RIGHT = right * lateral.
    st.x = picture_.position.x - fwd_x * longitudinal + right_x * lateral;
    st.y = picture_.position.y - fwd_y * longitudinal + right_y * lateral;
    st.z = picture_.alt_msl_ft - vertical;  // vert + = below the lead
    return st;
}

double WingmanModule::station_error_ft(
    const flight::IAircraftState& own) const {
    if (!picture_.valid) return 0.0;

    const geo::WorldPosition station = formation_position();
    const double ox = own.position_east_ft() - station.x;
    const double oy = own.position_north_ft() - station.y;

    // Project on the lead's forward direction: + = ahead of the slot.
    const double fwd_x = std::sin(picture_.heading_rad);
    const double fwd_y = std::cos(picture_.heading_rad);
    return ox * fwd_x + oy * fwd_y;
}

double WingmanModule::lateral_error_ft(
    const flight::IAircraftState& own) const {
    if (!picture_.valid) return 0.0;

    const geo::WorldPosition station = formation_position();
    const double ox = own.position_east_ft() - station.x;
    const double oy = own.position_north_ft() - station.y;

    // Project on the lead's RIGHT direction: + = right of the slot.
    const double right_x = std::cos(picture_.heading_rad);
    const double right_y = -std::sin(picture_.heading_rad);
    return ox * right_x + oy * right_y;
}

double WingmanModule::desired_speed_kts(
    const flight::IAircraftState& own) const {
    if (!picture_.valid) return own.vcas_kts();

    if (sm_.current() == WingState::Rejoining) {
        // The BLOW-PAST GUARD: a wingman AHEAD of the slot (positive
        // along-track error) is not chasing anything, and the chase law
        // below is poison there — its lead+10 floor and its
        // opening-rate term accelerate the wingman away from the
        // formation (a faster wingman that overruns the slot can never
        // slow back to it: the range opens, the P term saturates, and
        // the state machine sits in Rejoining while the jet flies off —
        // measured twice in the 2v2 post-fight rejoin). Brake instead:
        // P on the excess along-track error, no rate term (the
        // station-frame rate is frame rotation during a lead turn — the
        // phugoid the inertial chase law exists to avoid). The LEAD
        // then closes the range; the capture ring fires on lead range;
        // Following's PD finishes the join.
        const double along = station_error_ft(own);
        if (along > cfg_.formation_tolerance_ft) {
            const double brake = std::clamp(
                cfg_.follow_speed_gain *
                    (along - cfg_.formation_tolerance_ft),
                0.0, 0.5 * cfg_.max_lead_speed_delta_kts);
            return std::clamp(picture_.vcas_kts - brake,
                              cfg_.min_speed_kts, cfg_.max_speed_kts);
        }

        // REJOIN law — on RANGE TO THE LEAD (rotation-free): during the
        // lead's post-fight turn the station frame rotates under the
        // wingman and the along-track error's rate is frame rotation,
        // not closure — the station-frame law phugoided 36 kft in the
        // 2v2 E2E before this. Lead range + its rate are inertial truth.
        const double ldx = own.position_east_ft() - picture_.position.x;
        const double ldy = own.position_north_ft() - picture_.position.y;
        const double lead_range = std::sqrt(ldx * ldx + ldy * ldy);

        // Nominal range = the station's own distance from the lead.
        const auto [lat_m, long_m, vert_m] = formation_offsets();
        const double nominal = std::sqrt(
            (lat_m * cfg_.lateral_spacing_ft) *
                (lat_m * cfg_.lateral_spacing_ft) +
            (long_m * cfg_.longitudinal_spacing_ft) *
                (long_m * cfg_.longitudinal_spacing_ft));

        // P on the excess range, D on the closure (range rate is
        // NEGATIVE while closing: + damp*rate SUBTRACTS — the braking).
        double kts = picture_.vcas_kts
            + std::clamp(0.05 * (lead_range - nominal),
                         30.0, cfg_.max_lead_speed_delta_kts)
            + cfg_.follow_damp_kt_per_fps * lead_range_rate_ftps_;
        kts = std::clamp(kts,
                         picture_.vcas_kts + 10.0,
                         picture_.vcas_kts + cfg_.max_lead_speed_delta_kts);
        return std::clamp(kts, cfg_.min_speed_kts, cfg_.max_speed_kts);
    }

    // FOLLOWING law — the station-frame PD: match the lead, correct the
    // along-track error (+ = ahead of the slot -> slow; - = behind ->
    // speed up), brake on the closure rate so the join does not
    // overshoot through the slot and phugoid around it.
    const double along = station_error_ft(own);
    double kts = picture_.vcas_kts
        - cfg_.follow_speed_gain * along
        - cfg_.follow_damp_kt_per_fps * along_rate_ftps_;
    kts = std::clamp(kts,
                     picture_.vcas_kts - cfg_.max_lead_speed_delta_kts,
                     picture_.vcas_kts + cfg_.max_lead_speed_delta_kts);
    return std::clamp(kts, cfg_.min_speed_kts, cfg_.max_speed_kts);
}

AirSteering::Input WingmanModule::steering_input(
    const flight::IAircraftState& s) const noexcept {
    AirSteering::Input in;
    in.position = current_position_;
    in.heading_rad = s.heading_rad();
    in.pitch_rad = s.pitch_angle_rad();
    in.roll_rad = s.roll_angle_rad();
    in.roll_rate_radps = s.roll_rate_radps();
    in.pitch_rate_radps = s.pitch_rate_radps();
    in.vs_fpm = s.vertical_speed_fpm();
    in.vcas_kts = s.vcas_kts();
    in.alt_msl_ft = s.altitude_msl_ft();
    return in;
}

// ============================================================================
// Names
// ============================================================================

std::string WingmanModule::state_name() const {
    auto name = sm_.name_of(sm_.current());
    return name.empty() ? std::to_string(static_cast<int>(sm_.current()))
                        : std::string(name);
}

std::string WingmanModule::formation_name() const {
    switch (form_) {
        case FormationType::FightingWing: return "FightingWing";
        case FormationType::EchelonRight: return "EchelonRight";
        case FormationType::EchelonLeft:  return "EchelonLeft";
        case FormationType::Trail:        return "Trail";
        case FormationType::LineAbreast:  return "LineAbreast";
    }
    return "Unknown";
}

} // namespace f4::ai::modules
