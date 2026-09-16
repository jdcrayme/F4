// f4-sensors/include/f4/sensors/visual_component.hpp
//
// VisualComponent — the Mk1 eyeball / seeker TV camera as an ECS
// behavioral component, driven by the SimData SENSDATA/VISUAL card
// (f4-data's VisualSensorData: azimuth/elevation limits + gain).
//
// The model IS the original's documented signal law (f4-data's
// sensor_data.hpp records it from visual.cpp's threshold comment):
//
//   signal = gain * visual_sig / range_ft^2   —  detect iff >= 1.0
//
//     gain       — the card (generic.vss ships 3.7e9 = (10 NM in ft)^2,
//                  i.e. a 10-NM nominal range against the reference
//                  airframe); mav/tpod ship (100 NM)^2.
//     visual_sig — the target's VIS signature ratio (SIGDATA VIS grid,
//                  aspect-driven size factor; flat 1.0 in the shipped
//                  generic data). A 4x-bigger transport reads at 2x the
//                  range — inverse-square, no fourth root: this is the
//                  one-way eyeball, not a radar echo.
//     range_ft^2 — the straight-line range squared.
//
// Deterministic: NO RNG anywhere (the original threshold is a
// comparison, not a probability). Same world state => same contacts,
// which is what the deterministic-sim discipline wants from the sensor
// a human eye stands behind.
//
// Field of view: the card's az/el limits gate the scan exactly like the
// IRST's gimbal gates (generic: 181/91 = "everything"). Own heading
// from the velocity vector; stationary -> azimuth gate off.
//
// Ground clutter: NOT rejected — you can SEE parked aircraft (and the
// mav/tpod cards exist to look at the ground). Ground targets read at
// full range; the visual model has no ground factor in the card.
//
// ECS framing: priority 45 — the radar's pass.
//
// Publishing: NONE this tranche (queryable state; see the IRST header).

#pragma once

#include <cstdint>
#include <string>

#include <f4/entities/entity.hpp>
#include <f4/messaging/bus.hpp>
#include <f4/sensors/passive_track.hpp>

namespace f4::sensors {

/// Pure signal model — the original law: gain * sig / range_ft^2.
[[nodiscard]] double visual_signal(double gain, double visual_sig,
                                   double range_ft) noexcept;

/// Range (feet) at which a target of the given visual signature crosses
/// the 1.0 threshold: sqrt(gain * sig).
[[nodiscard]] double visual_detection_range_ft(double gain,
                                               double visual_sig) noexcept;

struct VisualParameters {
    double az_limit_deg = 181.0;   // SENSDATA/VISUAL generic.vss
    double el_limit_deg = 91.0;
    double gain = 3.7e9;

    /// Range pre-gate multiplier: candidates beyond this multiple of the
    /// threshold range can never detect (sqrt gain ceiling).
    double scan_cutoff_multiplier = 2.0;
};

class VisualComponent : public entities::BehavioralComponent<VisualComponent> {
public:
    int priority() const noexcept override { return 45; }

    void on_attached(entities::EntityHandle& self) override { owner_ = self; }
    void update(double dt, messaging::MessageBus& bus) override;

    // --- Simulation time (host-stamped; mirrors RadarSimComponent) --------
    static void set_sim_time(double t) { sim_time_s() = t; }
    static double sim_time() { return sim_time_s(); }

    // --- Configuration (public fields: data cards, live-tunable) ----------
    VisualParameters params{};
    double scan_interval_s = 1.0;
    std::string own_team = "blue";
    double contact_hold_s = 3.0;

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
    double scan_timer_ = 0.0;
    std::uint64_t scans_ = 0;
};

} // namespace f4::sensors
