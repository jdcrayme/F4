// f4-avionics/include/f4/avionics/fcr_page.hpp
//
// FCR page — the fire-control radar page as an avionics model: the
// RWS/TWS/VS mode state machine, the lock state with the reference's
// lock rules, and the renderer-facing page snapshot.
//
// AVIONICS-2 (Docs/AVIONICS_PLAN.md §4). The plan's charter boundary
// drawn once more: this is pure avionics LOGIC. The page consumes the
// radar's published state (f4-sensors' RadarSimComponent — the SAME
// component the AI's RadarBackedDetectionPolicy reads, never the AI)
// and produces a struct of numbers/flags/enums a cockpit renderer
// draws. It never draws and never reads devices. The golden test for
// the whole subsystem: two different renderers consuming one snapshot
// must produce the same instrument behavior.
//
// THE LOCK HAND-OFF: the page's designate()/break_lock() are the ONLY
// writers here, and they drive the radar through its own primitives —
// RadarSimComponent::command_track()/command_search(). The radar then
// owns everything downstream: the track refresh (Track mode scans the
// locked target regardless of the search volume), the lock-drop rule
// ("the lock cannot outlive its track"), and the victim's RWR Lock
// strobe. The fire-control path the AI's MissileModule::should_fire
// gates on (a live radar track -> detected_by_radar) therefore honors
// the page lock WITHOUT any new sensor concept: the page is the
// player-side driver of the same primitives the AI's intents drive.
// The AI does NOT consume this library (the charter's parallel-rule).
//
// DETERMINISM: the snapshot is a pure recompute from the radar's track
// store and scan volume — no RNG, no wall clock, symbols in ascending
// entity_id order. Same radar state + same ownship state => byte-
// identical snapshot, so two renderers agree frame for frame.
//
// v1 semantics (documented, pinned in test_fcr_page.cpp):
//   - Mode machine: Off --PowerOn--> Rws; Rws <-> Tws <-> Vs (and
//     Rws <-> Vs) via the Select events; PowerOff from any live state.
//     Mode switches KEEP the lock (the lock is orthogonal to the
//     search display); PowerOff clears it.
//   - Designate: refused with the page Off (an unpowered page
//     designates nothing); refused for a target the radar does not
//     hold a live (non-Dropped) track for — the same refusal
//     command_track itself enforces, checked through the radar's own
//     answer so the page lock and the radar lock can never diverge.
//   - Break lock: clears the page lock and parks the radar back into
//     Search (command_search).
//   - Track death: when the page observes its locked track Dropped,
//     the page lock drops with it and the radar is parked (a defensive
//     command_search — the radar's own decay rule usually lands first;
//     this makes the page self-consistent even when driven without the
//     radar's decay pass). update() writes NOTHING else, ever.
//   - VS page: velocity-only semantics — the snapshot flags
//     velocity_only and lists only CLOSING contacts. No new radar
//     physics: the mode gates the DISPLAY, not the scan.
//
// C++20. Header-only.

#pragma once

#include <f4/geo/f4_geo.hpp>
#include <f4/sensors/radar_component.hpp>

#include <f4/fsm/f4_fsm.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

