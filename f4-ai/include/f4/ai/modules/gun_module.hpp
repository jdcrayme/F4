// f4-ai/include/f4/ai/modules/gun_module.hpp
//
// GunModule — the guns fire control (AI_IMPLEMENTATION_PLAN.md §5 Steps
// 11-12, "Missile/Guns Engage"; FreeFalcon GunsEngageMode /
// guneval-style predictor).
//
// The LAST unflown weapon: heaters and AMRAAMs leave the rail through
// MissileModule instances (BVR + WVR rungs); the gun gets the same
// treatment here, composed by WVRModule — the module that owns the fight
// inside 3 NM, where a 6,000 ft gun envelope lives.
//
// ROLE 1 — THE PREDICTOR (pure math). A gun is not fired at the target,
// it is fired at the LEAD POINT: where the target will be after the
// bullet's time of flight.
//     t_bullet = range / (muzzle_velocity + closure)     [clamped 0..2.5 s]
//     lead     = predict(target) + target.velocity * t_bullet
// The firing solution error is the angle between the ownship's velocity
// (the boresight a real gun fires along) and the direction to the lead
// point. THAT error — not the pursuit error — gates the trigger: a
// heater-grade lead pursuit over-leads a gun solution by degrees at
// merge ranges, and degrees are misses at 3,000 ft.
//
// THE TRACK-FILE PREDICTION: the fusion refreshes at the skill interval
// (seconds), not per tick — the snapshot this module consumes is STALE
// on arrival, and at merge closure (1,000 ft/s) five stale seconds is
// the entire gun envelope. Every geometric quantity below starts from
// the dead-reckoned NOW position:
//     predict(t) = t.position + t.velocity * t.age_s
// — exactly what a radar track file's fire control does between scans.
// age 0 (hand-built test targets) means the snapshot IS now.
//
// ROLE 2 — THE TRIGGER DISCIPLINE (state machine). A burst is
// `burst_rounds` rounds (~0.4 s of trigger at 6,000 rpm), then
// `burst_cooldown_sec` before the next (ammo discipline + thermal
// rhythm; FreeFalcon fires short bursts, never a hose). Ammo is a
// rounds budget the host configures from the store's gun station — the
// module's doctrine stops pulling the trigger when the budget is spent,
// exactly like the missile shoot-shoot allotment.
//
// INTENT CONTRACT (engine-agnostic, same shape as release_pulse):
//   gun_pulse()      — true for EXACTLY ONE update() per burst (the
//                      rising edge; the host driver converts it into
//                      GunStream::start_burst + a store debit)
//   trigger_down()   — true through the burst's flight time (HUD/recorder
//                      cosmetics; the physical stream bursts on its own)
//
// ROE: hold_fire (module-level, set by the host from the scenario's
// hold_fire/guns_hold) — should_fire() answers no, so no pulse, no
// phantom burst, no budget burned on a trigger that never moved.
//
// Dependencies: f4-geo, f4-math, f4-ai (TargetInfo). C++20.

#pragma once

#include <cstdint>

#include <f4/geo/position.hpp>
#include <f4/math/vec3.hpp>

#include "f4/ai/target_info.hpp"

