// include/qdgz300/common/metrics.h
// 原子指标基类（AtomicCounter / AtomicGauge）
#pragma once
#include <atomic>
#include <cstdint>
#include <string_view>

namespace qdgz300
{

    /// 原子计数器（单调递增）
    class AtomicCounter
    {
    public:
        explicit AtomicCounter(std::string_view name = "") noexcept
            : name_(name), value_{0} {}

        void increment(uint64_t delta = 1) noexcept
        {
            value_.fetch_add(delta, std::memory_order_relaxed);
        }

        uint64_t get() const noexcept
        {
            return value_.load(std::memory_order_relaxed);
        }

        void reset() noexcept
        {
            value_.store(0, std::memory_order_relaxed);
        }

        std::string_view name() const noexcept { return name_; }

    private:
        std::string_view name_;
        std::atomic<uint64_t> value_;
    };

    /// 原子仪表盘（可增可减）
    class AtomicGauge
    {
    public:
        explicit AtomicGauge(std::string_view name = "") noexcept
            : name_(name), value_{0} {}

        void set(int64_t val) noexcept
        {
            value_.store(val, std::memory_order_relaxed);
        }

        void increment(int64_t delta = 1) noexcept
        {
            value_.fetch_add(delta, std::memory_order_relaxed);
        }

        void decrement(int64_t delta = 1) noexcept
        {
            value_.fetch_sub(delta, std::memory_order_relaxed);
        }

        int64_t get() const noexcept
        {
            return value_.load(std::memory_order_relaxed);
        }

        std::string_view name() const noexcept { return name_; }

    private:
        std::string_view name_;
        std::atomic<int64_t> value_;
    };

} // namespace qdgz300
