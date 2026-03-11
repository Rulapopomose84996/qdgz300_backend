// include/qdgz300/common/spsc_queue.h
// SPSC Lock-free Ring Buffer — 数据面核心队列
// alignas(64) 头尾指针防止 false sharing
// memory_order_release/acquire 保证跨线程可见性
// 溢出策略: drop-oldest（覆盖最旧数据 + 计数）
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>

namespace qdgz300
{

    template <typename T, size_t Cap>
    class SPSCQueue
    {
        static_assert(Cap > 0 && (Cap & (Cap - 1)) == 0,
                      "Capacity must be power of 2");

    public:
        SPSCQueue() noexcept : head_{0}, tail_{0}, drop_count_{0} {}

        bool try_push(const T &item) noexcept
        {
            const size_t tail = tail_.load(std::memory_order_relaxed);
            const size_t next = (tail + 1) & mask_;
            if (next == head_.load(std::memory_order_acquire))
            {
                return false; // full
            }
            buffer_[tail] = item;
            tail_.store(next, std::memory_order_release);
            return true;
        }

        bool try_push(T &&item) noexcept
        {
            const size_t tail = tail_.load(std::memory_order_relaxed);
            const size_t next = (tail + 1) & mask_;
            if (next == head_.load(std::memory_order_acquire))
            {
                return false;
            }
            buffer_[tail] = std::move(item);
            tail_.store(next, std::memory_order_release);
            return true;
        }

        /// @return true=直接入队成功, false=丢弃最旧元素后入队
        bool drop_oldest_push(const T &item) noexcept
        {
            bool dropped = false;
            for (;;)
            {
                const size_t tail = tail_.load(std::memory_order_relaxed);
                const size_t next = (tail + 1) & mask_;
                size_t head = head_.load(std::memory_order_acquire);

                if (next != head)
                {
                    buffer_[tail] = item;
                    tail_.store(next, std::memory_order_release);
                    return !dropped;
                }

                const size_t new_head = (head + 1) & mask_;
                if (head_.compare_exchange_weak(head, new_head,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire))
                {
                    drop_count_.fetch_add(1, std::memory_order_relaxed);
                    dropped = true;
                }
            }
        }

        /// @return true=直接入队成功, false=丢弃最旧元素后入队
        bool drop_oldest_push(T &&item) noexcept
        {
            bool dropped = false;
            for (;;)
            {
                const size_t tail = tail_.load(std::memory_order_relaxed);
                const size_t next = (tail + 1) & mask_;
                size_t head = head_.load(std::memory_order_acquire);

                if (next != head)
                {
                    buffer_[tail] = std::move(item);
                    tail_.store(next, std::memory_order_release);
                    return !dropped;
                }

                const size_t new_head = (head + 1) & mask_;
                if (head_.compare_exchange_weak(head, new_head,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire))
                {
                    drop_count_.fetch_add(1, std::memory_order_relaxed);
                    dropped = true;
                }
            }
        }

        std::optional<T> try_pop() noexcept
        {
            const size_t head = head_.load(std::memory_order_relaxed);
            if (head == tail_.load(std::memory_order_acquire))
            {
                return std::nullopt; // empty
            }
            T item = std::move(buffer_[head]);
            head_.store((head + 1) & mask_, std::memory_order_release);
            return std::move(item);
        }

        size_t size() const noexcept
        {
            const size_t tail = tail_.load(std::memory_order_acquire);
            const size_t head = head_.load(std::memory_order_acquire);
            return (tail - head) & mask_;
        }

        bool empty() const noexcept
        {
            return head_.load(std::memory_order_acquire) ==
                   tail_.load(std::memory_order_acquire);
        }

        bool full() const noexcept
        {
            const size_t tail = tail_.load(std::memory_order_relaxed);
            const size_t next = (tail + 1) & mask_;
            return next == head_.load(std::memory_order_acquire);
        }

        static constexpr size_t capacity() noexcept { return Cap; }

        uint64_t drop_count() const noexcept
        {
            return drop_count_.load(std::memory_order_relaxed);
        }

        void reset() noexcept
        {
            head_.store(0, std::memory_order_relaxed);
            tail_.store(0, std::memory_order_relaxed);
            drop_count_.store(0, std::memory_order_relaxed);
        }

    private:
        alignas(64) std::atomic<size_t> head_;
        alignas(64) std::atomic<size_t> tail_;
        alignas(64) std::atomic<uint64_t> drop_count_;
        T buffer_[Cap]; // Cap 个槽，可存 Cap-1 个元素

        static constexpr size_t mask_ = Cap - 1;

        // usable capacity = Cap - 1 (one slot reserved to distinguish full vs empty)
    public:
        static constexpr size_t usable_capacity() noexcept { return Cap - 1; }
    };

} // namespace qdgz300
