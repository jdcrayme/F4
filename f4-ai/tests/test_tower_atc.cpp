// f4-ai/tests/test_tower_atc.cpp
//
// TowerATC (Tier 3) — the sequencing tower behind the stub's protocol.
//
// These are BUS-LEVEL tests: requests are published by hand, clearances
// are tapped by hand. The 6-DOF end-to-end coverage lives in
// f4-simulation/tests/test_digi_mission.cpp (tower-mode variant of the
// full mission); the module report hooks (DepartureReport on liftoff,
// RunwayVacatedReport on the rollout exit) are asserted in the module
// tests. Here we certify the TOWER's own contract:
//
//   - The runway is a resource: one occupant at a time.
//   - Requests while occupied are queued FIFO and deferred (silence).
//   - Release edges: DepartureReport, RunwayVacatedReport, GoAround
//     (releases a granted claim / removes a queued arrival), and the
//     occupancy timeout (vanished occupant).
//   - Late reports from timed-out aircraft are inert (reporter == occupant
//     guards).
//   - Per-airbase isolation; unknown ids fall back to the default field.
//   - Approach-data parity with the StubATC (the modules must not notice
//     the swap).
//   - The controller FSM leaves a greppable trace.

#include <gtest/gtest.h>

#include <f4/ai/atc/stub_atc.hpp>
#include <f4/ai/atc/tower_atc.hpp>
#include <f4/messaging/bus.hpp>

#include <string>
#include <vector>

using namespace f4::ai::atc;
namespace messaging = f4::messaging;
namespace geo = f4::geo;
namespace fsm = f4::fsm;

namespace {

// Kunsan-like airfield (mirrors test_takeoff_module's fixture so a config
// that works against the stub is exercised against the tower too).
AirfieldConfig make_kunsan_config() {
    AirfieldConfig config;
    config.active_runway_id = 36;
    config.active_runway_name = "Rwy 36L";
    config.runway_heading_rad = 0.0;  // north
    config.threshold_position = geo::WorldPosition(0.0, 5000.0, 0.0);
    config.threshold_altitude_ft = 0.0;
    config.pattern_altitude_ft = 2500.0;
    config.glide_slope_angle_rad = 3.0 * 3.14159265358979 / 180.0;
    config.decision_height_ft = 200.0;
    config.departure_altitude_ft = 2500.0;
    config.taxi_route = {
        geo::WorldPosition(0.0, 2500.0, 0.0),
        geo::WorldPosition(0.0, 5000.0, 0.0),
    };
    config.runway_end_position = geo::WorldPosition(0.0, 10000.0, 0.0);
    config.runway_width_ft = 150.0;
    config.runway_length_ft = 9000.0;
    return config;
}

// Tap recorder: logs "TypeName:id" for every ATC-protocol message.
class ProtocolTap {
public:
    explicit ProtocolTap(messaging::MessageBus& bus) {
        bus.subscribe<TaxiRequest>([this](const TaxiRequest& m) {
            push("TaxiRequest", m.aircraft_id);
        });
        bus.subscribe<TaxiClearance>([this](const TaxiClearance& m) {
            push("TaxiClearance", m.aircraft_id);
        });
        bus.subscribe<HoldShortRequest>([this](const HoldShortRequest& m) {
            push("HoldShortRequest", m.aircraft_id);
        });
        bus.subscribe<HoldShortClearance>([this](const HoldShortClearance& m) {
            push("HoldShortClearance", m.aircraft_id);
        });
        bus.subscribe<TakeoffRequest>([this](const TakeoffRequest& m) {
            push("TakeoffRequest", m.aircraft_id);
        });
        bus.subscribe<TakeoffClearance>([this](const TakeoffClearance& m) {
            push("TakeoffClearance", m.aircraft_id);
        });
        bus.subscribe<LandingRequest>([this](const LandingRequest& m) {
            push("LandingRequest", m.aircraft_id);
        });
        bus.subscribe<LandingClearance>([this](const LandingClearance& m) {
            push("LandingClearance", m.aircraft_id);
        });
        bus.subscribe<ApproachClearance>([this](const ApproachClearance& m) {
            push("ApproachClearance", m.aircraft_id);
        });
        bus.subscribe<ClearedToLand>([this](const ClearedToLand& m) {
            push("ClearedToLand", m.aircraft_id);
        });
        bus.subscribe<GoAroundMessage>([this](const GoAroundMessage& m) {
            push("GoAround:" + m.reason, m.aircraft_id);
        });
        bus.subscribe<DepartureReport>([this](const DepartureReport& m) {
            push("DepartureReport", m.aircraft_id);
        });
        bus.subscribe<RunwayVacatedReport>([this](const RunwayVacatedReport& m) {
            push("RunwayVacatedReport", m.aircraft_id);
        });
    }

