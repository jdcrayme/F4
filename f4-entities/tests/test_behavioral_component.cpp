// test_behavioral_component.cpp
//
// Unit tests for BehavioralComponentBase, BehavioralComponent<Derived>,
// EntityWorld::update_all(), and the on_attached() lifecycle hook.
//
// These tests verify the Phase A.1 contract:
//   1. BehavioralComponentBase exposes priority() (default 0) and a pure-
//      virtual update(dt, bus).
//   2. update_all() iterates behavioral components in two passes:
//        Pass 1: priority >= update_phase::BRAIN_THRESHOLD (brains).
//        Pass 2: 0 < priority < BRAIN_THRESHOLD (physics).
//      Components with priority == 0 (passive data) are skipped.
//   3. Within each pass, components run in entity-storage order.
//   4. Dead entities are skipped.
//   5. on_attached() is called once by EntityHandle::add<T>() with the
//      owning handle, and the handle remains valid for sibling lookups.

#include <gtest/gtest.h>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include <f4/entities/f4_entities.hpp>
#include <f4/messaging/f4_messaging.hpp>

using namespace f4::entities;
using namespace f4::messaging;

namespace {

// ----------------------------------------------------------------------------
// Test fixtures — minimal behavioral components with side-effect counters
// so the tests can observe ordering and call counts.
// ----------------------------------------------------------------------------

// A "brain" — priority 100 (>= BRAIN_THRESHOLD = 75, runs in pass 1).
struct FakeBrain : BehavioralComponent<FakeBrain> {
    std::atomic<int> update_count{0};
    std::atomic<double> last_dt{0.0};
    std::vector<int>* order_log{nullptr};
    int self_id{0};

    int priority() const noexcept override { return update_phase::BRAIN_PRIORITY; }

    void update(double dt, MessageBus& /*bus*/) override {
        ++update_count;
        last_dt = dt;
        if (order_log) order_log->push_back(self_id);
    }
};

// A "physics" component — priority 50 (0 < p < 75, runs in pass 2).
struct FakePhysics : BehavioralComponent<FakePhysics> {
    std::atomic<int> update_count{0};
    std::vector<int>* order_log{nullptr};
    int self_id{0};

    int priority() const noexcept override { return update_phase::PHYSICS_PRIORITY; }

    void update(double /*dt*/, MessageBus& /*bus*/) override {
        ++update_count;
        if (order_log) order_log->push_back(self_id);
    }
};

// A behavioral component that captures its owner EntityHandle in
// on_attached() and uses it to look up a sibling during update().
// This is the brain -> flight-model pattern A.2 will need.
struct BrainWithBackRef : BehavioralComponent<BrainWithBackRef> {
    EntityHandle* owner{nullptr};
    FakePhysics* sibling_seen{nullptr};
    std::atomic<int> attached_count{0};

    int priority() const noexcept override { return update_phase::BRAIN_PRIORITY; }

    void on_attached(EntityHandle& self) override {
        owner = &self;
        ++attached_count;
    }

    void update(double /*dt*/, MessageBus& /*bus*/) override {
        if (owner) sibling_seen = owner->get<FakePhysics>();
    }
};

// A behavioral component that publishes a synchronous message on update,
// so we can verify the bus is wired through correctly.
struct PingMessage { int value; };
struct PingingBrain : BehavioralComponent<PingingBrain> {
    int value_to_send{42};
    int priority() const noexcept override { return update_phase::BRAIN_PRIORITY; }
    void update(double /*dt*/, MessageBus& bus) override {
        bus.publish(PingMessage{value_to_send});
    }
};

} // namespace

// ============================================================================
// BehavioralComponentBase — defaults and identity
// ============================================================================
TEST(BehavioralComponent, DefaultPriorityIsZero) {
    // A fresh BehavioralComponent<FakeBrain> has priority set by the
    // override, not the default. To check the default we use a struct
    // that does NOT override priority().
    struct NoPriority : BehavioralComponent<NoPriority> {
        void update(double, MessageBus&) override {}
    };
    NoPriority np;
    EXPECT_EQ(np.priority(), 0);
}

TEST(BehavioralComponent, TypeIdMatchesDerivedType) {
    FakeBrain b;
    EXPECT_EQ(b.type_id(), std::type_index(typeid(FakeBrain)));

    FakePhysics p;
    EXPECT_EQ(p.type_id(), std::type_index(typeid(FakePhysics)));
    EXPECT_NE(b.type_id(), p.type_id());
}

TEST(BehavioralComponent, BrainPriorityIsHundred) {
    FakeBrain b;
    EXPECT_EQ(b.priority(), 100);
    EXPECT_GE(b.priority(), update_phase::BRAIN_THRESHOLD);
}

TEST(BehavioralComponent, PhysicsPriorityIsFifty) {
    FakePhysics p;
    EXPECT_EQ(p.priority(), 50);
    EXPECT_LT(p.priority(), update_phase::BRAIN_THRESHOLD);
    EXPECT_GT(p.priority(), 0);
}