namespace f4::ai::modules {

class GunModule {
public:
    /// Fire-control parameters. Envelope + muzzle velocity + rpm are set
    /// by the host from the Gun-category weapon class card (M61A1); the
    /// trigger geometry is doctrine.
    struct Config {
        /// Employment envelope (NM). The EMPLOYMENT DOCTRINE, not the
        /// aerodynamic limit: the trigger works inside ~0.35 NM (2,100 ft)
        /// — the range where this FCS's ~1.5-2 deg boresight tracking
        /// error still projects inside the 40-ft hit footprint (at
        /// 5,000 ft the same tracking error is a 150-ft miss; the card's
        /// ~1 NM "effective range" is a hit-probability statement about
        /// the BULLET, not about this fire control's aim). The host
        /// derives it from the class card capped at this doctrine.
        double max_range_nm{0.35};
        double min_range_nm{0.08};
        /// The lead solution's bullet speed (ft/s) — M61A1 muzzle.
        double muzzle_velocity_fps{3400.0};
        /// HIT-QUALITY CONE: the trigger goes down only when the angular
        /// solution error projects INSIDE the lethal footprint at the
        /// CURRENT range:
        ///     error <= atan2(hit_radius_ft, range_now_ft)
        /// A fixed cone cannot work — at 5,000 ft the FCS's own tracking
        /// lag (~0.5 deg) is a 44-ft miss, while the same lag at 1,500 ft
        /// is a hit. The tolerance must scale with the range the way the
        /// miss geometry does. 40 ft: the weapons model's hit radius —
        /// the cone guarantees the mean impact is a hit; the damage
        /// falloff handles the edge.
        double hit_radius_ft{40.0};
        /// Solution-cone ceiling (rad): at very short range the scaled
        /// cone relaxes toward "any direction roughly at it" — cap it
        /// (~5 deg: a 300-ft snapshot is still a snapshot) so the trigger
        /// keeps meaning something.
        double max_solution_rad{0.0872664626};
        /// Burst discipline: rounds per trigger pull, and the pause (s)
        /// between bursts. 100 rounds = a full second of trigger at
        /// 6,000 rpm — the snapshot doctrine (per-round damage at the
        /// falloff edge is small; the burst's volume is the lethality).
        int    burst_rounds{100};
        double burst_cooldown_sec{1.0};
        /// The gun's cyclic rate (rpm) — burst flight-time bookkeeping
        /// (trigger_down duration), from the class card / M61A1 default.
        double rounds_per_minute{6000.0};
        /// Ammo budget (host sets from the store's gun station; 511 for
        /// a full M61A1 drum).
        int    rounds_budget{511};
        /// ROE: guns tight (scenario hold_fire / combat guns_hold).
        bool   hold_fire{false};
    };

    GunModule() = default;

    // =====================================================================
    // Role 1 — the predictor (pure functions; unit-test surface)
    // =====================================================================
    /// The dead-reckoned NOW position of the target (the track-file
    /// prediction — see the header's TRACK-FILE PREDICTION note).
    [[nodiscard]] static geo::WorldPosition predicted_position(
        const TargetInfo& t) noexcept;

    /// The range to the target NOW (ft, slant, from the predicted
    /// position — NOT the snapshot's stale range).
    [[nodiscard]] static double range_now_ft(
        const TargetInfo& t, const geo::WorldPosition& ownship_pos)
        noexcept;

    /// Bullet time of flight to the target (s): range-now / (muzzle + max
    /// closure, 0), clamped [0, 2.5]. Positive closure (a closing fight)
    /// shortens the flight; an opening target lengthens it, never past
    /// the clamp (a 2.5 s bullet is a prayer, not a solution).
    [[nodiscard]] double bullet_tof_s(
        const TargetInfo& t, const geo::WorldPosition& ownship_pos)
        const noexcept;

    /// The lead point: where the target will be when the bullet arrives
    /// (predicted now-position + the target's velocity over the flight
    /// time).
    [[nodiscard]] geo::WorldPosition lead_point(
        const TargetInfo& t, const geo::WorldPosition& ownship_pos)
        const noexcept;

    /// Horizontal bearing to the lead point from `ownship_pos` (rad, CW
    /// from north) — the steering target while working a guns solution
    /// (WVRModule swaps its missile-grade pursuit for this inside the
    /// gun envelope).
    [[nodiscard]] double lead_heading_rad(
        const TargetInfo& t, const geo::WorldPosition& ownship_pos)
        const noexcept;

    /// True when the target is inside the employment envelope NOW
    /// (predicted range; the stale snapshot range is NOT consulted).
    [[nodiscard]] bool in_envelope(const TargetInfo& t,
                                   const geo::WorldPosition& ownship_pos)
        const noexcept;

    /// The firing solution error (rad): angle between the ownship's
    /// 3D velocity (the boresight) and the direction to the lead point.
    /// `ownship_velocity` is the world-frame velocity (ft/s) — the
    /// caller's estimate (WVRModule derives it from consecutive ownship
    /// positions, the exact quantity the host sweep fires along).
    [[nodiscard]] double solution_error_rad(
        const TargetInfo& t, const geo::WorldPosition& ownship_pos,
        const math::Vec3<double>& ownship_velocity) const noexcept;

