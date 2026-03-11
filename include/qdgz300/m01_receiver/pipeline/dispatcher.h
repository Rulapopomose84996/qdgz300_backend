#ifndef RECEIVER_PIPELINE_DISPATCHER_H
#define RECEIVER_PIPELINE_DISPATCHER_H

#include "qdgz300/m01_receiver/protocol/protocol_types.h"
#include "qdgz300/m01_receiver/network/packet_pool.h"
#include <atomic>
#include <deque>
#include <functional>
#include <vector>

namespace receiver
{
    namespace pipeline
    {

        /**
         * @brief 报文类型分发器
         *
         * 职责：
         * - 根据PacketType路由数据包到不同处理流程
         * - MVP范围：仅处理PacketType=0x03（数据包）和0x04（心跳），其他类型静默丢弃并计数
         * - 统计有效数据/心跳与丢弃数量
         */
        class Dispatcher
        {
        public:
            using DataPacketHandler = std::function<void(const protocol::ParsedPacket &)>;
            using PacketHandler = std::function<void(const protocol::ParsedPacket &)>;
            using ZeroCopyDataHandler = std::function<void(const protocol::ParsedPacket &, network::PacketBuffer &&)>;

            /**
             * @brief 构造函数
             * @param data_handler 数据包（0x03）处理回调
             */
            explicit Dispatcher(DataPacketHandler data_handler);
            Dispatcher(DataPacketHandler data_handler, PacketHandler heartbeat_handler);

            /**
             * @brief 析构分发器
             */
            ~Dispatcher() = default;

            /**
             * @brief 分发数据包
             * @param packet 已解析并校验的数据包
             * @return void
             */
            void dispatch(const protocol::ParsedPacket &packet);
            void dispatch(const protocol::ParsedPacket &packet, network::PacketBuffer &&buffer);
            void dispatch_with_priority(const protocol::ParsedPacket &packet);

            /**
             * @brief 批量分发数据包
             * @param packets 已解析并校验的数据包集合
             * @return void
             */
            void dispatch_batch(const std::vector<protocol::ParsedPacket> &packets);
            void set_heartbeat_max_queue_depth(size_t depth) { heartbeat_max_queue_depth_ = depth == 0 ? 1 : depth; }
            void set_zero_copy_data_handler(ZeroCopyDataHandler handler) { zero_copy_data_handler_ = std::move(handler); }

            /**
             * @brief 统计信息
             */
            struct Statistics
            {
                std::atomic<uint64_t> data_packets{0};      // 0x03数据包（处理）
                std::atomic<uint64_t> heartbeat_packets{0}; // 0x04心跳包（处理）
                std::atomic<uint64_t> dropped_packets{0};   // 非支持类型或校验失败
            };

            struct HeartbeatStatistics
            {
                std::atomic<uint64_t> received_total{0};
                std::atomic<uint64_t> loss_total{0};
                std::atomic<uint64_t> last_seen_ms{0};
                std::atomic<uint64_t> queue_priority_hits{0};
                std::atomic<uint64_t> starved_total{0};
            };

            /**
             * @brief 获取分发统计信息
             * @return 当前统计信息的只读引用
             */
            const Statistics &get_statistics() const { return stats_; }
            const HeartbeatStatistics &get_heartbeat_statistics() const { return heartbeat_stats_; }

        private:
            struct QueuedPacket
            {
                protocol::PacketType type;
                protocol::CommonHeader header;
                std::vector<uint8_t> payload;
                size_t total_size{0};
            };

            DataPacketHandler data_handler_;
            ZeroCopyDataHandler zero_copy_data_handler_;
            PacketHandler heartbeat_handler_;
            Statistics stats_;
            HeartbeatStatistics heartbeat_stats_;
            std::deque<QueuedPacket> high_priority_queue_;
            std::deque<QueuedPacket> normal_queue_;
            size_t heartbeat_max_queue_depth_{1000};
            bool has_last_heartbeat_seq_{false};
            uint32_t last_heartbeat_seq_{0};

            /**
             * @brief 处理非数据包（静默丢弃+计数）
             * @param type 报文类型
             */
            void handle_non_data_packet(protocol::PacketType type);
            void enqueue_packet(const protocol::ParsedPacket &packet);
            void drain_queues();
            void dispatch_queued_packet(const QueuedPacket &packet);
            void dispatch_heartbeat_packet(const QueuedPacket &queued);
            void dispatch_heartbeat_direct(const protocol::ParsedPacket &packet);
        };

    } // namespace pipeline
} // namespace receiver

#endif // RECEIVER_PIPELINE_DISPATCHER_H
