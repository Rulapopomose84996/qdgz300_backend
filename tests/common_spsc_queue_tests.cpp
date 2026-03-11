// tests/common_spsc_queue_tests.cpp
// SPSCQueue<T, Cap> 单元测试 — 并发/满/空/drop-oldest 场景
#include <gtest/gtest.h>
#include "qdgz300/common/spsc_queue.h"

#include <thread>
#include <vector>
#include <atomic>
#include <numeric>

using namespace qdgz300;

// ═══ 基本功能测试 ═══

TEST(SPSCQueueTest, EmptyOnConstruction)
{
    SPSCQueue<int, 16> q;
    EXPECT_TRUE(q.empty());
    EXPECT_FALSE(q.full());
    EXPECT_EQ(q.size(), 0u);
    EXPECT_EQ(q.capacity(), 16u);
    EXPECT_EQ(q.usable_capacity(), 15u);
    EXPECT_EQ(q.drop_count(), 0u);
}

TEST(SPSCQueueTest, PushPopSingle)
{
    SPSCQueue<int, 4> q;
    EXPECT_TRUE(q.try_push(42));
    EXPECT_EQ(q.size(), 1u);
    EXPECT_FALSE(q.empty());

    auto val = q.try_pop();
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, 42);
    EXPECT_TRUE(q.empty());
    EXPECT_EQ(q.size(), 0u);
}

TEST(SPSCQueueTest, PopFromEmpty)
{
    SPSCQueue<int, 8> q;
    auto val = q.try_pop();
    EXPECT_FALSE(val.has_value());
}

TEST(SPSCQueueTest, FillToCapacity)
{
    // Cap=4 → 3 usable slots (one reserved for full/empty distinction)
    SPSCQueue<int, 4> q;
    EXPECT_TRUE(q.try_push(1));
    EXPECT_TRUE(q.try_push(2));
    EXPECT_TRUE(q.try_push(3));
    EXPECT_FALSE(q.try_push(4)); // full
    EXPECT_TRUE(q.full());
    EXPECT_EQ(q.size(), 3u);
    EXPECT_EQ(q.usable_capacity(), 3u);
}

TEST(SPSCQueueTest, FillAndDrain)
{
    SPSCQueue<int, 8> q;
    // Fill 7 slots (Cap=8, usable=7)
    for (int i = 0; i < 7; ++i)
    {
        EXPECT_TRUE(q.try_push(i));
    }
    EXPECT_TRUE(q.full());
    EXPECT_FALSE(q.try_push(99));

    // Drain all
    for (int i = 0; i < 7; ++i)
    {
        auto val = q.try_pop();
        ASSERT_TRUE(val.has_value());
        EXPECT_EQ(*val, i);
    }
    EXPECT_TRUE(q.empty());
}

// ═══ Drop-Oldest 测试 ═══

TEST(SPSCQueueTest, DropOldestWhenFull)
{
    SPSCQueue<int, 4> q; // 3 usable slots
    q.try_push(1);
    q.try_push(2);
    q.try_push(3);
    EXPECT_TRUE(q.full());

    // drop_oldest_push should drop item 1 and push 4
    bool direct = q.drop_oldest_push(4);
    EXPECT_FALSE(direct); // was full, had to drop
    EXPECT_EQ(q.drop_count(), 1u);
    EXPECT_EQ(q.size(), 3u);

    // Verify order: 2, 3, 4 (oldest "1" was dropped)
    EXPECT_EQ(*q.try_pop(), 2);
    EXPECT_EQ(*q.try_pop(), 3);
    EXPECT_EQ(*q.try_pop(), 4);
    EXPECT_TRUE(q.empty());
}

TEST(SPSCQueueTest, DropOldestWhenNotFull)
{
    SPSCQueue<int, 8> q;
    q.try_push(1);
    q.try_push(2);

    // Not full — should push directly
    bool direct = q.drop_oldest_push(3);
    EXPECT_TRUE(direct);
    EXPECT_EQ(q.drop_count(), 0u);
    EXPECT_EQ(q.size(), 3u);
}

TEST(SPSCQueueTest, DropOldestMultiple)
{
    SPSCQueue<int, 4> q; // 3 usable
    q.try_push(1);
    q.try_push(2);
    q.try_push(3);

    // Push 3 more via drop_oldest
    q.drop_oldest_push(4);
    q.drop_oldest_push(5);
    q.drop_oldest_push(6);

    EXPECT_EQ(q.drop_count(), 3u);
    EXPECT_EQ(*q.try_pop(), 4);
    EXPECT_EQ(*q.try_pop(), 5);
    EXPECT_EQ(*q.try_pop(), 6);
}

// ═══ Move 语义测试 ═══

