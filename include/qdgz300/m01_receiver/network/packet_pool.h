#ifndef RECEIVER_NETWORK_PACKET_POOL_H
#define RECEIVER_NETWORK_PACKET_POOL_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace receiver
{
    namespace network
    {

        /**
         * @brief 数据包内存池（避免频繁malloc/free）
         *
         * 职责：
         * - 预分配固定大小的数据包缓冲区
         * - 提供快速的分配/回收接口
         * - 线程安全（CAS无锁自由链表）
         * - 统计池使用情况
         */
        class PacketPool
        {
        public:
            /**
             * @brief 构造函数
             * @param packet_size 单个数据包缓冲区大小（字节）
             * @param pool_size 池中预分配的缓冲区数量
             * @param numa_node NUMA节点（默认Node1）
             */
            explicit PacketPool(size_t packet_size, size_t pool_size, int numa_node = 1);
            ~PacketPool();

            // 禁用拷贝和移动
            PacketPool(const PacketPool &) = delete;
            PacketPool &operator=(const PacketPool &) = delete;

            /**
             * @brief 从池中分配一个数据包缓冲区
             * @return 缓冲区指针（池为空时返回nullptr或动态分配）
             */
            uint8_t *allocate();

            /**
             * @brief 回收数据包缓冲区到池中
             * @param buffer 要回收的缓冲区指针
             */
            void deallocate(uint8_t *buffer);

            /**
             * @brief 获取池的使用统计
             */
            struct Statistics
            {
                size_t total_size;     // 池总大小
                size_t available;      // 当前可用数量
                size_t allocated;      // 当前已分配数量
                size_t alloc_count;    // 累计分配次数
                size_t dealloc_count;  // 累计回收次数
                size_t fallback_alloc; // 池耗尽时动态分配次数
            };

            Statistics get_statistics() const;

        private:
            size_t packet_size_;
            size_t pool_size_;

            // 实现细节（自由链表、内存块管理）在源文件中定义
            class Impl;
            std::unique_ptr<Impl> impl_;
        };

        /**
         * @brief 数据包缓冲区智能指针（自动回收到池）
         */
        class PacketBuffer
        {
        public:
            PacketBuffer() : buffer_(nullptr), pool_(nullptr) {}

            PacketBuffer(uint8_t *buffer, PacketPool *pool)
                : buffer_(buffer), pool_(pool) {}

            ~PacketBuffer()
            {
                if (buffer_ && pool_)
                {
                    pool_->deallocate(buffer_);
                }
            }

            // 禁用拷贝，允许移动
            PacketBuffer(const PacketBuffer &) = delete;
            PacketBuffer &operator=(const PacketBuffer &) = delete;

            PacketBuffer(PacketBuffer &&other) noexcept
                : buffer_(other.buffer_), pool_(other.pool_)
            {
                other.buffer_ = nullptr;
                other.pool_ = nullptr;
            }

            PacketBuffer &operator=(PacketBuffer &&other) noexcept
            {
                if (this != &other)
                {
                    if (buffer_ && pool_)
                    {
                        pool_->deallocate(buffer_);
                    }
                    buffer_ = other.buffer_;
                    pool_ = other.pool_;
                    other.buffer_ = nullptr;
                    other.pool_ = nullptr;
                }
                return *this;
            }

            uint8_t *get() { return buffer_; }
            const uint8_t *get() const { return buffer_; }

            uint8_t *release()
            {
                uint8_t *ptr = buffer_;
                buffer_ = nullptr;
                return ptr;
            }

        private:
            uint8_t *buffer_;
            PacketPool *pool_;
        };

    } // namespace network
} // namespace receiver

#endif // RECEIVER_NETWORK_PACKET_POOL_H