// ============================================================================
// update_all — basic invocation
// ============================================================================
TEST(UpdateAll, CallsBrainUpdate) {
    EntityWorld w;
    MessageBus bus;
    auto h = w.create();
    auto& brain = h.add<FakeBrain>();

    w.update_all(0.016, bus);

    EXPECT_EQ(brain.update_count.load(), 1);
    EXPECT_DOUBLE_EQ(brain.last_dt.load(), 0.016);
}

TEST(UpdateAll, CallsPhysicsUpdate) {
    EntityWorld w;
    MessageBus bus;
    auto h = w.create();
    auto& phys = h.add<FakePhysics>();

    w.update_all(0.016, bus);

    EXPECT_EQ(phys.update_count.load(), 1);
}

TEST(UpdateAll, DoesNotCallPassiveComponentUpdate) {
    // Passive components (Component<T>) don't have update(); they're skipped.
    // This test verifies the dynamic_cast filtering works: a TransformComponent
    // is in the same components map as the behavioral ones, but update_all
    // must not crash trying to call update() on it.
    EntityWorld w;
    MessageBus bus;
    auto h = w.create();
    h.add<TransformComponent>();          // passive — must be skipped
    h.add<CampaignIdentityComponent>();   // passive — must be skipped
    auto& brain = h.add<FakeBrain>();
    auto& phys  = h.add<FakePhysics>();

    w.update_all(0.016, bus);

    EXPECT_EQ(brain.update_count.load(), 1);
    EXPECT_EQ(phys.update_count.load(), 1);
}

TEST(UpdateAll, MultipleTicksAccumulate) {
    EntityWorld w;
    MessageBus bus;
    auto h = w.create();
    auto& brain = h.add<FakeBrain>();

    for (int i = 0; i < 5; ++i) {
        w.update_all(0.1, bus);
    }

    EXPECT_EQ(brain.update_count.load(), 5);
}

// ============================================================================
// update_all — two-pass ordering
// ============================================================================
TEST(UpdateAll, BrainRunsBeforePhysicsOnSameEntity) {
    // Both components on the same entity log their self_id when they run.
    // The brain must appear before the physics in the order log.
    EntityWorld w;
    MessageBus bus;
    auto h = w.create();
    std::vector<int> log;
    auto& brain = h.add<FakeBrain>();
    auto& phys  = h.add<FakePhysics>();
    brain.order_log = &log; brain.self_id = 1;
    phys.order_log  = &log; phys.self_id  = 2;

    w.update_all(0.016, bus);

    ASSERT_EQ(log.size(), 2u);
    EXPECT_EQ(log[0], 1);   // brain first
    EXPECT_EQ(log[1], 2);   // physics second
}

TEST(UpdateAll, AllBrainsRunBeforeAllPhysicsAcrossEntities) {
    // Two entities, each with a brain and a physics component.
    // All brains (pass 1) must run before any physics (pass 2),
    // regardless of entity order.
    EntityWorld w;
    MessageBus bus;
    std::vector<int> log;

    auto h1 = w.create();
    auto& b1 = h1.add<FakeBrain>();
    auto& p1 = h1.add<FakePhysics>();
    b1.order_log = &log; b1.self_id = 10;
    p1.order_log = &log; p1.self_id = 20;

    auto h2 = w.create();
    auto& b2 = h2.add<FakeBrain>();
    auto& p2 = h2.add<FakePhysics>();
    b2.order_log = &log; b2.self_id = 11;
    p2.order_log = &log; p2.self_id = 21;

    w.update_all(0.016, bus);

    ASSERT_EQ(log.size(), 4u);
    // All brains (10, 11) must come before all physics (20, 21).
    EXPECT_EQ(log[0], 10);
    EXPECT_EQ(log[1], 11);
    EXPECT_EQ(log[2], 20);
    EXPECT_EQ(log[3], 21);
}

// ============================================================================
// update_all — dead entities are skipped
// ============================================================================
TEST(UpdateAll, DeadEntitiesAreSkipped) {
    // When an entity is destroyed, its components are destroyed too
    // (the unique_ptr<ComponentBase> in the components map is dropped).
    // So we can't read the dead brain's update_count after destroy —
    // that would be UB. Instead we verify via a shared external counter
    // that the dead brain never wrote to: if update_all correctly skips
    // dead entities, the counter for the dead brain's id stays 0.
    EntityWorld w;
    MessageBus bus;

    std::vector<int> log;   // shared order log; brains push their self_id

    auto live = w.create();
    auto& live_brain = live.add<FakeBrain>();
    live_brain.order_log = &log;
    live_brain.self_id   = 100;

    auto dead = w.create();
    auto& dead_brain = dead.add<FakeBrain>();
    dead_brain.order_log = &log;
    dead_brain.self_id   = 200;

    w.destroy(dead.id());   // destroys dead entity AND its FakeBrain

    w.update_all(0.016, bus);

    // The live brain ran exactly once.
    EXPECT_EQ(live_brain.update_count.load(), 1);

    // The dead brain's id must NOT appear in the log. (If update_all
    // failed to skip dead entities, the dead brain's update would have
    // run on freed memory — likely crashing or pushing 200.)
    ASSERT_EQ(log.size(), 1u);
    EXPECT_EQ(log[0], 100);
}