    // IMPORTANT: the tap subscribes BEFORE the ATC in these tests, so it
    // sees requests too; clearances published by the ATC land in the same
    // log in causal order.
    std::vector<std::string> log;

    [[nodiscard]] long count(const std::string& name) const {
        long n = 0;
        for (const auto& entry : log) {
            if (entry.rfind(name, 0) == 0) ++n;
        }
        return n;
    }
    [[nodiscard]] long index_of(const std::string& entry) const {
        for (std::size_t i = 0; i < log.size(); ++i) {
            if (log[i] == entry) return static_cast<long>(i);
        }
        return -1;
    }

private:
    void push(const std::string& name, std::uint64_t id) {
        log.push_back(name + ":" + std::to_string(id));
    }
};

struct TowerFixture : public ::testing::Test {
    messaging::MessageBus bus;
    ProtocolTap tap{bus};          // subscribes FIRST (sees everything)
    TowerATC tower{bus, /*occupancy_timeout_s=*/300.0};

    TowerFixture() { tower.set_airfield(make_kunsan_config()); }

    void taxi(std::uint64_t id) {
        TaxiRequest req;
        req.aircraft_id = id;
        bus.publish(req);
    }
    void hold_short(std::uint64_t id) {
        HoldShortRequest req;
        req.aircraft_id = id;
        bus.publish(req);
    }
    void request_takeoff(std::uint64_t id) {
        TakeoffRequest req;
        req.aircraft_id = id;
        bus.publish(req);
    }
    void request_landing(std::uint64_t id, std::uint64_t base = 0) {
        LandingRequest req;
        req.aircraft_id = id;
        req.airbase_id = base;
        bus.publish(req);
    }
    void established(std::uint64_t id) {
        ApproachClearance req;
        req.aircraft_id = id;
        req.runway_id = 36;
        req.approach_type = "VISUAL";
        bus.publish(req);
    }
    void departed(std::uint64_t id, std::uint64_t base = 0) {
        DepartureReport report;
        report.aircraft_id = id;
        report.airbase_id = base;
        report.runway_id = 36;
        bus.publish(report);
    }
    void vacated(std::uint64_t id, std::uint64_t base = 0) {
        RunwayVacatedReport report;
        report.aircraft_id = id;
        report.airbase_id = base;
        report.runway_id = 36;
        bus.publish(report);
    }
    void go_around(std::uint64_t id, const char* reason) {
        GoAroundMessage msg;
        msg.aircraft_id = id;
        msg.runway_id = 36;
        msg.reason = reason;
        bus.publish(msg);
    }
};

} // anonymous namespace

// ============================================================================
// The runway is a resource
// ============================================================================

TEST_F(TowerFixture, SoloDepartureChainClearsImmediately) {
    taxi(1);
    hold_short(1);
    request_takeoff(1);

    EXPECT_EQ(tap.count("TaxiClearance:1"), 1);
    EXPECT_EQ(tap.count("HoldShortClearance:1"), 1);
    EXPECT_EQ(tap.count("TakeoffClearance:1"), 1);
    EXPECT_EQ(tower.runway_state(0), TowerATC::RunwayController::State::Departing);
    EXPECT_EQ(tower.occupant(0), 1u);
    EXPECT_EQ(tower.queue_depth(0), 0u);

    // The release edge returns the runway to Vacant.
    departed(1);
    EXPECT_EQ(tower.runway_state(0), TowerATC::RunwayController::State::Vacant);
    EXPECT_EQ(tower.occupant(0), 0u);
}

