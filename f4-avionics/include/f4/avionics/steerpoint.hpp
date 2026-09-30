// f4-avionics/include/f4/avionics/steerpoint.hpp
//
// Steerpoint navigation: the flight plan (an ordered steerpoint list with a
// selected point) and the navigation reads over it — bearing/range/altitude
// solutions and the HSI steering cue.
//
// AVIONICS-1 (Docs/AVIONICS_PLAN.md §4). All reads go through a supplied
// position/heading — pass InsUnit::solution() to read THROUGH the drifting
// INS (the avionics view), or the raw truth to compare against it (the
// tests do both: the drift-zero case must compare equal member-for-member).
//
// The route source is the caller's: the campaign bridge already stamps a
// flight plan on spawned flights, and scenario hosts hand their waypoint
// chains — this library owns the list and the geometry, not the source.
//
// C++20. Header-only.

#pragma once

#include "f4/avionics/ins.hpp"
#include "f4/geo/f4_geo.hpp"

#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace f4::avionics {

// ============================================================================
// Steerpoint + the flight plan
// ============================================================================

/// One steerpoint: a name and a theater-frame position (f4::geo's
/// WorldPosition — ENU feet, z = altitude MSL).
struct Steerpoint {
    std::string name{};
    geo::WorldPosition position{};
};

/// The flight plan: an ordered steerpoint list with a selected (active)
/// point. The sequence has no opinion about overflight — advancing past
/// the last point is the host's decision (next() reports the wall).
class SteerpointSequence final {
public:
    void push_back(Steerpoint steer) { points_.push_back(std::move(steer)); }

    [[nodiscard]] std::size_t size() const noexcept { return points_.size(); }
    [[nodiscard]] bool empty() const noexcept { return points_.empty(); }
    [[nodiscard]] const std::vector<Steerpoint>& points() const noexcept { return points_; }
    [[nodiscard]] std::size_t index() const noexcept { return selected_; }

    /// The active steerpoint. Throws std::out_of_range on an empty
    /// sequence — a caller with no flight plan has no steerpoint to read.
    [[nodiscard]] const Steerpoint& current() const {
        if (points_.empty()) {
            throw std::out_of_range("avionics: steerpoint sequence is empty");
        }
        return points_[selected_];
    }

    /// Select the active steerpoint by index. Throws std::out_of_range
    /// for an out-of-range index.
    void select(std::size_t i) {
        if (i >= points_.size()) {
            throw std::out_of_range("avionics: steerpoint index out of range");
        }
        selected_ = i;
    }

    /// Advance to the next steerpoint. Returns false when already at the
    /// end (the caller keeps steering the last point).
    bool next() {
        if (selected_ + 1 >= points_.size()) return false;
        ++selected_;
        return true;
    }

private:
    std::vector<Steerpoint> points_;
    std::size_t selected_{0};
};

// ============================================================================
// Navigation reads
// ============================================================================

/// Bearing/range/altitude to a steerpoint from a position — f4::geo's BRA
/// over the supplied (believed) position. Bearing is true bearing
/// [0, 2*pi) CW from north; range is SLANT range (the steering read is
/// 3-D: a steerpoint 2,000 ft below is 2,000 ft closer than the planar
/// map says); altitude is the steerpoint's MSL.
[[nodiscard]] inline geo::BRA to_steer(const geo::WorldPosition& believed_position,
                                       const Steerpoint& steer) noexcept {
    return geo::to_bra(believed_position, steer.position);
}

/// The active steerpoint's solution read through an INS unit. Throws
/// std::out_of_range if the sequence is empty (through current()).
[[nodiscard]] inline geo::BRA current_steer(const InsUnit& ins,
                                            const SteerpointSequence& sequence) {
    return to_steer(ins.solution().position, sequence.current());
}

/// HSI steering cue: the signed bearing error from the believed heading to
/// the steer bearing, wrapped to [-pi, +pi], and the shortest-way flag.
/// (+ = the steer bearing lies clockwise of the heading — turn right.)
/// Exactly dead astern (|error| = pi) pins RIGHT — arbitrary, but pinned
/// so the two renderers that will consume this cue agree.
struct SteeringCue {
    double bearing_error_rad{};   ///< [-pi, +pi]; + = steer bearing is clockwise of heading
    bool turn_right{};            ///< shortest-way turn direction

    auto operator<=>(const SteeringCue&) const = default;
};

/// The steering cue from a believed heading and a steer bearing (both
/// radians; the heading may be any real — it is wrapped).
[[nodiscard]] inline SteeringCue steer_cue(double believed_heading_rad,
                                           double steer_bearing_rad) noexcept {
    double err = detail::wrap_pi_pi(steer_bearing_rad - believed_heading_rad);
    if (err <= -f4::geo::PI) err = f4::geo::PI;   // dead astern pins right (documented pin)
    return SteeringCue{err, err > 0.0};
}

/// The active steerpoint's cue read through an INS unit: the bearing error
/// from the BELIEVED heading to the steer bearing measured from the
/// BELIEVED position. Throws std::out_of_range if the sequence is empty.
[[nodiscard]] inline SteeringCue current_steer_cue(const InsUnit& ins,
                                                   const SteerpointSequence& sequence) {
    const geo::BRA br = current_steer(ins, sequence);
    return steer_cue(ins.solution().heading_rad, br.bearing_rad);
}

}  // namespace f4::avionics
