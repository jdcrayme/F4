// f4-scenario-player/src/radio_log.cpp
//
// RadioLog implementation — subscribes to the ATC protocol messages and
// formats a human-readable transcript.

#include "radio_log.hpp"

#include <f4/ai/atc/messages.hpp>
#include <f4/ai/atc/stub_atc.hpp>

#include <utility>

namespace f4::scenario_player {

using namespace f4::ai::atc;

void RadioLog::push(bool from_atc, std::string text, double t) {
    entries_[next_] = Entry{t, from_atc, std::move(text)};
    next_ = (next_ + 1) % CAPACITY;
    if (size_ < CAPACITY) {
        ++size_;
    } else {
        begin_ = (begin_ + 1) % CAPACITY;   // overwrite the oldest
    }
}

void RadioLog::attach(f4::simulation::Simulation& sim) {
    sim_ = &sim;
    auto& bus = sim.bus();
    const auto t = [this] { return sim_ ? sim_->sim_time_s() : 0.0; };

    bus.subscribe<TaxiRequest>([this, t](const TaxiRequest&) {
        push(false, "Tower, request taxi for departure.", t());
    });
    bus.subscribe<TaxiClearance>([this, t](const TaxiClearance& m) {
        push(true, "Taxi to " + m.runway_name + " via the taxiway, hold short.", t());
    });
    bus.subscribe<HoldShortRequest>([this, t](const HoldShortRequest&) {
        push(false, "Holding short.", t());
    });
    bus.subscribe<HoldShortClearance>([this, t](const HoldShortClearance&) {
        push(true, "Hold short acknowledged.", t());
    });
    bus.subscribe<TakeoffRequest>([this, t](const TakeoffRequest&) {
        push(false, "Holding short, ready for departure.", t());
    });
    bus.subscribe<TakeoffClearance>([this, t](const TakeoffClearance&) {
        push(true, "Cleared for takeoff.", t());
    });
    bus.subscribe<LineUpAndWait>([this, t](const LineUpAndWait&) {
        push(true, "Line up and wait.", t());
    });
    bus.subscribe<LandingRequest>([this, t](const LandingRequest&) {
        push(false, "Inbound, request approach.", t());
    });
    bus.subscribe<LandingClearance>([this, t](const LandingClearance& m) {
        push(true, "Cleared approach " + m.runway_name + ", report established.", t());
    });
    bus.subscribe<ApproachClearance>([this, t](const ApproachClearance&) {
        push(false, "Established on final, request landing.", t());
    });
    bus.subscribe<ClearedToLand>([this, t](const ClearedToLand& m) {
        push(true, "Cleared to land runway " + std::to_string(m.runway_id) + ".", t());
    });
    bus.subscribe<GoAroundMessage>([this, t](const GoAroundMessage& m) {
        push(false, "Going around (" + m.reason + ").", t());
    });
    bus.subscribe<TaxiOffClearance>([this, t](const TaxiOffClearance&) {
        push(true, "Vacate via the taxiway, taxi to parking.", t());
    });
}

} // namespace f4::scenario_player