TEST_F(TowerFixture, SecondDepartureWaitsForRelease) {
    request_takeoff(1);            // A: granted immediately
    ASSERT_EQ(tower.occupant(0), 1u);

    request_takeoff(2);            // B: occupied -> queued, SILENCE
    EXPECT_EQ(tap.count("TakeoffClearance:2"), 0);
    EXPECT_EQ(tower.queue_depth(0), 1u);
    EXPECT_EQ(tower.occupant(0), 1u);

    departed(1);                   // A reports wheels-up -> B promoted
    EXPECT_EQ(tap.count("TakeoffClearance:2"), 1);
    EXPECT_EQ(tower.occupant(0), 2u);
    EXPECT_EQ(tower.queue_depth(0), 0u);

    departed(2);                   // B reports -> vacant again
    EXPECT_EQ(tower.runway_state(0), TowerATC::RunwayController::State::Vacant);
}

TEST_F(TowerFixture, ThreeDeparturesSequenceInFifoOrder) {
    request_takeoff(1);
    request_takeoff(2);
    request_takeoff(3);
    ASSERT_EQ(tower.queue_depth(0), 2u);

    departed(1);
    EXPECT_EQ(tap.count("TakeoffClearance:2"), 1);
    EXPECT_EQ(tap.count("TakeoffClearance:3"), 0)
        << "the tower must promote ONE waiter per release, not the queue";

    departed(2);
    EXPECT_EQ(tap.count("TakeoffClearance:3"), 1);
    EXPECT_EQ(tower.runway_state(0), TowerATC::RunwayController::State::Departing);
    EXPECT_EQ(tower.occupant(0), 3u);
}

// ============================================================================
// Arrivals sequence against departures
// ============================================================================

TEST_F(TowerFixture, ArrivalWaitsForDepartureRelease) {
    request_takeoff(1);            // A departs; runway Departing
    ASSERT_EQ(tower.occupant(0), 1u);

    request_landing(2);            // B gets approach DATA regardless...
    ASSERT_EQ(tap.count("LandingClearance:2"), 1);
    established(2);                // ...but the LAND clearance waits
    EXPECT_EQ(tap.count("ClearedToLand:2"), 0);
    EXPECT_EQ(tower.queue_depth(0), 1u);

    departed(1);                   // A wheels-up -> B cleared to land
    EXPECT_EQ(tap.count("ClearedToLand:2"), 1);
    EXPECT_EQ(tower.runway_state(0), TowerATC::RunwayController::State::Arriving);
    EXPECT_EQ(tower.occupant(0), 2u);

    vacated(2);                    // B exits the runway -> vacant
    EXPECT_EQ(tower.runway_state(0), TowerATC::RunwayController::State::Vacant);
}

TEST_F(TowerFixture, ArrivalGrantedOnVacantRunwayImmediately) {
    request_landing(2);
    established(2);
    EXPECT_EQ(tap.count("ClearedToLand:2"), 1);
    EXPECT_EQ(tower.runway_state(0), TowerATC::RunwayController::State::Arriving);
    EXPECT_EQ(tower.occupant(0), 2u);
}

// ============================================================================
// Go-arounds
// ============================================================================

TEST_F(TowerFixture, GoAroundReleasesGrantedClaimAndPromotesNext) {
    // B (arrival) holds the claim, C waits behind it.
    request_landing(2);
    established(2);
    ASSERT_EQ(tower.occupant(0), 2u);
    request_takeoff(3);
    ASSERT_EQ(tower.queue_depth(0), 1u);

    // B overflies the threshold and goes around -> claim released, C up.
    go_around(2, "threshold_overflown");
    EXPECT_EQ(tap.count("TakeoffClearance:3"), 1);
    EXPECT_EQ(tower.occupant(0), 3u);
    EXPECT_EQ(tower.queue_depth(0), 0u);
}

TEST_F(TowerFixture, GoAroundRemovesQueuedArrivalFromLine) {
    // A holds the runway; B queues as an arrival, then hits DH uncleared
    // and goes around ("not_cleared") — B must LEAVE the line, so A's
    // release promotes nobody.
    request_takeoff(1);
    ASSERT_EQ(tower.occupant(0), 1u);

    request_landing(2);
    established(2);
    ASSERT_EQ(tower.queue_depth(0), 1u);

    go_around(2, "not_cleared");
    EXPECT_EQ(tower.queue_depth(0), 0u);

    departed(1);
    EXPECT_EQ(tower.runway_state(0), TowerATC::RunwayController::State::Vacant);
    EXPECT_EQ(tap.count("ClearedToLand:2"), 0)
        << "a queued arrival that went around must not be cleared later";
}