// ============================================================================
// update_all — bus is wired through
// ============================================================================
TEST(UpdateAll, BusIsForwardedToUpdate) {
    // A brain publishes a PingMessage; a subscriber on the bus receives it.
    EntityWorld w;
    MessageBus bus;

    int received = -1;
    bus.subscribe<PingMessage>([&](const PingMessage& msg) {
        received = msg.value;
    });

    auto h = w.create();
    auto& brain = h.add<PingingBrain>();
    brain.value_to_send = 99;

    w.update_all(0.016, bus);

    EXPECT_EQ(received, 99);
}

// ============================================================================
// on_attached — lifecycle hook
// ============================================================================
TEST(OnAttached, CalledOnceOnAdd) {
    EntityWorld w;
    auto h = w.create();
    auto& brain = h.add<BrainWithBackRef>();

    EXPECT_EQ(brain.attached_count.load(), 1);
    EXPECT_NE(brain.owner, nullptr);
}

TEST(OnAttached, NotCalledForPassiveComponent) {
    // Passive components don't have on_attached (it's a no-op on ComponentBase
    // — but actually ComponentBase doesn't have on_attached at all; only
    // BehavioralComponentBase does). The `if constexpr` in add<T>() gates
    // the call, so passive add<TransformComponent>() must NOT invoke any
    // on_attached. We verify this indirectly: TransformComponent is a plain
    // Component<TransformComponent> with no on_attached method; if add<T>()
    // tried to call it, the code wouldn't compile. So this test passing
    // means the `if constexpr` gate works.
    EntityWorld w;
    auto h = w.create();
    h.add<TransformComponent>();
    h.add<TeamComponent>();
    SUCCEED();
}

TEST(OnAttached, BackRefResolvesSiblingComponent) {
    // The brain's on_attached captures the EntityHandle. In update(), it
    // uses the back-ref to look up a sibling FakePhysics component. This
    // is exactly the brain -> flight-model lookup pattern A.2 needs.
    EntityWorld w;
    MessageBus bus;
    auto h = w.create();
    auto& phys = h.add<FakePhysics>();
    auto& brain = h.add<BrainWithBackRef>();

    // Before update: brain hasn't queried yet.
    EXPECT_EQ(brain.sibling_seen, nullptr);

    w.update_all(0.016, bus);

    // After update: brain's update() queried the FakePhysics sibling via
    // the captured owner handle, and got a pointer to the same instance
    // that the test holds.
    EXPECT_EQ(brain.sibling_seen, &phys);
}

TEST(OnAttached, BackRefSurvivesAcrossTicks) {
    // The handle captured in on_attached must remain valid across many
    // ticks — it's the same EntityHandle that owns this component.
    EntityWorld w;
    MessageBus bus;
    auto h = w.create();
    h.add<FakePhysics>();
    auto& brain = h.add<BrainWithBackRef>();

    for (int i = 0; i < 10; ++i) {
        w.update_all(0.1, bus);
        ASSERT_NE(brain.owner, nullptr);
        ASSERT_NE(brain.sibling_seen, nullptr);
    }
}

// ============================================================================
// update_all — empty world is a no-op
// ============================================================================
TEST(UpdateAll, EmptyWorldIsNoOp) {
    // A fresh EntityWorld with no entities. update_all must not crash.
    EntityWorld w;
    MessageBus bus;
    w.update_all(0.016, bus);
    SUCCEED();
}

TEST(UpdateAll, EntityWithNoBehavioralComponentsIsSkipped) {
    // An entity with only passive components. update_all must not crash
    // and must not call any update (there's nothing to call).
    EntityWorld w;
    MessageBus bus;
    auto h = w.create();
    h.add<TransformComponent>();
    h.add<TeamComponent>();
    h.add<PropertyBag>();

    w.update_all(0.016, bus);
    SUCCEED();
}

// ============================================================================
// update_all — does NOT auto-flush the bus
// ============================================================================
TEST(UpdateAll, DoesNotFlushBusPending) {
    // update_all() must leave deferred messages in the queue. The caller
    // owns the bus lifecycle. We verify by enqueueing a deferred message,
    // calling update_all, and asserting the pending count is unchanged.
    EntityWorld w;
    MessageBus bus;

    bus.publish_deferred(PingMessage{7});
    ASSERT_EQ(bus.pending_count(), 1u);

    auto h = w.create();
    h.add<FakeBrain>();

    w.update_all(0.016, bus);

    EXPECT_EQ(bus.pending_count(), 1u);  // still pending — caller must flush

    // Cleanup: flush so the deferred handler (none in this test) doesn't
    // fire during teardown.
    bus.flush_pending();
}
