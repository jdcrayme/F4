// test_bus.cpp — MessageBus: subscribe / publish / unsubscribe / deferred.

#include <gtest/gtest.h>
#include <atomic>
#include <f4/messaging/f4_messaging.hpp>
#include <string>
#include <vector>

using namespace f4::messaging;

// ============================================================================
// Plain-data message types for tests. Mirrors §9.2's "messages are plain
// structs — no inheritance required" principle.
// ============================================================================
namespace {
struct DamageMsg {
    uint64_t target_id = 0;
    uint64_t source_id = 0;
    int damage_type = 0;
    float strength = 0.0f;
};
struct MissileFireMsg {
    uint64_t shooter_id = 0;
    uint64_t missile_id = 0;
    uint64_t target_id = 0;
    int weapon_type = 0;
};
struct WingmanCmdMsg {
    uint64_t sender_id = 0;
    uint64_t receiver_id = 0;
    int command_type = 0;
};
struct EmptyMsg {};
} // namespace

// ============================================================================
// Subscribe / publish, same-thread
// ============================================================================
TEST(MessageBus, PublishDeliversToSubscriber) {
    MessageBus bus;
    int received = 0;
    bus.subscribe<DamageMsg>([&](const DamageMsg& m) {
        ++received;
        EXPECT_EQ(m.target_id, 7u);
    });
    bus.publish(DamageMsg{7u, 1u, 0, 0.5f});
    EXPECT_EQ(received, 1);
}

TEST(MessageBus, PublishWithNoSubscribersIsNoop) {
    MessageBus bus;
    // Should not throw, should not crash.
    bus.publish(DamageMsg{1, 2, 3, 4.0f});
    bus.publish(MissileFireMsg{});
    SUCCEED();
}

TEST(MessageBus, MultipleHandlersFireInRegistrationOrder) {
    MessageBus bus;
    std::vector<int> order;
    bus.subscribe<DamageMsg>([&](const DamageMsg&) { order.push_back(1); });
    bus.subscribe<DamageMsg>([&](const DamageMsg&) { order.push_back(2); });
    bus.subscribe<DamageMsg>([&](const DamageMsg&) { order.push_back(3); });
    bus.publish(DamageMsg{});
    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], 1);
    EXPECT_EQ(order[1], 2);
    EXPECT_EQ(order[2], 3);
}

TEST(MessageBus, DifferentMessageTypesAreIsolated) {
    MessageBus bus;
    int damage_count = 0;
    int missile_count = 0;
    bus.subscribe<DamageMsg>([&](const DamageMsg&) { ++damage_count; });
    bus.subscribe<MissileFireMsg>([&](const MissileFireMsg&) { ++missile_count; });
    bus.publish(DamageMsg{});
    bus.publish(DamageMsg{});
    bus.publish(MissileFireMsg{});
    EXPECT_EQ(damage_count, 2);
    EXPECT_EQ(missile_count, 1);
}

TEST(MessageBus, SubscribeReturnsUsableIdForUnsubscribe) {
    MessageBus bus;
    int hits_a = 0, hits_b = 0;
    auto id_a = bus.subscribe<DamageMsg>([&](const DamageMsg&) { ++hits_a; });
    auto id_b = bus.subscribe<DamageMsg>([&](const DamageMsg&) { ++hits_b; });
    bus.publish(DamageMsg{});
    EXPECT_EQ(hits_a, 1);
    EXPECT_EQ(hits_b, 1);

    bus.unsubscribe<DamageMsg>(id_a);
    bus.publish(DamageMsg{});
    EXPECT_EQ(hits_a, 1);  // a not called again
    EXPECT_EQ(hits_b, 2);  // b still fires

    bus.unsubscribe<DamageMsg>(id_b);
    bus.publish(DamageMsg{});
    EXPECT_EQ(hits_a, 1);
    EXPECT_EQ(hits_b, 2);
}

TEST(MessageBus, UnsubscribeUnknownIdIsSafe) {
    MessageBus bus;
    bus.subscribe<DamageMsg>([&](const DamageMsg&) {});
    EXPECT_NO_THROW(bus.unsubscribe<DamageMsg>(99));
    EXPECT_NO_THROW(bus.unsubscribe<EmptyMsg>(0));  // type never subscribed
}

