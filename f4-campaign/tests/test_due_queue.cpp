// f4-campaign/tests/test_due_queue.cpp
//
// AGG-2a — the deterministic due-queue + stagger primitives
// (include/f4/campaign/due_queue.hpp). The contract:
//
//   1. Pop order is exactly (due_time, priority, insertion_seq) —
//      no RNG, no wall clock, no float keys; two identically-driven
//      queues pop identical work in identical order.
//   2. The due boundary: due == now fires, due == now+1 waits.
//   3. Re-entrant scheduling: work the visitor schedules mid-pass
//      participates immediately — a due-now re-arm fires within the
//      same pass, ordered by the key (earlier seq first).
//   4. The stagger: stagger_phase(VU, interval) is a pure function of
//      its inputs (FNV-1a, NOT std::hash — the phase must be pinned
//      across stdlib builds for the replay axis), lands in
//      [0, interval), and spreads 600 VUs across a 60 s interval.

#include <f4/campaign/due_queue.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <set>
#include <string>
#include <vector>

using f4::campaign::DueQueue;
using f4::campaign::stagger_phase;
using f4::campaign::vu_hash;

namespace {

struct Fired {
    f4::campaign::CampaignTime due = 0;
    int priority = 0;
    std::string payload;
};

/// Drain everything due from `q` at `now`, recording the fire order.
std::vector<Fired> drain(DueQueue<std::string>& q,
                         f4::campaign::CampaignTime now) {
    std::vector<Fired> out;
    q.pop_due(now, [&](std::string&& s) {
        // The key the entry was scheduled under is probed by the
        // caller's expectations; the drain records the payload order.
        out.push_back(Fired{0, 0, std::move(s)});
    });
    return out;
}

} // namespace

TEST(DueQueue, PopOrderIsDueThenPriorityThenSeq) {
    DueQueue<std::string> q;
    q.schedule(10, 1, "late-p1");
    q.schedule(5, 1, "early-p1");
    q.schedule(5, 0, "early-p0");
    q.schedule(5, 0, "early-p0-second");

    const auto fired = drain(q, 100);
    ASSERT_EQ(fired.size(), 4u);
    EXPECT_EQ(fired[0].payload, "early-p0");
    EXPECT_EQ(fired[1].payload, "early-p0-second");
    EXPECT_EQ(fired[2].payload, "early-p1");
    EXPECT_EQ(fired[3].payload, "late-p1");
    EXPECT_TRUE(q.empty());
}

TEST(DueQueue, DueBoundaryIsInclusiveAtNow) {
    DueQueue<int> q;
    q.schedule(7, 0, 7);
    q.schedule(8, 0, 8);

    int fired = 0;
    q.pop_due(7, [&](int&&) { ++fired; });
    EXPECT_EQ(fired, 1);
    EXPECT_EQ(q.size(), 1u);

    fired = 0;
    q.pop_due(8, [&](int&&) { ++fired; });
    EXPECT_EQ(fired, 1);
    EXPECT_TRUE(q.empty());
}

TEST(DueQueue, VisitorScheduledWorkFiresWithinThePass) {
    // The re-arm: a fired unit schedules its next tick. A re-arm with
    // due <= now fires in the SAME pass, after everything already in
    // flight (later seq); a re-arm with a later due waits.
    DueQueue<int> q;
    q.schedule(0, 0, 1);
    q.schedule(0, 0, 99);   // an unfired peer at the same key

    std::vector<int> order;
    q.pop_due(0, [&](int&& v) {
        order.push_back(v);
        if (v == 1) q.schedule(0, 0, 2);       // due now — fires this pass
        if (v == 99) q.schedule(5, 0, 100);    // later — waits
    });

    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], 1);
    EXPECT_EQ(order[1], 99);
    EXPECT_EQ(order[2], 2);   // the re-arm fires after both originals
    EXPECT_EQ(q.size(), 1u);  // the later re-arm still waits
}

TEST(DueQueue, PeekAndClear) {
    DueQueue<int> q;
    EXPECT_FALSE(q.peek_due().has_value());
    q.schedule(30, 0, 0);
    q.schedule(10, 0, 0);
    ASSERT_TRUE(q.peek_due().has_value());
    EXPECT_EQ(*q.peek_due(), 10);
    q.clear();
    EXPECT_TRUE(q.empty());
}

TEST(DueQueue, TwoIdenticalQueuesPopIdentically) {
    // The determinism contract, by construction: same schedule calls,
    // same pop calls, same fire order — regardless of the schedule
    // call ORDER across keys.
    DueQueue<std::string> a, b;
    a.schedule(3, 1, "x");
    a.schedule(2, 0, "y");
    a.schedule(2, 0, "z");
    b.schedule(2, 0, "y");
    b.schedule(3, 1, "x");
    b.schedule(2, 0, "z");

    const auto fa = drain(a, 10);
    const auto fb = drain(b, 10);
    ASSERT_EQ(fa.size(), fb.size());
    for (std::size_t i = 0; i < fa.size(); ++i) {
        EXPECT_EQ(fa[i].payload, fb[i].payload);
    }
}

TEST(Stagger, IsAPureFunctionOfVuAndInterval) {
    for (std::uint32_t vu = 1; vu < 200; ++vu) {
        const auto a = stagger_phase(vu, 60);
        const auto b = stagger_phase(vu, 60);
        ASSERT_EQ(a, b) << "vu " << vu;
        ASSERT_GE(a, 0);
        ASSERT_LT(a, 60);
    }
    // Non-positive intervals degenerate to 0 (always due — the
    // caller's contract decides what that means).
    EXPECT_EQ(stagger_phase(42, 0), 0);
    EXPECT_EQ(stagger_phase(42, -5), 0);
    // The hash separates neighbouring VUs (smoke — the spread test
    // below is the real property).
    EXPECT_NE(vu_hash(1), vu_hash(2));
}

TEST(Stagger, SpreadsSixHundredVusAcrossTheInterval) {
    // The anti-thundering-herd property the reference's rand() jitter
    // used to buy, deterministically: 600 VUs over a 60 s interval
    // cover at least 50 of the 60 slots.
    std::set<int> slots;
    for (std::uint32_t vu = 1; vu <= 600; ++vu) {
        slots.insert(static_cast<int>(stagger_phase(vu, 60)));
    }
    EXPECT_GE(slots.size(), 50u);
}