namespace f4::avionics {

// ============================================================================
// Page mode states & events
// ============================================================================

enum class FcrState {
    Off,  ///< powered down: no symbols, no lock
    Rws,  ///< Range While Search: volume sweep, all live tracks shown
    Tws,  ///< Track While Scan: the same picture, the designated track
          ///  highlighted (the page's soft-lock display)
    Vs,   ///< Velocity Search: closing contacts only, range suspect
};

enum class FcrEvent {
    PowerOn,   ///< Off -> Rws
    PowerOff,  ///< {Rws,Tws,Vs} -> Off (the lock clears with the page)
    SelectRws,
    SelectTws,
    SelectVs,
};

// ============================================================================
// The mode machine
// ============================================================================

using FcrSm = fsm::StateMachine<FcrState, FcrEvent>;

/// The page's mode machine as a PURE transition table — no captures, no
/// side effects (the lock lives beside the machine, not in it; PowerOff's
/// lock clear is a page-side effect, the InsUnit discipline). Every
/// transition and every refusal that matters is pinned in
/// test_fcr_page.cpp.
[[nodiscard]] inline FcrSm make_fcr_machine() {
    return FcrSm::Builder()
        .initial(FcrState::Off)
        .state(FcrState::Off, "Off")
        .state(FcrState::Rws, "Rws")
        .state(FcrState::Tws, "Tws")
        .state(FcrState::Vs, "Vs")
        .event_name(FcrEvent::PowerOn, "PowerOn")
        .event_name(FcrEvent::PowerOff, "PowerOff")
        .event_name(FcrEvent::SelectRws, "SelectRws")
        .event_name(FcrEvent::SelectTws, "SelectTws")
        .event_name(FcrEvent::SelectVs, "SelectVs")
        .on(FcrState::Off, FcrState::Rws, FcrEvent::PowerOn,
            nullptr, nullptr, "page power on")
        .on(FcrState::Rws, FcrState::Tws, FcrEvent::SelectTws,
            nullptr, nullptr, "RWS -> TWS")
        .on(FcrState::Rws, FcrState::Vs, FcrEvent::SelectVs,
            nullptr, nullptr, "RWS -> VS")
        .on(FcrState::Rws, FcrState::Off, FcrEvent::PowerOff,
            nullptr, nullptr, "power off from RWS")
        .on(FcrState::Tws, FcrState::Rws, FcrEvent::SelectRws,
            nullptr, nullptr, "TWS -> RWS")
        .on(FcrState::Tws, FcrState::Vs, FcrEvent::SelectVs,
            nullptr, nullptr, "TWS -> VS")
        .on(FcrState::Tws, FcrState::Off, FcrEvent::PowerOff,
            nullptr, nullptr, "power off from TWS")
        .on(FcrState::Vs, FcrState::Rws, FcrEvent::SelectRws,
            nullptr, nullptr, "VS -> RWS")
        .on(FcrState::Vs, FcrState::Tws, FcrEvent::SelectTws,
            nullptr, nullptr, "VS -> TWS")
        .on(FcrState::Vs, FcrState::Off, FcrEvent::PowerOff,
            nullptr, nullptr, "power off from VS")
        .build();
}

// ============================================================================
// The renderer-facing snapshot
// ============================================================================

/// One symbol on the FCR page: the per-contact row a renderer draws.
/// Positions are PAGE-relative (offset from the antenna), not world
/// coordinates — the same geometry a real FCR format draws.
struct FcrSymbol {
    std::uint64_t entity_id{0};
    /// Bearing offset from the antenna's scan center, wrapped [-pi, pi].
    /// 0 = straight up the page (the antenna boresight).
    double azimuth_rad{0.0};
    /// Elevation from the ownship's horizon, positive up.
    double elevation_rad{0.0};
    /// Slant range.
    double range_nm{0.0};
    /// Range rate along the line of sight, ft/s. POSITIVE = closing
    /// (range decreasing) — the radar-page convention.
    double closure_fps{0.0};
    /// IFF: the track's team differs from the radar's own_team.
    bool hostile{false};
    /// The track-store's "good track" set (Established or Coasting —
    /// a Tentative track draws hollow).
    bool established{false};
    /// The page's designated (locked) contact.
    bool designated{false};
    /// Convenience for velocity-only pages: closure_fps > 0.
    bool closing{false};

    auto operator<=>(const FcrSymbol&) const = default;
};

/// Everything the FCR page draws, no pointers. Two renderers consuming
/// one snapshot must produce the same instrument behavior (the plan's
/// §3 golden rule).
struct FcrPageSnapshot {
    FcrState mode{FcrState::Off};
    bool locked{false};
    std::uint64_t locked_target_id{0};
    /// VS page: velocity-only semantics — the symbol list carries only
    /// closing contacts and range displays are the renderer's problem
    /// (the flag, not a filtered range field, keeps the model honest
    /// about what the radar actually measures).
    bool velocity_only{false};
    /// The scan frame the page draws around the symbols (the live
    /// ScanVolume's shape, mirrored for the renderer).
    double az_half_width_rad{0.0};
    double el_min_rad{0.0};
    double el_max_rad{0.0};
    double range_scale_nm{0.0};
    /// Live (non-Dropped) tracks, ascending entity_id — deterministic.
    std::vector<FcrSymbol> symbols;

    auto operator<=>(const FcrPageSnapshot&) const = default;
};

// ============================================================================
// The page model
// ============================================================================

/// The FCR page unit: the mode machine + the lock state + the snapshot
/// recompute. The InsUnit pattern — the machine is the pure table, the
/// unit owns the side effects, and the ONLY writers are the pilot-input
/// methods (designate/break_lock) plus the one defensive write in
/// update() (the track-death parking).
class FcrPageModel final {
public:
    FcrPageModel() = default;

    // --- Mode controls (return true when the machine moved) -------------

    [[nodiscard]] bool power_on() {
        return send(FcrEvent::PowerOn);
    }
    [[nodiscard]] bool power_off() {
        const bool moved = send(FcrEvent::PowerOff);
        if (moved) lock_id_ = 0;   // the lock clears with the page
        return moved;
    }
    [[nodiscard]] bool select_rws() { return send(FcrEvent::SelectRws); }
    [[nodiscard]] bool select_tws() { return send(FcrEvent::SelectTws); }
    [[nodiscard]] bool select_vs()  { return send(FcrEvent::SelectVs); }

    // --- The lock (the reference's rules; the f4-sensors hand-off) ------

    /// Designate a target. Refused with the page Off; refused for a
    /// target the radar does not hold a live track for (the radar's own
    /// command_track answers the same rule — the page takes THAT answer,
    /// so the page lock and the radar lock cannot diverge). On success
    /// the radar is in Track mode (STT) and the page is locked.
    [[nodiscard]] bool designate(std::uint64_t target_id,
                                 sensors::RadarSimComponent& radar) {
        if (sm_.current() == FcrState::Off) return false;
        if (!radar.command_track(target_id)) return false;
        lock_id_ = target_id;
        return true;
    }

