// tests/common_memory_pool_tests.cpp
// NUMAPool<T> 单元测试 — 分配/归还/耗尽/并发
#include <gtest/gtest.h>
#include "qdgz300/common/memory_pool.h"

#include <thread>
#include <vector>
#include <set>
#include <atomic>

using namespace qdgz300;

namespace
{
    struct TestBlock
    {
        uint64_t a;
        uint64_t b;
        uint32_t c;
    };
} // namespace

// ═══ 基本功能测试 ═══

TEST(NUMAPoolTest, ConstructionAndBasicProperties)
{
    NUMAPool<TestBlock> pool(64, 0); // 64 blocks, NUMA node 0
    EXPECT_EQ(pool.total(), 64u);
    EXPECT_EQ(pool.available(), 64u);
    EXPECT_EQ(pool.numa_node(), 0);
}

TEST(NUMAPoolTest, AllocateAndDeallocate)
{
    NUMAPool<TestBlock> pool(16, 0);

    auto *ptr = pool.allocate();
    ASSERT_NE(ptr, nullptr);
    EXPECT_EQ(pool.available(), 15u);

    // 使用分配的内存
    ptr->a = 42;
    ptr->b = 100;
    ptr->c = 999;
    EXPECT_EQ(ptr->a, 42u);

    pool.deallocate(ptr);
    EXPECT_EQ(pool.available(), 16u);
}

TEST(NUMAPoolTest, AllocateAll)
{
    constexpr size_t COUNT = 32;
    NUMAPool<TestBlock> pool(COUNT, 0);

    std::vector<TestBlock *> ptrs;
    for (size_t i = 0; i < COUNT; ++i)
    {
        auto *p = pool.allocate();
        ASSERT_NE(p, nullptr) << "Failed at allocation #" << i;
        ptrs.push_back(p);
    }

    EXPECT_EQ(pool.available(), 0u);

    // 池耗尽 — 应返回 nullptr
    EXPECT_EQ(pool.allocate(), nullptr);

    // 全部归还
    for (auto *p : ptrs)
    {
        pool.deallocate(p);
    }
    EXPECT_EQ(pool.available(), COUNT);
}

TEST(NUMAPoolTest, UniquePointers)
{
    constexpr size_t COUNT = 64;
    NUMAPool<TestBlock> pool(COUNT, 0);

    std::set<TestBlock *> unique_ptrs;
    for (size_t i = 0; i < COUNT; ++i)
    {
        auto *p = pool.allocate();
        ASSERT_NE(p, nullptr);
        // 每个分配应返回唯一地址
        const bool inserted = unique_ptrs.insert(p).second;
        EXPECT_TRUE(inserted) << "Duplicate pointer at allocation #" << i;
    }
    EXPECT_EQ(unique_ptrs.size(), COUNT);

    // 归还后再分配，地址应复用
    for (auto *p : unique_ptrs)
    {
        pool.deallocate(p);
    }

    std::set<TestBlock *> realloc_ptrs;
    for (size_t i = 0; i < COUNT; ++i)
    {
        auto *p = pool.allocate();
        ASSERT_NE(p, nullptr);
        realloc_ptrs.insert(p);
    }
    // 复用的地址应该都在原始集合中
    for (auto *p : realloc_ptrs)
    {
        EXPECT_TRUE(unique_ptrs.count(p) > 0)
            << "Reallocation returned an unexpected address";
    }

    for (auto *p : realloc_ptrs)
    {
        pool.deallocate(p);
    }
}

TEST(NUMAPoolTest, DeallocateNull)
{
    NUMAPool<TestBlock> pool(8, 0);
    // Should not crash
    pool.deallocate(nullptr);
    EXPECT_EQ(pool.available(), 8u);
}

TEST(NUMAPoolTest, OwnsCheck)
{
    NUMAPool<TestBlock> pool(16, 0);
    auto *p = pool.allocate();
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(pool.owns(p));

    TestBlock stack_block;
    EXPECT_FALSE(pool.owns(&stack_block));

    pool.deallocate(p);
}

TEST(NUMAPoolTest, ZeroSizePool)
{
    NUMAPool<TestBlock> pool(0, 0);
    EXPECT_EQ(pool.total(), 0u);
    EXPECT_EQ(pool.available(), 0u);
    EXPECT_EQ(pool.allocate(), nullptr);
}

// ═══ 并发测试 ═══

TEST(NUMAPoolTest, ConcurrentAllocDeallocStress)
{
    constexpr size_t POOL_SIZE = 256;
    constexpr size_t OPS_PER_THREAD = 10000;
    constexpr size_t NUM_THREADS = 4;

    NUMAPool<TestBlock> pool(POOL_SIZE, 0);
    std::atomic<bool> start{false};
    std::atomic<uint64_t> alloc_fail_count{0};
    std::atomic<uint64_t> total_allocs{0};

    auto worker = [&]()
    {
        while (!start.load(std::memory_order_acquire))
        {
            // spin
        }
        std::vector<TestBlock *> local;
        local.reserve(32);

        for (size_t i = 0; i < OPS_PER_THREAD; ++i)
        {
            // 交替分配和归还
            if (local.size() < 16)
            {
                auto *p = pool.allocate();
                if (p)
                {
                    p->a = i;
                    local.push_back(p);
                    total_allocs.fetch_add(1, std::memory_order_relaxed);
                }
                else
                {
                    alloc_fail_count.fetch_add(1, std::memory_order_relaxed);
                }
            }
            else
            {
                // 归还一半
                size_t half = local.size() / 2;
                for (size_t j = 0; j < half; ++j)
                {
                    pool.deallocate(local.back());
                    local.pop_back();
                }
            }
        }

        // 归还所有剩余
        for (auto *p : local)
        {
            pool.deallocate(p);
        }
    };

    std::vector<std::thread> threads;
    for (size_t i = 0; i < NUM_THREADS; ++i)
    {
        threads.emplace_back(worker);
    }

    start.store(true, std::memory_order_release);
    for (auto &t : threads)
    {
        t.join();
    }

    // 全部归还后，可用数应等于总数
    EXPECT_EQ(pool.available(), POOL_SIZE);
}

// ═══ 不同类型测试 ═══

TEST(NUMAPoolTest, SmallType)
{
    NUMAPool<uint32_t> pool(128, 0);
    auto *p = pool.allocate();
    ASSERT_NE(p, nullptr);
    *p = 0xDEADBEEF;
    EXPECT_EQ(*p, 0xDEADBEEF);
    pool.deallocate(p);
}

TEST(NUMAPoolTest, LargeType)
{
    struct LargeBlock
    {
        uint8_t data[4096];
    };
    NUMAPool<LargeBlock> pool(4, 0);
    auto *p = pool.allocate();
    ASSERT_NE(p, nullptr);
    std::memset(p->data, 0xFF, sizeof(p->data));
    EXPECT_EQ(p->data[0], 0xFF);
    EXPECT_EQ(p->data[4095], 0xFF);
    pool.deallocate(p);
}