// ============================================================================
// Occupancy timeout — the self-healing edge
// ============================================================================

TEST_F(TowerFixture, TimeoutReleasesVanishedOccupantAndPromotes) {
    request_takeoff(1);            // A granted, then vanishes (killed)
    ASSERT_EQ(tower.occupant(0), 1u);
    request_takeoff(2);            // B queued behind the ghost
    ASSERT_EQ(tower.queue_depth(0), 1u);

    // 299 s: still held. 301 s: released + B promoted.
    tower.tick(299.0);
    EXPECT_EQ(tower.occupant(0), 1u);
    tower.tick(2.0);
    EXPECT_EQ(tower.occupant(0), 2u);
    EXPECT_EQ(tap.count("TakeoffClearance:2"), 1);
}

TEST_F(TowerFixture, LateReportFromTimedOutOccupantIsInert) {
    request_takeoff(1);
    request_takeoff(2);
    tower.tick(301.0);             // A times out, B promoted
    ASSERT_EQ(tower.occupant(0), 2u);

    departed(1);                   // A's ghost report arrives late
    EXPECT_EQ(tower.occupant(0), 2u)
        << "a late report must not release the NEW occupant's claim";
    EXPECT_EQ(tower.runway_state(0),
              TowerATC::RunwayController::State::Departing);
}

TEST_F(TowerFixture, VacantRunwayIgnoresStrayReports) {
    departed(7);                   // nobody holds the runway
    vacated(7);
    EXPECT_EQ(tower.runway_state(0), TowerATC::RunwayController::State::Vacant);
    EXPECT_EQ(tap.count("TakeoffClearance:7"), 0);
    EXPECT_EQ(tap.count("ClearedToLand:7"), 0);
}

// ============================================================================
// Protocol parity with the stub
// ============================================================================

TEST(TowerParity, LandingClearanceMatchesStub) {
    messaging::MessageBus bus;
    StubATC stub(bus);
    const auto cfg = make_kunsan_config();
    stub.set_airfield(cfg);

    LandingClearance from_stub;
    bus.subscribe<LandingClearance>([&](const LandingClearance& m) {
        if (m.aircraft_id == 5) from_stub = m;
    });
    LandingRequest req;
    req.aircraft_id = 5;
    bus.publish(req);

    messaging::MessageBus bus2;
    TowerATC tower(bus2);
    tower.set_airfield(cfg);
    LandingClearance from_tower;
    bus2.subscribe<LandingClearance>([&](const LandingClearance& m) {
        if (m.aircraft_id == 5) from_tower = m;
    });
    LandingRequest req2;
    req2.aircraft_id = 5;
    bus2.publish(req2);

    EXPECT_EQ(from_tower.runway_id, from_stub.runway_id);
    EXPECT_EQ(from_tower.runway_name, from_stub.runway_name);
    EXPECT_DOUBLE_EQ(from_tower.runway_heading_rad,
                     from_stub.runway_heading_rad);
    EXPECT_DOUBLE_EQ(from_tower.threshold_position.x,
                     from_stub.threshold_position.x);
    EXPECT_DOUBLE_EQ(from_tower.threshold_position.y,
                     from_stub.threshold_position.y);
    EXPECT_DOUBLE_EQ(from_tower.threshold_altitude_ft,
                     from_stub.threshold_altitude_ft);
    EXPECT_DOUBLE_EQ(from_tower.glide_slope_angle_rad,
                     from_stub.glide_slope_angle_rad);
    EXPECT_DOUBLE_EQ(from_tower.pattern_altitude_ft,
                     from_stub.pattern_altitude_ft);
    EXPECT_DOUBLE_EQ(from_tower.decision_height_ft,
                     from_stub.decision_height_ft);
    EXPECT_DOUBLE_EQ(from_tower.runway_width_ft, from_stub.runway_width_ft);
    EXPECT_DOUBLE_EQ(from_tower.runway_length_ft, from_stub.runway_length_ft);
}

// ============================================================================
// Per-airbase isolation
// ============================================================================