TEST(MessageBus, HandlerCountReflectsSubscriptions) {
    MessageBus bus;
    EXPECT_EQ(bus.handler_count<DamageMsg>(), 0u);
    auto id1 = bus.subscribe<DamageMsg>([&](const DamageMsg&) {});
    EXPECT_EQ(bus.handler_count<DamageMsg>(), 1u);
    auto id2 = bus.subscribe<DamageMsg>([&](const DamageMsg&) {});
    EXPECT_EQ(bus.handler_count<DamageMsg>(), 2u);
    bus.unsubscribe<DamageMsg>(id1);
    EXPECT_EQ(bus.handler_count<DamageMsg>(), 1u);
    bus.unsubscribe<DamageMsg>(id2);
    EXPECT_EQ(bus.handler_count<DamageMsg>(), 0u);
    (void)id1; (void)id2;
}

// ============================================================================
// Payload-carrying messages — handlers inspect fields at delivery time
// ============================================================================
TEST(MessageBus, HandlerReceivesFullMessagePayload) {
    MessageBus bus;
    MissileFireMsg captured;
    bus.subscribe<MissileFireMsg>([&](const MissileFireMsg& m) { captured = m; });
    bus.publish(MissileFireMsg{42u, 100u, 7u, 4});
    EXPECT_EQ(captured.shooter_id, 42u);
    EXPECT_EQ(captured.missile_id, 100u);
    EXPECT_EQ(captured.target_id, 7u);
    EXPECT_EQ(captured.weapon_type, 4);
}

TEST(MessageBus, HandlerCanMutateLocalStateFromMessage) {
    MessageBus bus;
    std::vector<uint64_t> seen;
    bus.subscribe<WingmanCmdMsg>([&](const WingmanCmdMsg& m) {
        seen.push_back(m.receiver_id);
    });
    bus.publish(WingmanCmdMsg{1, 10, 0});
    bus.publish(WingmanCmdMsg{1, 20, 0});
    bus.publish(WingmanCmdMsg{1, 30, 0});
    ASSERT_EQ(seen.size(), 3u);
    EXPECT_EQ(seen[0], 10u);
    EXPECT_EQ(seen[1], 20u);
    EXPECT_EQ(seen[2], 30u);
}

// ============================================================================
// Deferred delivery
// ============================================================================
TEST(MessageBus, DeferredMessagesAreNotDeliveredUntilFlush) {
    MessageBus bus;
    int hits = 0;
    bus.subscribe<DamageMsg>([&](const DamageMsg&) { ++hits; });

    bus.publish_deferred(DamageMsg{});
    bus.publish_deferred(DamageMsg{});
    EXPECT_EQ(hits, 0);
    EXPECT_EQ(bus.pending_count(), 2u);

    bus.flush_pending();
    EXPECT_EQ(hits, 2);
    EXPECT_EQ(bus.pending_count(), 0u);
}

TEST(MessageBus, DeferredDeliversToCurrentHandlersAtFlushTime) {
    // A handler subscribed AFTER publish_deferred but BEFORE flush should
    // still fire — the dispatch snapshot is taken at flush, not at enqueue.
    MessageBus bus;
    int hits = 0;
    bus.publish_deferred(DamageMsg{});
    EXPECT_EQ(hits, 0);
    bus.subscribe<DamageMsg>([&](const DamageMsg&) { ++hits; });
    bus.flush_pending();
    EXPECT_EQ(hits, 1);
}

TEST(MessageBus, FlushWithNoPendingIsNoop) {
    MessageBus bus;
    EXPECT_NO_THROW(bus.flush_pending());
    EXPECT_EQ(bus.pending_count(), 0u);
}

TEST(MessageBus, FlushDrainsInFifoOrder) {
    MessageBus bus;
    std::vector<int> seen;
    bus.subscribe<DamageMsg>([&](const DamageMsg& m) {
        seen.push_back(static_cast<int>(m.target_id));
    });
    for (int i = 0; i < 5; ++i) {
        bus.publish_deferred(DamageMsg{static_cast<uint64_t>(i), 0, 0, 0.0f});
    }
    bus.flush_pending();
    ASSERT_EQ(seen.size(), 5u);
    for (int i = 0; i < 5; ++i) EXPECT_EQ(seen[i], i);
}

