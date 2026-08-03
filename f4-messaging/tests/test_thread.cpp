// test_thread.cpp — MessageBus cross-thread semantics.
//
// These tests verify the §9.3 model: each subsystem owns a bus, producers
// publish_deferred() from their own thread, the owning thread flushes at
// the start of its tick. We spawn real std::threads to validate the
// mutex/condition_variable plumbing rather than mocking it out.

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <f4/messaging/f4_messaging.hpp>
#include <thread>
#include <vector>

using namespace f4::messaging;

namespace {
struct WorkMsg {
    uint64_t id = 0;
};
struct DoneMsg {
    uint64_t worker_id = 0;
};
} // namespace

TEST(MessageBusCrossThread, MultipleProducersCanPublishDeferredConcurrently) {
    MessageBus bus;
    std::atomic<int> hits{0};
    bus.subscribe<WorkMsg>([&](const WorkMsg&) { hits.fetch_add(1); });

    constexpr int N_THREADS = 4;
    constexpr int MSGS_PER_THREAD = 250;
    std::vector<std::thread> producers;
    producers.reserve(N_THREADS);
    for (int t = 0; t < N_THREADS; ++t) {
        producers.emplace_back([&bus, t]() {
            for (int i = 0; i < MSGS_PER_THREAD; ++i) {
                bus.publish_deferred(WorkMsg{static_cast<uint64_t>(t * 1000 + i)});
            }
        });
    }
    for (auto& th : producers) th.join();

    EXPECT_EQ(bus.pending_count(), N_THREADS * MSGS_PER_THREAD);
    bus.flush_pending();
    EXPECT_EQ(hits.load(), N_THREADS * MSGS_PER_THREAD);
    EXPECT_EQ(bus.pending_count(), 0u);
}

TEST(MessageBusCrossThread, SendToFromProducerThreadDeliversOnConsumerFlush) {
    // Mirrors the §9.3 pattern: campaign thread send_to(sim_bus, msg);
    // sim thread calls sim_bus.flush_pending() at top of its tick.
    MessageBus sim_bus;
    std::vector<uint64_t> received;
    sim_bus.subscribe<WorkMsg>([&](const WorkMsg& m) {
        received.push_back(m.id);
    });

    std::thread campaign_thread([&sim_bus]() {
        send_to(sim_bus, WorkMsg{11u});
        send_to(sim_bus, WorkMsg{22u});
        send_to(sim_bus, WorkMsg{33u});
    });
    campaign_thread.join();

    EXPECT_EQ(sim_bus.pending_count(), 3u);
    sim_bus.flush_pending();
    ASSERT_EQ(received.size(), 3u);
    EXPECT_EQ(received[0], 11u);
    EXPECT_EQ(received[1], 22u);
    EXPECT_EQ(received[2], 33u);
}

TEST(MessageBusCrossThread, ConsumerCanPublishWhileProducerEnqueues) {
    // Bidirectional: sim publishes DamageMsg to its own bus (same-thread)
    // while another thread is enqueuing WorkMsg on the same bus.
    MessageBus bus;
    std::atomic<int> work_hits{0};
    std::atomic<int> damage_hits{0};
    bus.subscribe<WorkMsg>([&](const WorkMsg&) { work_hits.fetch_add(1); });
    bus.subscribe<DoneMsg>([&](const DoneMsg&) { damage_hits.fetch_add(1); });

    std::thread producer([&bus]() {
        for (int i = 0; i < 100; ++i) {
            bus.publish_deferred(WorkMsg{static_cast<uint64_t>(i)});
        }
    });

    // Consumer simultaneously publishes a few immediate messages.
    for (int i = 0; i < 10; ++i) {
        bus.publish(DoneMsg{static_cast<uint64_t>(i)});
    }

    producer.join();
    bus.flush_pending();

    EXPECT_EQ(work_hits.load(), 100);
    EXPECT_EQ(damage_hits.load(), 10);
}

TEST(MessageBusCrossThread, FlushIsReentrantSafeFromHandler) {
    // A handler that itself calls flush_pending() must not deadlock or
    // corrupt state. The inner flush sees an empty pending list (we
    // swapped before invoking handlers) and returns cleanly.
    MessageBus bus;
    std::atomic<int> hits{0};
    bus.subscribe<WorkMsg>([&](const WorkMsg&) {
        hits.fetch_add(1);
        // Recursive flush from inside a handler — must not deadlock.
        bus.flush_pending();
    });

    bus.publish_deferred(WorkMsg{1});
    bus.flush_pending();
    EXPECT_EQ(hits.load(), 1);
}

TEST(MessageBusCrossThread, StressTestManyMessagesAcrossTwoBuses) {
    // Two buses, two threads, two message types — mirrors sim+campaign
    // topology. Verify all messages land.
    MessageBus sim_bus, camp_bus;
    std::atomic<int> sim_hits{0}, camp_hits{0};
    sim_bus.subscribe<WorkMsg>([&](const WorkMsg&) { sim_hits.fetch_add(1); });
    camp_bus.subscribe<DoneMsg>([&](const DoneMsg&) { camp_hits.fetch_add(1); });

    constexpr int N = 1000;
    std::thread sim_producer([&sim_bus]() {
        for (int i = 0; i < N; ++i) send_to(sim_bus, WorkMsg{static_cast<uint64_t>(i)});
    });
    std::thread camp_producer([&camp_bus]() {
        for (int i = 0; i < N; ++i) send_to(camp_bus, DoneMsg{static_cast<uint64_t>(i)});
    });

    // Consumer polls until both producers are done (joined below).
    sim_producer.join();
    camp_producer.join();

    sim_bus.flush_pending();
    camp_bus.flush_pending();

    EXPECT_EQ(sim_hits.load(), N);
    EXPECT_EQ(camp_hits.load(), N);
}
