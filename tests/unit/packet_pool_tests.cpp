#include "qdgz300/m01_receiver/network/packet_pool.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

#include <gtest/gtest.h>

using receiver::network::PacketPool;

TEST(PacketPoolTests, AllocateAndDeallocate)
{
    PacketPool pool(2048, 1);
    uint8_t *first = pool.allocate();
    ASSERT_NE(first, nullptr);
    pool.deallocate(first);
    uint8_t *second = pool.allocate();
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(first, second);
    pool.deallocate(second);
}

TEST(PacketPoolTests, BatchAllocate)
{
    PacketPool pool(1024, 1000);
    std::vector<uint8_t *> ptrs;
    ptrs.reserve(1000);
    for (int i = 0; i < 1000; ++i)
    {
        uint8_t *buf = pool.allocate();
        ASSERT_NE(buf, nullptr);
        ptrs.push_back(buf);
    }
    for (uint8_t *buf : ptrs)
    {
        pool.deallocate(buf);
    }
}

TEST(PacketPoolTests, ReuseAfterDeallocate)
{
    PacketPool pool(512, 2);
    uint8_t *a = pool.allocate();
    uint8_t *b = pool.allocate();
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    pool.deallocate(a);
    uint8_t *c = pool.allocate();
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c, a);
    pool.deallocate(b);
    pool.deallocate(c);
}

TEST(PacketPoolTests, ThreadSafety)
{
    PacketPool pool(2048, 256);
    constexpr int kThreads = 4;
    constexpr int kItersPerThread = 1500;

    std::atomic<int> null_allocs{0};
    std::atomic<bool> duplicate_in_flight{false};
    std::mutex set_mutex;
    std::unordered_set<uint8_t *> in_flight;
    std::vector<std::thread> workers;
    workers.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t)
    {
        workers.emplace_back([&]()
                             {
                                 for (int i = 0; i < kItersPerThread; ++i)
                                 {
                                     uint8_t *p = pool.allocate();
                                     if (p == nullptr)
                                     {
                                         null_allocs.fetch_add(1, std::memory_order_relaxed);
                                         continue;
                                     }

                                     {
                                         std::lock_guard<std::mutex> lock(set_mutex);
                                         const auto inserted = in_flight.insert(p).second;
                                         if (!inserted)
                                         {
                                             duplicate_in_flight.store(true, std::memory_order_relaxed);
                                         }
                                     }

                                     {
                                         std::lock_guard<std::mutex> lock(set_mutex);
                                         in_flight.erase(p);
                                     }
                                     pool.deallocate(p);
                                 } });
    }

    for (auto &th : workers)
    {
        th.join();
    }

    EXPECT_EQ(null_allocs.load(std::memory_order_relaxed), 0);
    EXPECT_FALSE(duplicate_in_flight.load(std::memory_order_relaxed));
    EXPECT_TRUE(in_flight.empty());
}