TEST(MessageBus, DeferredMessageCopiedPerHandler) {
    // Two handlers receive independent copies — one mutating its copy
    // must not affect the other.
    MessageBus bus;
    DamageMsg seen_a{0, 0, 0, 0.0f};
    DamageMsg seen_b{0, 0, 0, 0.0f};
    bus.subscribe<DamageMsg>([&](const DamageMsg& m) {
        seen_a = m;
        // mutate the local copy
        seen_a.strength = 999.0f;
    });
    bus.subscribe<DamageMsg>([&](const DamageMsg& m) { seen_b = m; });
    bus.publish_deferred(DamageMsg{7, 0, 0, 1.0f});
    bus.flush_pending();
    EXPECT_FLOAT_EQ(seen_a.strength, 999.0f);
    EXPECT_FLOAT_EQ(seen_b.strength, 1.0f);
}

TEST(MessageBus, DeferredWithNoSubscribersDropsOnFlush) {
    // With dispatch-at-flush semantics (not snapshot-at-enqueue), the
    // message is always enqueued. If no handler is subscribed when
    // flush runs, nothing fires — but the pending entry is consumed.
    // This means a subscribe() between publish_deferred and flush WILL
    // pick up the message (see DeferredDeliversToCurrentHandlersAtFlushTime).
    MessageBus bus;
    bus.publish_deferred(DamageMsg{});
    EXPECT_EQ(bus.pending_count(), 1u);  // one entry waiting
    bus.flush_pending();                 // no handlers → no-op
    EXPECT_EQ(bus.pending_count(), 0u);
    SUCCEED();
}

// ============================================================================
// send_to — cross-bus convenience
// ============================================================================
TEST(MessageBus, SendToEnqueuesOnTargetBus) {
    MessageBus campaign_bus;
    MessageBus sim_bus;
    int sim_hits = 0;
    sim_bus.subscribe<DamageMsg>([&](const DamageMsg&) { ++sim_hits; });

    send_to(sim_bus, DamageMsg{});
    EXPECT_EQ(sim_hits, 0);
    EXPECT_EQ(sim_bus.pending_count(), 1u);

    sim_bus.flush_pending();
    EXPECT_EQ(sim_hits, 1);
}

// ============================================================================
// Edge cases
// ============================================================================
TEST(MessageBus, EmptyStructMessageWorks) {
    MessageBus bus;
    int hits = 0;
    bus.subscribe<EmptyMsg>([&](const EmptyMsg&) { ++hits; });
    bus.publish(EmptyMsg{});
    bus.publish_deferred(EmptyMsg{});
    bus.flush_pending();
    EXPECT_EQ(hits, 2);
}

TEST(MessageBus, PublishDuringFlushDoesNotRecurseIntoSameVector) {
    // A handler that itself calls publish_deferred must not corrupt the
    // pending vector mid-iteration. The new message should land in the
    // freshly-swapped (empty) pending list and require a second flush.
    MessageBus bus;
    int hits = 0;
    int reentry_budget = 1;  // only re-enqueue once, so the queue drains
    bus.subscribe<DamageMsg>([&](const DamageMsg&) { ++hits; });
    bus.subscribe<DamageMsg>([&](const DamageMsg&) {
        ++hits;
        if (reentry_budget > 0) {
            --reentry_budget;
            // During flush, enqueue another. This must NOT be delivered
            // in the current flush — it goes into the swapped-in empty
            // pending list and requires a second flush.
            bus.publish_deferred(DamageMsg{});
        }
    });

    bus.publish_deferred(DamageMsg{});
    bus.flush_pending();
    // First flush: 2 handlers fire (one of them enqueues 1 new msg).
    // publish_deferred stores ONE pending entry per message (the
    // per-handler fan-out happens when that entry is flushed and
    // re-enters publish, which dispatches to both handlers).
    EXPECT_EQ(hits, 2);
    EXPECT_EQ(bus.pending_count(), 1u);

    bus.flush_pending();
    EXPECT_EQ(hits, 4);
    EXPECT_EQ(bus.pending_count(), 0u);  // budget exhausted, no re-enqueue
}