    /// Break the lock: the page unlocks and the radar parks back into
    /// Search. A no-op with no lock (nothing to un-write).
    void break_lock(sensors::RadarSimComponent& radar) {
        if (lock_id_ == 0) return;
        lock_id_ = 0;
        radar.command_search();
    }

    // --- Per-frame recompute --------------------------------------------

    /// Recompute the page snapshot from the radar's published state.
    /// Takes the ownship position/velocity the page geometry is drawn
    /// against (the BRA/LOS math is page geometry, not radar state).
    ///
    /// WRITE RULE: the page drives the radar ONLY through
    /// designate/break_lock — except here, once, when the locked track
    /// is observed Dropped: the page lock drops with the track (the
    /// reference's "lock cannot outlive its track") and the radar is
    /// parked defensively (its own decay rule normally lands first;
    /// this keeps the page self-consistent when driven without the
    /// radar's decay pass). No other write, ever.
    [[nodiscard]] FcrPageSnapshot
    update(sensors::RadarSimComponent& radar,
           const f4::geo::WorldPosition& own_position,
           const f4::math::Vec3<double>& own_velocity) {
        // The track-death mirror: a Dropped locked track drops the page
        // lock (and parks the radar — idempotent when the radar's own
        // decay already did it).
        if (lock_id_ != 0) {
            const auto* tf = radar.tracks().find(lock_id_);
            if (tf == nullptr || tf->state == sensors::TrackState::Dropped) {
                lock_id_ = 0;
                radar.command_search();
            }
        }

        FcrPageSnapshot snap;
        snap.mode = sm_.current();
        snap.locked = lock_id_ != 0;
        snap.locked_target_id = lock_id_;
        snap.velocity_only = (snap.mode == FcrState::Vs);
        snap.az_half_width_rad = radar.scan.azimuth_half_width_rad;
        snap.el_min_rad = radar.scan.elevation_min_rad;
        snap.el_max_rad = radar.scan.elevation_max_rad;
        snap.range_scale_nm = radar.scan.range_scale_nm;

        for (const auto* tf : radar.tracks().live()) {
            FcrSymbol s = symbol_for(*tf, own_position, own_velocity,
                                     radar.scan.azimuth_center_rad);
            s.designated = snap.locked && tf->entity_id == lock_id_;
            if (snap.velocity_only && !s.closing) continue;
            snap.symbols.push_back(s);
        }
        return snap;
    }

    // --- Queries ----------------------------------------------------------

    [[nodiscard]] FcrState mode() const noexcept { return sm_.current(); }
    [[nodiscard]] bool locked() const noexcept { return lock_id_ != 0; }
    [[nodiscard]] std::uint64_t locked_target_id() const noexcept {
        return lock_id_;
    }
    [[nodiscard]] const FcrSm& machine() const noexcept { return sm_; }

private:
    [[nodiscard]] bool send(FcrEvent e) {
        const auto before = sm_.current();
        sm_.process(e);
        return sm_.current() != before;
    }

    [[nodiscard]] static FcrSymbol
    symbol_for(const sensors::TrackFile& tf,
               const f4::geo::WorldPosition& own_position,
               const f4::math::Vec3<double>& own_velocity,
               double antenna_center_rad) {
        const f4::geo::BRA bra = f4::geo::to_bra(own_position, tf.position);
        const double dx = tf.position.x - own_position.x;
        const double dy = tf.position.y - own_position.y;
        const double dz = tf.position.z - own_position.z;
        const double horiz = std::sqrt(dx * dx + dy * dy);

        FcrSymbol s;
        s.entity_id = tf.entity_id;
        // The page draws offset from the antenna: bearing minus the scan
        // center, wrapped to [-pi, pi] (the shortest way round the dial).
        double rel = bra.bearing_rad - antenna_center_rad;
        while (rel > M_PI) rel -= 2.0 * M_PI;
        while (rel < -M_PI) rel += 2.0 * M_PI;
        s.azimuth_rad = rel;
        s.elevation_rad =
            (bra.range_ft > 0.0) ? std::atan2(dz, horiz) : 0.0;
        s.range_nm = bra.range_nm();

        // Closure along the line of sight: positive = range decreasing
        // (the radar-page convention). Unit LOS in 3D, relative velocity
        // dotted on; a degenerate (zero-range) LOS reads 0.
        const double slant = bra.range_ft;
        if (slant > 0.0) {
            const double ux = dx / slant, uy = dy / slant,
                         uz = dz / slant;
            const double relvx = own_velocity.x - tf.velocity.x;
            const double relvy = own_velocity.y - tf.velocity.y;
            const double relvz = own_velocity.z - tf.velocity.z;
            s.closure_fps =
                relvx * ux + relvy * uy + relvz * uz;
        }
        s.hostile = tf.hostile_by_iff;
        s.established = tf.state == sensors::TrackState::Established ||
                        tf.state == sensors::TrackState::Coasting;
        s.closing = s.closure_fps > 0.0;
        return s;
    }

    FcrSm sm_{make_fcr_machine()};
    std::uint64_t lock_id_{0};
};

} // namespace f4::avionics
