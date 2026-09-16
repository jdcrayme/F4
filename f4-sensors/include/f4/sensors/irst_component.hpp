// f4-sensors/include/f4/sensors/irst_component.hpp
//
// IrstComponent — the infrared search-and-track sensor as an ECS
// behavioral component, driven by the SimData SENSDATA/IRST card
// (f4-data's IrstSeekerData: azimuth/elevation gimbal limits, nominal
// range, ground factor, flare chance).
//
// The card in the tree doubles as the IR SEEKER table (aim9l/sa7/...
// rows with flare_chance > 0 and ground_factor ~ 0.001): those rows
// feed the countermeasure model's flare-defeat rolls (see f4-weapons'
// countermeasures.hpp). THIS component is the AIRFRAME IRST consumer —
// the sensor that watches the air picture passively. The "generic" row
// (az 120 / el 60 / 10 NM / ground factor 1.0) is the airframe card
// the defaults below mirror.
//
// Detection model (documented placeholder, data-shaped — the FreeFalcon
// runtime read precompiled .ICD tables, not these text cards, so the
// exact original signal math is unrecoverable from the tree):
//
//   R_det = nominal_range_nm * sqrt(ir_sig / reference_ir) * ground
//
//     ir_sig    — the target's IR signature ratio from its
//                 SignatureComponent (SIGDATA IR band; hot-rear lobe).
//                 SQRT because IR flux is one-way (power ~ 1/r^2, so
//                 range ~ sqrt(intensity)) — the radar's fourth root is
//                 the two-way echo, IR is the emitter itself.
//     reference_ir — 1.0: the signature the nominal range was authored
//                 against. A flat-1.0 band reads as the nominal range.
//     ground    — the card's ground_factor when the target sits on the
//                 ground (the airframe card's 1.0 = no penalty; the
//                 seeker cards' 0.001 = an IR missile cannot track a
//                 cold parked airframe).
//
//   P(detect) — the radar's 0.75-knee ramp shape: 1.0 inside 75% of
//   R_det, linear to 0 at R_det, 0 beyond. The scan rolls a seeded
//   mt19937 exactly like RadarSimComponent (same seed + same scenario
//   => same detection sequence).
//
// Field of view: the gimbal limits gate the scan — |bearing-to-target
// off own heading| <= az_limit_deg AND |elevation| <= el_limit_deg.
// Own heading comes from the velocity vector; below the stationary
// speed the azimuth gate is OFF (the parked/holding sensor sweeps —
// RWR's omni contract; elevation still gates).
//
// Ground clutter: an airframe IRST tracks the AIR picture — stationary
// ground clutter is skipped (the C6 finding radar already encoded),
// unless track_ground_clutter is set (an air-to-ground sensor card's
// use; the ground factor then scales the range). Moving ground
// entities (taxiing aircraft) are never clutter and always scan.
//
// ECS framing: priority 45 — the radar's pass, so scans read this
// tick's flight-model positions and the AI pass reads fresh contacts.
//
// Publishing: NONE this tranche. Contacts are queryable state (the
// fusion pass that consumes them polls components; the radar's
// transition messages exist for debrief/RWR audio consumers IRST does
// not have yet). Documented as the fusion tranche's decision.

#pragma once

#include <cstdint>
#include <random>
#include <string>

#include <f4/entities/entity.hpp>
#include <f4/messaging/bus.hpp>
#include <f4/sensors/passive_track.hpp>

namespace f4::sensors {

/// Pure detection model — detection range (NM) against a target whose
/// IR signature ratio is `ir_sig` (1.0 = the reference the card's
/// nominal range was authored against). `ground_target` applies the
/// card's ground factor.
[[nodiscard]] double irst_detection_range_nm(
    const struct IrstParameters& params, double ir_sig,
    bool ground_target) noexcept;

/// Pure detection model — the radar's 0.75-knee ramp: 1.0 inside 75%
/// of r_det_nm, linear to 0 at r_det_nm, 0 beyond.
[[nodiscard]] double irst_detection_probability(double range_nm,
                                                double r_det_nm) noexcept;

struct IrstParameters {
    double az_limit_deg = 120.0;    // SENSDATA/IRST generic.irs
    double el_limit_deg = 60.0;
    double nominal_range_nm = 10.0;
    double ground_factor = 1.0;
    double reference_ir_value = 1.0;

    /// Range pre-gate multiplier (the radar's 8x shape): candidates
    /// beyond this multiple of the nominal range can never detect.
    double scan_cutoff_multiplier = 8.0;

    bool track_ground_clutter = false;   // airframe IRST: air picture
};

class IrstComponent : public entities::BehavioralComponent<IrstComponent> {
public:
    int priority() const noexcept override { return 45; }

    void on_attached(entities::EntityHandle& self) override { owner_ = self; }
    void update(double dt, messaging::MessageBus& bus) override;

    // --- Simulation time (host-stamped; mirrors RadarSimComponent) --------
    static void set_sim_time(double t) { sim_time_s() = t; }
    static double sim_time() { return sim_time_s(); }

    // --- Configuration (public fields: data cards, live-tunable) ----------
    IrstParameters params{};
    double scan_interval_s = 1.0;
    std::uint32_t rng_seed = 0x49525354ull;   // "IRST"
    std::string own_team = "blue";
    double contact_hold_s = 3.0;   // drop a contact unseen this long

    // --- State accessors ---------------------------------------------------
    [[nodiscard]] const PassiveTrackStore& contacts() const noexcept {
        return contacts_;
    }
    [[nodiscard]] const PassiveContact* find(
        std::uint64_t id) const noexcept {
        return contacts_.find(id);
    }
    [[nodiscard]] std::uint64_t scans_performed() const noexcept {
        return scans_;
    }

private:
    void perform_scan(messaging::MessageBus& bus);

    entities::EntityHandle owner_{};
    static double& sim_time_s() {
        static double t = 0.0;
        return t;
    }

    PassiveTrackStore contacts_{};
    std::mt19937 rng_{};
    bool initialized_ = false;
    double scan_timer_ = 0.0;
    std::uint64_t scans_ = 0;
};

} // namespace f4::sensors