    /// The hit-quality tolerance (rad) at the target's CURRENT predicted
    /// range: atan2(hit_radius_ft, range), capped at max_solution_rad.
    /// The trigger only goes down when the solution error is inside THIS
    /// — the shot's miss geometry, not a doctrine constant.
    [[nodiscard]] double solution_tolerance_rad(
        const TargetInfo& t, const geo::WorldPosition& ownship_pos)
        const noexcept;

    /// True when a burst at `t` is legal NOW: hostile, visible, not a
    /// missile, inside the envelope, solution error within the cone,
    /// trigger state idle, budget not spent, ROE free. Pure except for
    /// the trigger/budget state.
    [[nodiscard]] bool should_fire(const TargetInfo& t,
                                   const geo::WorldPosition& ownship_pos,
                                   const math::Vec3<double>& ownship_velocity)
        const noexcept;

    // =====================================================================
    // Role 2 — the trigger state machine
    // =====================================================================
    /// Pull the trigger: starts a burst (bursting for the burst's flight
    /// time, then cooldown), debits the budget by the burst size
    /// (clipped to the remaining budget). No-op unless should_fire's
    /// state conditions held (the caller checks should_fire first; this
    /// still guards: a bursting/cooldown gun cannot double-fire).
    void note_burst();

    /// Burn the burst/cooldown timers by dt. The host ticks this every
    /// update (fight or not) — the gun's cycle is physical time.
    void tick(double dt) noexcept;

    /// Reset the engagement bookkeeping (new target / WVR reset /
    /// fight over): the pulse drops, and a mid-burst reset transitions
    /// to cooldown (the rounds in the stream keep flying; the gun still
    /// owes its cycle — the cooldown is the shooter's, not the target's,
    /// same rule as the missile rail cadence). The budget survives
    /// (ammo is ammo).
    void reset_engagement() noexcept;

    // --- State ------------------------------------------------------------
    /// One-tick pulse per burst — the host driver's start_burst edge.
    [[nodiscard]] bool gun_pulse() const noexcept { return pulse_; }
    /// True through the burst's flight time (HUD / recorder cosmetics).
    [[nodiscard]] bool trigger_down() const noexcept {
        return state_ == TriggerState::Bursting;
    }
    /// Seconds until the trigger is available again (0 = ready).
    [[nodiscard]] double cooldown_remaining_sec() const noexcept {
        return state_ == TriggerState::Cooldown ? timer_ : 0.0;
    }
    /// Rounds left in the budget.
    [[nodiscard]] int rounds_remaining() const noexcept {
        return rounds_;
    }
    /// Bursts fired this engagement (doctrine counter).
    [[nodiscard]] int bursts_fired() const noexcept { return bursts_; }

    // --- Configuration ------------------------------------------------------
    [[nodiscard]] const Config& config() const noexcept { return cfg_; }
    [[nodiscard]] Config&       config()       noexcept { return cfg_; }
    void set_config(const Config& c) noexcept { cfg_ = c; }
    /// Envelope shortcut (host: the M61A1 class card's range).
    void set_envelope_nm(double min_nm, double max_nm) noexcept {
        cfg_.min_range_nm = min_nm;
        cfg_.max_range_nm = max_nm;
    }
    /// Load the gun (host: the store's gun-station round count at
    /// configure time). Sets the budget AND the live round counter —
    /// an ammo count is physical truth, not doctrine to be re-derived.
    void set_rounds_budget(int rounds) noexcept {
        cfg_.rounds_budget = rounds;
        rounds_ = rounds;
    }

private:
    enum class TriggerState { Idle, Bursting, Cooldown };

    Config cfg_{};
    TriggerState state_{TriggerState::Idle};
    double timer_{0.0};    // remaining bursting or cooldown time
    int rounds_{511};      // budget countdown (default: full M61A1 drum)
    int bursts_{0};
    bool pulse_{false};
};

} // namespace f4::ai::modules
