// test_queue.cpp — MessageQueue<Msg>: push / drain / size.

#include <gtest/gtest.h>
#include <f4/messaging/f4_messaging.hpp>
#include <string>
#include <vector>

using namespace f4::messaging;

namespace {
struct CmdMsg {
    int code = 0;
    std::string text;
};
} // namespace

TEST(MessageQueue, NewQueueIsEmpty) {
    MessageQueue<CmdMsg> q;
    EXPECT_EQ(q.size(), 0u);
    EXPECT_TRUE(q.drain().empty());
}

TEST(MessageQueue, PushThenDrainReturnsInFifoOrder) {
    MessageQueue<CmdMsg> q;
    q.push(CmdMsg{1, "alpha"});
    q.push(CmdMsg{2, "bravo"});
    q.push(CmdMsg{3, "charlie"});
    EXPECT_EQ(q.size(), 3u);

    auto batch = q.drain();
    ASSERT_EQ(batch.size(), 3u);
    EXPECT_EQ(batch[0].code, 1);
    EXPECT_EQ(batch[0].text, "alpha");
    EXPECT_EQ(batch[1].code, 2);
    EXPECT_EQ(batch[2].code, 3);
    EXPECT_TRUE(q.drain().empty());
    EXPECT_EQ(q.size(), 0u);
}

TEST(MessageQueue, DrainEmptyQueueIsEmpty) {
    MessageQueue<CmdMsg> q;
    auto batch = q.drain();
    EXPECT_TRUE(batch.empty());
}

TEST(MessageQueue, PushAfterDrainWorks) {
    MessageQueue<CmdMsg> q;
    q.push(CmdMsg{1, ""});
    (void)q.drain();
    q.push(CmdMsg{2, ""});
    auto batch = q.drain();
    ASSERT_EQ(batch.size(), 1u);
    EXPECT_EQ(batch[0].code, 2);
}

TEST(MessageQueue, MoveOnlyMessageWorks) {
    // Queue must support move-only types (e.g. owning payloads).
    struct MoveOnly {
        int v = 0;
        MoveOnly() = default;
        explicit MoveOnly(int x) : v(x) {}
        MoveOnly(MoveOnly&&) noexcept = default;
        MoveOnly& operator=(MoveOnly&&) noexcept = default;
        MoveOnly(const MoveOnly&) = delete;
        MoveOnly& operator=(const MoveOnly&) = delete;
    };

    MessageQueue<MoveOnly> q;
    q.push(MoveOnly{42});
    q.push(MoveOnly{99});
    auto batch = q.drain();
    ASSERT_EQ(batch.size(), 2u);
    EXPECT_EQ(batch[0].v, 42);
    EXPECT_EQ(batch[1].v, 99);
}