TEST_F(TowerFixture, AirbasesSequenceIndependently) {
    AirfieldConfig other = make_kunsan_config();
    other.active_runway_id = 18;
    other.active_runway_name = "Rwy 18R";
    tower.set_airbase_airfield(77, other);

    // Base 77's runway is claimed...
    TakeoffRequest at77;
    at77.aircraft_id = 1;
    at77.airbase_id = 77;
    bus.publish(at77);
    ASSERT_EQ(tower.occupant(77), 1u);

    // ...the default field is untouched.
    request_takeoff(2);
    EXPECT_EQ(tap.count("TakeoffClearance:1"), 1);
    EXPECT_EQ(tap.count("TakeoffClearance:2"), 1)
        << "another base's occupancy must not hold a different field";
    EXPECT_EQ(tower.occupant(0), 2u);
    EXPECT_EQ(tower.occupant(77), 1u);
}

TEST_F(TowerFixture, UnknownAirbaseFallsBackToDefaultController) {
    tower.set_airbase_airfield(77, make_kunsan_config());

    request_takeoff(1);            // airbase_id 0 -> default controller
    TakeoffRequest unknown;
    unknown.aircraft_id = 2;
    unknown.airbase_id = 999;      // unregistered -> default controller too
    bus.publish(unknown);

    EXPECT_EQ(tap.count("TakeoffClearance:1"), 1);
    EXPECT_EQ(tap.count("TakeoffClearance:2"), 0);
    EXPECT_EQ(tower.queue_depth(0), 1u);
    EXPECT_EQ(tower.controller_count(), 1u)
        << "unknown ids must not spawn a fresh controller";
}

// ============================================================================
// Dedupe + re-request handling
// ============================================================================

TEST_F(TowerFixture, DuplicateRequestsDoNotDoubleQueue) {
    request_takeoff(1);
    ASSERT_EQ(tower.occupant(0), 1u);
    request_takeoff(2);
    request_takeoff(2);
    request_takeoff(2);
    EXPECT_EQ(tower.queue_depth(0), 1u);
}

TEST_F(TowerFixture, OccupantReRequestIsAnsweredNotQueued) {
    // The module's Wait -> HoldShort bounce re-publishes TakeoffRequest
    // after a promotion was latched; the occupant must get an (idempotent)
    // answer, never a self-queued ghost entry.
    request_takeoff(1);
    ASSERT_EQ(tower.occupant(0), 1u);
    request_takeoff(1);
    EXPECT_EQ(tower.queue_depth(0), 0u);
    EXPECT_EQ(tap.count("TakeoffClearance:1"), 2)
        << "the re-request is answered again (idempotent grant)";
    EXPECT_EQ(tower.occupant(0), 1u);
}

// ============================================================================
// Greppable FSM trace
// ============================================================================

TEST_F(TowerFixture, ControllerTraceRecordsTransitions) {
    request_takeoff(1);
    departed(1);
    request_landing(2);
    established(2);
    vacated(2);

    const auto* trace = tower.controller_trace(0);
    ASSERT_NE(trace, nullptr);
    const auto records = trace->records();
    ASSERT_FALSE(records.empty());

    std::vector<std::string> reasons;
    for (const auto& r : records) {
        if (r.fired) reasons.push_back(r.reason);
    }
    ASSERT_GE(reasons.size(), 4u);
    EXPECT_EQ(reasons[0], "departure_cleared");
    EXPECT_EQ(reasons[1], "departure_reported");
    EXPECT_EQ(reasons[2], "arrival_cleared");
    EXPECT_EQ(reasons[3], "runway_vacated");

    // Parseable text — the repository's observability discipline.
    const std::string text = trace->to_text(
        [](TowerATC::RunwayController::State s) {
            switch (s) {
                case TowerATC::RunwayController::State::Vacant:
                    return "Vacant";
                case TowerATC::RunwayController::State::Departing:
                    return "Departing";
                case TowerATC::RunwayController::State::Arriving:
                    return "Arriving";
            }
            return "?";
        },
        [](TowerATC::RunwayController::Event e) {
            switch (e) {
                case TowerATC::RunwayController::Event::GrantDeparture:
                    return "GrantDeparture";
                case TowerATC::RunwayController::Event::GrantArrival:
                    return "GrantArrival";
                case TowerATC::RunwayController::Event::Release:
                    return "Release";
                case TowerATC::RunwayController::Event::Abort:
                    return "Abort";
                case TowerATC::RunwayController::Event::Timeout:
                    return "Timeout";
            }
            return "?";
        });
    EXPECT_NE(text.find("reason=\"departure_cleared\""), std::string::npos);
}
