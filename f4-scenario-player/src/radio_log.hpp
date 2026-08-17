// f4-scenario-player/src/radio_log.hpp
//
// PRIVATE HEADER — internal to the f4-scenario-player library.
//
// RadioLog — observes ATC traffic on the simulation's message bus and
// keeps a scrolling transcript for the overlay. This is the visible proof
// of the clearance sequence (taxi request -> clearance -> takeoff request
// -> clearance -> approach request -> clearance -> cleared to land).
//
// Each entry is a short pilot/tower phrase in the style of a comms log,
// timestamped with the sim time at receipt.

#pragma once

#include <f4/simulation/simulation.hpp>
#include <f4/messaging/bus.hpp>

#include <array>
#include <cstdint>
#include <string>

namespace f4::scenario_player {

class RadioLog {
public:
    struct Entry {
        double time_s{0.0};
        bool from_atc{false};     // false = pilot transmission
        std::string text;
    };

    static constexpr std::size_t CAPACITY = 64;   // ring buffer size

    /// Subscribe to the ATC message types on the simulation's bus. Call
    /// AFTER Simulation::initialize(). The time source is the simulation
    /// clock at message receipt.
    void attach(f4::simulation::Simulation& sim);

    /// Most recent entries, oldest first (up to CAPACITY).
    [[nodiscard]] const std::array<Entry, CAPACITY>& entries() const noexcept {
        return entries_;
    }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t begin_index() const noexcept { return begin_; }

    /// Convenience: entry i of the logical sequence (0 = oldest kept).
    [[nodiscard]] const Entry* at(std::size_t i) const noexcept {
        if (i >= size_) return nullptr;
        return &entries_[(begin_ + i) % CAPACITY];
    }

private:
    void push(bool from_atc, std::string text, double t);

    std::array<Entry, CAPACITY> entries_{};
    std::size_t size_{0};
    std::size_t begin_{0};
    std::size_t next_{0};
    f4::simulation::Simulation* sim_{nullptr};   // time source
};

} // namespace f4::scenario_player
