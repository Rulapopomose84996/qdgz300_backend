#ifndef RECEIVER_DELIVERY_SPSC_QUEUE_H
#define RECEIVER_DELIVERY_SPSC_QUEUE_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace receiver
{
    namespace delivery
    {
        /**
         * @brief Single Producer Single Consumer Lock-free Ring Buffer
         *
         * M01 §7 队列拓扑规范
         * 特性：
         * - Lock-free：使用std::atomic + memory_order_release/acquire
         * - Cache-line对齐：alignas(64)防止false sharing
         * - Drop-oldest溢出策略：队列满时丢弃最旧元素
         * - 零拷贝：存储智能指针，避免大对象拷贝
         *
         * @tparam T 队列元素类型（通常为智能指针）
         */
        template <typename T>
        class SpscQueue
        {
        public:
            /**
             * @brief 构造SPSC队列
             * @param capacity 队列容量（必须 > 0）
             */
            explicit SpscQueue(size_t capacity)
                : capacity_(capacity > 0 ? capacity : 1),
                  buffer_(capacity_ + 1) // +1 用于区分空/满状态
            {
            }

            /**
             * @brief 禁止拷贝
             */
            SpscQueue(const SpscQueue &) = delete;
            SpscQueue &operator=(const SpscQueue &) = delete;

            /**
             * @brief 生产者：推入元素（移动语义）
             *
             * @param item 要推入的元素
             * @return true if成功推入, false if队列已满且执行了drop-oldest
             */
            bool push(T &&item)
            {
                const size_t current_head = head_.load(std::memory_order_relaxed);
                const size_t next_head = (current_head + 1) % buffer_.size();
                const size_t current_tail = tail_.load(std::memory_order_acquire);

                if (next_head == current_tail)
                {
                    // 队列已满，执行drop-oldest策略
                    const size_t next_tail = (current_tail + 1) % buffer_.size();
                    tail_.store(next_tail, std::memory_order_release);
                    dropped_count_.fetch_add(1, std::memory_order_relaxed);
                }

                buffer_[current_head] = std::move(item);
                head_.store(next_head, std::memory_order_release);
                return true;
            }

            /**
             * @brief 消费者：弹出元素
             *
             * @param item 输出参数，存储弹出的元素
             * @return true if成功弹出, false if队列为空
             */
            bool pop(T &item)
            {
                const size_t current_tail = tail_.load(std::memory_order_relaxed);
                const size_t current_head = head_.load(std::memory_order_acquire);

                if (current_tail == current_head)
                {
                    // 队列为空
                    return false;
                }

                item = std::move(buffer_[current_tail]);
                const size_t next_tail = (current_tail + 1) % buffer_.size();
                tail_.store(next_tail, std::memory_order_release);
                return true;
            }

            /**
             * @brief 检查队列是否为空
             */
            bool empty() const
            {
                const size_t current_tail = tail_.load(std::memory_order_acquire);
                const size_t current_head = head_.load(std::memory_order_acquire);
                return current_tail == current_head;
            }

            /**
             * @brief 获取当前队列深度（近似值）
             */
            size_t size() const
            {
                const size_t current_tail = tail_.load(std::memory_order_acquire);
                const size_t current_head = head_.load(std::memory_order_acquire);
                if (current_head >= current_tail)
                {
                    return current_head - current_tail;
                }
                return buffer_.size() - current_tail + current_head;
            }

            /**
             * @brief 获取队列容量
             */
            size_t capacity() const { return capacity_; }

            /**
             * @brief 获取丢弃计数（drop-oldest策略触发次数）
             */
            uint64_t dropped_count() const
            {
                return dropped_count_.load(std::memory_order_relaxed);
            }

            /**
             * @brief 重置丢弃计数
             */
            void reset_dropped_count()
            {
                dropped_count_.store(0, std::memory_order_relaxed);
            }

        private:
            const size_t capacity_;
            std::vector<T> buffer_;

            // Cache-line对齐，防止false sharing
            alignas(64) std::atomic<size_t> head_{0};            // Producer写入位置
            alignas(64) std::atomic<size_t> tail_{0};            // Consumer读取位置
            alignas(64) std::atomic<uint64_t> dropped_count_{0}; // 丢弃计数
        };

    } // namespace delivery
} // namespace receiver

#endif // RECEIVER_DELIVERY_SPSC_QUEUE_H
