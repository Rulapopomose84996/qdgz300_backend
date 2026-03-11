// include/qdgz300/common/memory_pool.h
// NUMA-Aware 内存池模板
// - 预分配固定数量的 T 对象
// - 使用 numa_alloc_onnode() 在指定 NUMA node 分配（fallback mmap/malloc）
// - CAS 无锁自由链表管理空闲块
// - 运行期 O(1) allocate/deallocate，禁止动态扩展
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <new>
#include "qdgz300/common/numa_utils.h"

namespace qdgz300
{

    template <typename T>
    class NUMAPool
    {
    public:
        /// @param count 预分配的 T 对象数量
        /// @param numa_node 目标 NUMA 节点（数据面默认 Node1）
        explicit NUMAPool(size_t count, int numa_node = 1)
            : total_count_(count), numa_node_(numa_node)
        {
            // 每个 slot 需要容纳 T 对象或 FreeNode（取较大者）
            static_assert(alignof(T) <= 64, "T alignment must <= 64");
            constexpr size_t slot_size = sizeof(T) > sizeof(FreeNode) ? sizeof(T) : sizeof(FreeNode);
            // 确保 slot 对齐到 T 的对齐要求和 FreeNode 的对齐要求
            constexpr size_t slot_align = alignof(T) > alignof(FreeNode) ? alignof(T) : alignof(FreeNode);
            constexpr size_t aligned_slot = (slot_size + slot_align - 1) & ~(slot_align - 1);
            slot_size_ = aligned_slot;

            raw_size_ = aligned_slot * count;
            if (raw_size_ == 0)
            {
                return;
            }

            // NUMA 感知分配（内部自动 fallback 到 malloc）
            raw_memory_ = qdgz300::numa_alloc(raw_size_, numa_node);
            if (!raw_memory_)
            {
                total_count_ = 0;
                return;
            }

            // 初始化自由链表：逆序串联，使得首次分配从低地址开始
            std::memset(raw_memory_, 0, raw_size_);
            FreeNode *prev = nullptr;
            for (size_t i = 0; i < count; ++i)
            {
                auto *node = reinterpret_cast<FreeNode *>(
                    static_cast<uint8_t *>(raw_memory_) + i * aligned_slot);
                node->next.store(prev, std::memory_order_relaxed);
                prev = node;
            }
            free_head_.store(prev, std::memory_order_release);
            available_.store(count, std::memory_order_relaxed);
        }

        ~NUMAPool()
        {
            if (raw_memory_)
            {
                qdgz300::numa_free(raw_memory_, raw_size_);
                raw_memory_ = nullptr;
            }
        }

        NUMAPool(const NUMAPool &) = delete;
        NUMAPool &operator=(const NUMAPool &) = delete;

        /// 分配一个 T 大小的内存块（O(1)，CAS 无锁）
        /// @return 指向可用内存的指针，池耗尽时返回 nullptr
        T *allocate() noexcept
        {
            FreeNode *head = free_head_.load(std::memory_order_acquire);
            while (head)
            {
                FreeNode *next = head->next.load(std::memory_order_relaxed);
                if (free_head_.compare_exchange_weak(head, next,
                                                     std::memory_order_acq_rel,
                                                     std::memory_order_acquire))
                {
                    available_.fetch_sub(1, std::memory_order_relaxed);
                    return reinterpret_cast<T *>(head);
                }
                // CAS 失败，head 已被更新，继续重试
            }
            return nullptr; // 池耗尽
        }

        /// 归还一个 T 对象的内存到池中（O(1)，CAS 无锁）
        /// @warning ptr 必须来自本池的 allocate()，否则行为未定义
        void deallocate(T *ptr) noexcept
        {
            if (!ptr)
                return;
            auto *node = reinterpret_cast<FreeNode *>(ptr);
            FreeNode *old_head = free_head_.load(std::memory_order_relaxed);
            do
            {
                node->next.store(old_head, std::memory_order_relaxed);
            } while (!free_head_.compare_exchange_weak(old_head, node,
                                                       std::memory_order_release,
                                                       std::memory_order_relaxed));
            available_.fetch_add(1, std::memory_order_relaxed);
        }

        /// 当前可用（空闲）对象数量
        size_t available() const noexcept
        {
            return available_.load(std::memory_order_relaxed);
        }

        /// 池总容量
        size_t total() const noexcept { return total_count_; }

        /// 目标 NUMA 节点
        int numa_node() const noexcept { return numa_node_; }

        /// 检查指针是否属于本池
        bool owns(const T *ptr) const noexcept
        {
            auto addr = reinterpret_cast<uintptr_t>(ptr);
            auto base = reinterpret_cast<uintptr_t>(raw_memory_);
            return addr >= base && addr < base + raw_size_;
        }

    private:
        struct FreeNode
        {
            std::atomic<FreeNode *> next;
        };

        void *raw_memory_{nullptr};
        size_t raw_size_{0};
        size_t slot_size_{0};
        size_t total_count_{0};
        int numa_node_{1};
        alignas(64) std::atomic<FreeNode *> free_head_{nullptr};
        alignas(64) std::atomic<size_t> available_{0};
    };

} // namespace qdgz300
