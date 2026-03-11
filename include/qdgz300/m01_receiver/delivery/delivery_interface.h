#ifndef RECEIVER_DELIVERY_DELIVERY_INTERFACE_H
#define RECEIVER_DELIVERY_DELIVERY_INTERFACE_H

#include "qdgz300/m01_receiver/pipeline/reorderer.h"
#include <functional>
#include <memory>
#include <string>

namespace receiver
{
    namespace delivery
    {

        /**
         * @brief 统一交付接口
         *
         * 职责：
         * - 提供有序数据包交付抽象
         * - 支持刷新与统计查询
         */
        class DeliveryInterface
        {
        public:
            /**
             * @brief 虚析构函数
             */
            virtual ~DeliveryInterface() = default;

            /**
             * @brief 交付一个有序数据包
             * @param packet 有序数据包
             * @return 交付成功返回true
             */
            virtual bool deliver(const pipeline::OrderedPacket &packet) = 0;

            /**
             * @brief 刷新交付缓冲
             * @return void
             */
            virtual void flush() = 0;

            struct Statistics
            {
                uint64_t delivered_packets{0};
                uint64_t delivery_errors{0};
                uint64_t bytes_delivered{0};
            };

            /**
             * @brief 获取交付统计
             * @return 当前统计快照
             */
            virtual Statistics get_statistics() const = 0;
        };

        /**
         * @brief 回调式交付实现
         */
        class CallbackDelivery : public DeliveryInterface
        {
        public:
            using Callback = std::function<void(const pipeline::OrderedPacket &)>;

            /**
             * @brief 构造回调交付器
             * @param callback 交付回调
             */
            explicit CallbackDelivery(Callback callback);

            /**
             * @brief 析构回调交付器
             */
            ~CallbackDelivery() override = default;

            /**
             * @brief 通过回调交付数据包
             * @param packet 有序数据包
             * @return 交付成功返回true
             */
            bool deliver(const pipeline::OrderedPacket &packet) override;

            /**
             * @brief 刷新回调交付器
             * @return void
             */
            void flush() override;

            /**
             * @brief 获取统计信息
             * @return 当前统计快照
             */
            Statistics get_statistics() const override;

        private:
            Callback callback_;
            Statistics stats_;
        };

        /**
         * @brief 共享内存交付实现
         */
        class SharedMemoryDelivery : public DeliveryInterface
        {
        public:
            /**
             * @brief 构造共享内存交付器
             * @param shm_name 共享内存名称
             * @param shm_size 共享内存大小（字节）
             */
            SharedMemoryDelivery(const std::string &shm_name, size_t shm_size);

            /**
             * @brief 析构共享内存交付器
             */
            ~SharedMemoryDelivery() override;

            /**
             * @brief 写入共享内存交付数据包
             * @param packet 有序数据包
             * @return 交付成功返回true
             */
            bool deliver(const pipeline::OrderedPacket &packet) override;

            /**
             * @brief 刷新共享内存交付状态
             * @return void
             */
            void flush() override;

            /**
             * @brief 获取统计信息
             * @return 当前统计快照
             */
            Statistics get_statistics() const override;

        private:
            class Impl;
            std::unique_ptr<Impl> impl_;
        };

        /**
         * @brief Unix Socket 交付实现
         */
        class UnixSocketDelivery : public DeliveryInterface
        {
        public:
            /**
             * @brief 构造Unix Socket交付器
             * @param socket_path Unix Socket路径
             * @param reconnect_interval_ms 重连间隔（毫秒）
             */
            explicit UnixSocketDelivery(const std::string &socket_path, uint32_t reconnect_interval_ms = 100);

            /**
             * @brief 析构Unix Socket交付器
             */
            ~UnixSocketDelivery() override;

            /**
             * @brief 通过Unix Socket交付数据包
             * @param packet 有序数据包
             * @return 交付成功返回true
             */
            bool deliver(const pipeline::OrderedPacket &packet) override;

            /**
             * @brief 刷新Socket交付缓冲
             * @return void
             */
            void flush() override;

            /**
             * @brief 获取统计信息
             * @return 当前统计快照
             */
            Statistics get_statistics() const override;

        private:
            class Impl;
            std::unique_ptr<Impl> impl_;
        };

    } // namespace delivery
} // namespace receiver

#endif // RECEIVER_DELIVERY_DELIVERY_INTERFACE_H