TEST(SPSCQueueTest, MoveSemantics)
{
    SPSCQueue<std::unique_ptr<int>, 4> q;
    auto p = std::make_unique<int>(100);
    EXPECT_TRUE(q.try_push(std::move(p)));
    EXPECT_EQ(p, nullptr); // moved

    auto val = q.try_pop();
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(**val, 100);
}

// ═══ Reset 测试 ═══

TEST(SPSCQueueTest, ResetClearsQueue)
{
    SPSCQueue<int, 8> q;
    q.try_push(1);
    q.try_push(2);
    q.try_push(3);
    q.drop_oldest_push(4);

    q.reset();
    EXPECT_TRUE(q.empty());
    EXPECT_EQ(q.size(), 0u);
    EXPECT_EQ(q.drop_count(), 0u);

    // Should work normally after reset
    EXPECT_TRUE(q.try_push(99));
    EXPECT_EQ(*q.try_pop(), 99);
}

// ═══ 并发测试 ═══

TEST(SPSCQueueTest, ConcurrentProducerConsumer)
{
    constexpr size_t CAP = 1024;
    constexpr size_t NUM_ITEMS = 100000;
    SPSCQueue<uint64_t, CAP> q;

    std::atomic<bool> start{false};
    std::vector<uint64_t> consumed;
    consumed.reserve(NUM_ITEMS);

    // Consumer thread
    std::thread consumer([&]()
                         {
        while (!start.load(std::memory_order_acquire))
        {
            // spin wait for start
        }
        size_t count = 0;
        while (count < NUM_ITEMS)
        {
            auto val = q.try_pop();
            if (val.has_value())
            {
                consumed.push_back(*val);
                ++count;
            }
        } });

    // Producer thread
    std::thread producer([&]()
                         {
        while (!start.load(std::memory_order_acquire))
        {
            // spin wait
        }
        for (uint64_t i = 0; i < NUM_ITEMS; ++i)
        {
            while (!q.try_push(i))
            {
                // backpressure — spin until space available
            }
        } });

    // Go!
    start.store(true, std::memory_order_release);
    producer.join();
    consumer.join();

    // Verify all items received in order (FIFO)
    ASSERT_EQ(consumed.size(), NUM_ITEMS);
    for (uint64_t i = 0; i < NUM_ITEMS; ++i)
    {
        EXPECT_EQ(consumed[i], i) << "Out-of-order at index " << i;
    }
}

TEST(SPSCQueueTest, ConcurrentWithDropOldest)
{
    constexpr size_t CAP = 16;
    constexpr size_t NUM_ITEMS = 10000;
    SPSCQueue<uint64_t, CAP> q;

    std::atomic<bool> done{false};
    std::vector<uint64_t> consumed;

    // Slow consumer
    std::thread consumer([&]()
                         {
        while (!done.load(std::memory_order_acquire) || !q.empty())
        {
            auto val = q.try_pop();
            if (val.has_value())
            {
                consumed.push_back(*val);
            }
        } });

    // Fast producer with drop_oldest
    for (uint64_t i = 0; i < NUM_ITEMS; ++i)
    {
        q.drop_oldest_push(i);
    }
    done.store(true, std::memory_order_release);

    consumer.join();

    // 验证：consumed + dropped = NUM_ITEMS
    // consumed 中的值应单调递增（FIFO 保证）
    EXPECT_GT(consumed.size(), 0u);
    for (size_t i = 1; i < consumed.size(); ++i)
    {
        EXPECT_GT(consumed[i], consumed[i - 1])
            << "Non-monotonic at index " << i;
    }

    // drop_count + consumed.size() 应接近 NUM_ITEMS
    // 由于并发时序，可能有少量偏差
    uint64_t total = q.drop_count() + consumed.size();
    EXPECT_GE(total, NUM_ITEMS - CAP); // 至少大部分被处理
}

// ═══ 编译期约束测试 ═══

TEST(SPSCQueueTest, CapacityIsPowerOfTwo)
{
    // 以下应可编译
    SPSCQueue<int, 2> q2;
    SPSCQueue<int, 64> q64;
    SPSCQueue<int, 1024> q1024;
    (void)q2;
    (void)q64;
    (void)q1024;

    // 非 2 的幂次应触发 static_assert（编译期验证，无法在运行时测试）
}

// ═══ 大容量循环测试 ═══

TEST(SPSCQueueTest, WrapAroundStress)
{
    SPSCQueue<uint32_t, 8> q; // 7 usable slots
    // Push/pop many rounds to test index wrap-around
    for (uint32_t round = 0; round < 1000; ++round)
    {
        for (uint32_t i = 0; i < 7; ++i)
        {
            ASSERT_TRUE(q.try_push(round * 7 + i));
        }
        ASSERT_TRUE(q.full());
        for (uint32_t i = 0; i < 7; ++i)
        {
            auto val = q.try_pop();
            ASSERT_TRUE(val.has_value());
            EXPECT_EQ(*val, round * 7 + i);
        }
        ASSERT_TRUE(q.empty());
    }
}
