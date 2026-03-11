#include "qdgz300/m01_receiver/pipeline/dispatcher.h"
#include "qdgz300/m01_receiver/monitoring/metrics.h"
#include "qdgz300/m01_receiver/protocol/crc32c.h"

#include <chrono>
#include <cstring>
#include <utility>

namespace receiver
{
    namespace pipeline
    {
        namespace
        {
            uint32_t read_u32_le(const uint8_t *ptr)
            {
                uint32_t value = 0;
                std::memcpy(&value, ptr, sizeof(value));
                return value;
            }

            uint64_t now_ms_epoch()
            {
                return static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count());
            }
        } // namespace

        Dispatcher::Dispatcher(DataPacketHandler data_handler)
            : data_handler_(std::move(data_handler))
        {
        }

        Dispatcher::Dispatcher(DataPacketHandler data_handler, PacketHandler heartbeat_handler)
            : data_handler_(std::move(data_handler)),
              heartbeat_handler_(std::move(heartbeat_handler))
        {
        }

        void Dispatcher::dispatch(const protocol::ParsedPacket &packet)
        {
            // P1-9 优化：同步路径下直接路由到 handler，消除 enqueue/drain 中间层和 payload 拷贝
            const protocol::PacketType type = packet.header.get_packet_type();

            if (type == protocol::PacketType::DATA)
            {
                stats_.data_packets.fetch_add(1, std::memory_order_relaxed);
                if (data_handler_)
                {
                    data_handler_(packet);
                }
                return;
            }

            if (type == protocol::PacketType::HEARTBEAT)
            {
                dispatch_heartbeat_direct(packet);
                return;
            }

            handle_non_data_packet(type);
        }

        void Dispatcher::dispatch(const protocol::ParsedPacket &packet, network::PacketBuffer &&buffer)
        {
            const protocol::PacketType type = packet.header.get_packet_type();

            if (type == protocol::PacketType::DATA)
            {
                stats_.data_packets.fetch_add(1, std::memory_order_relaxed);
                if (zero_copy_data_handler_)
                {
                    zero_copy_data_handler_(packet, std::move(buffer));
                }
                else if (data_handler_)
                {
                    // 回退路径：buffer 仍为活引用（未被 move），packet.payload 仍然有效
                    data_handler_(packet);
                }
                return;
            }

            // 非 DATA 类型不需要 buffer 所有权，PacketBuffer 在此作用域结束自动归还
            if (type == protocol::PacketType::HEARTBEAT)
            {
                dispatch_heartbeat_direct(packet);
                return;
            }

            handle_non_data_packet(type);
        }

        void Dispatcher::dispatch_with_priority(const protocol::ParsedPacket &packet)
        {
            dispatch(packet);
        }

        void Dispatcher::dispatch_batch(const std::vector<protocol::ParsedPacket> &packets)
        {
            for (const auto &packet : packets)
            {
                dispatch(packet);
            }
        }

        void Dispatcher::enqueue_packet(const protocol::ParsedPacket &packet)
        {
            const protocol::PacketType type = packet.header.get_packet_type();
            if (type != protocol::PacketType::DATA && type != protocol::PacketType::HEARTBEAT)
            {
                handle_non_data_packet(type);
                return;
            }

            QueuedPacket queued{};
            queued.type = type;
            queued.header = packet.header;
            queued.total_size = packet.total_size;

            const size_t payload_len = packet.header.payload_len;
            queued.payload.resize(payload_len);
            if (payload_len > 0 && packet.payload != nullptr)
            {
                std::memcpy(queued.payload.data(), packet.payload, payload_len);
            }

            if (queued.type == protocol::PacketType::HEARTBEAT)
            {
                while (high_priority_queue_.size() >= heartbeat_max_queue_depth_)
                {
                    high_priority_queue_.pop_front();
                    heartbeat_stats_.starved_total.fetch_add(1, std::memory_order_relaxed);
                    stats_.dropped_packets.fetch_add(1, std::memory_order_relaxed);
                }
                high_priority_queue_.push_back(std::move(queued));
                monitoring::MetricsCollector::instance().set_heartbeat_queue_depth(high_priority_queue_.size());
                return;
            }

            normal_queue_.push_back(std::move(queued));
        }

        void Dispatcher::drain_queues()
        {
            while (!high_priority_queue_.empty() || !normal_queue_.empty())
            {
                if (!high_priority_queue_.empty())
                {
                    if (!normal_queue_.empty())
                    {
                        heartbeat_stats_.queue_priority_hits.fetch_add(1, std::memory_order_relaxed);
                    }

                    QueuedPacket queued = std::move(high_priority_queue_.front());
                    high_priority_queue_.pop_front();
                    monitoring::MetricsCollector::instance().set_heartbeat_queue_depth(high_priority_queue_.size());
                    dispatch_queued_packet(queued);
                    continue;
                }

                QueuedPacket queued = std::move(normal_queue_.front());
                normal_queue_.pop_front();
                dispatch_queued_packet(queued);
            }
        }

        void Dispatcher::dispatch_queued_packet(const QueuedPacket &queued)
        {
            if (queued.type == protocol::PacketType::DATA)
            {
                stats_.data_packets.fetch_add(1, std::memory_order_relaxed);
                if (data_handler_)
                {
                    protocol::ParsedPacket packet{};
                    packet.header = queued.header;
                    packet.payload = queued.payload.empty() ? nullptr : queued.payload.data();
                    packet.total_size = queued.total_size;
                    data_handler_(packet);
                }
                return;
            }

            if (queued.type == protocol::PacketType::HEARTBEAT)
            {
                dispatch_heartbeat_packet(queued);
                return;
            }

            handle_non_data_packet(queued.type);
        }

        void Dispatcher::dispatch_heartbeat_packet(const QueuedPacket &queued)
        {
            constexpr size_t kHeartbeatDataLen = 44;
            constexpr size_t kHeartbeatPayloadLen = protocol::HEARTBEAT_PAYLOAD_SIZE;
            constexpr size_t kHeartbeatCrcOffset = 44;

            if (queued.payload.size() < kHeartbeatPayloadLen)
            {
                heartbeat_stats_.loss_total.fetch_add(1, std::memory_order_relaxed);
                stats_.dropped_packets.fetch_add(1, std::memory_order_relaxed);
                return;
            }

            const uint32_t wire_crc = read_u32_le(queued.payload.data() + kHeartbeatCrcOffset);
            const uint32_t calc_crc = protocol::crc32c(queued.payload.data(), kHeartbeatDataLen);
            if (wire_crc != calc_crc)
            {
                heartbeat_stats_.loss_total.fetch_add(1, std::memory_order_relaxed);
                stats_.dropped_packets.fetch_add(1, std::memory_order_relaxed);
                return;
            }

            if (has_last_heartbeat_seq_)
            {
                const uint32_t delta = queued.header.sequence_number - last_heartbeat_seq_;
                if (delta > 1 && delta < (1u << 31))
                {
                    heartbeat_stats_.loss_total.fetch_add(static_cast<uint64_t>(delta - 1), std::memory_order_relaxed);
                }
            }
            has_last_heartbeat_seq_ = true;
            last_heartbeat_seq_ = queued.header.sequence_number;

            heartbeat_stats_.received_total.fetch_add(1, std::memory_order_relaxed);
            heartbeat_stats_.last_seen_ms.store(now_ms_epoch(), std::memory_order_relaxed);
            stats_.heartbeat_packets.fetch_add(1, std::memory_order_relaxed);
            monitoring::MetricsCollector::instance().increment_heartbeat_packets_processed();

            if (!heartbeat_handler_)
            {
                return;
            }

            protocol::ParsedPacket packet{};
            packet.header = queued.header;
            packet.payload = queued.payload.data();
            packet.total_size = queued.total_size;
            heartbeat_handler_(packet);
        }

        void Dispatcher::dispatch_heartbeat_direct(const protocol::ParsedPacket &packet)
        {
            constexpr size_t kHeartbeatDataLen = 44;
            constexpr size_t kHeartbeatPayloadLen = protocol::HEARTBEAT_PAYLOAD_SIZE;
            constexpr size_t kHeartbeatCrcOffset = 44;

            if (packet.payload == nullptr || packet.header.payload_len < kHeartbeatPayloadLen)
            {
                heartbeat_stats_.loss_total.fetch_add(1, std::memory_order_relaxed);
                stats_.dropped_packets.fetch_add(1, std::memory_order_relaxed);
                return;
            }

            const uint32_t wire_crc = read_u32_le(packet.payload + kHeartbeatCrcOffset);
            const uint32_t calc_crc = protocol::crc32c(packet.payload, kHeartbeatDataLen);
            if (wire_crc != calc_crc)
            {
                heartbeat_stats_.loss_total.fetch_add(1, std::memory_order_relaxed);
                stats_.dropped_packets.fetch_add(1, std::memory_order_relaxed);
                return;
            }

            if (has_last_heartbeat_seq_)
            {
                const uint32_t delta = packet.header.sequence_number - last_heartbeat_seq_;
                if (delta > 1 && delta < (1u << 31))
                {
                    heartbeat_stats_.loss_total.fetch_add(static_cast<uint64_t>(delta - 1), std::memory_order_relaxed);
                }
            }
            has_last_heartbeat_seq_ = true;
            last_heartbeat_seq_ = packet.header.sequence_number;

            heartbeat_stats_.received_total.fetch_add(1, std::memory_order_relaxed);
            heartbeat_stats_.last_seen_ms.store(now_ms_epoch(), std::memory_order_relaxed);
            stats_.heartbeat_packets.fetch_add(1, std::memory_order_relaxed);
            monitoring::MetricsCollector::instance().increment_heartbeat_packets_processed();

            if (heartbeat_handler_)
            {
                heartbeat_handler_(packet);
            }
        }

        void Dispatcher::handle_non_data_packet(protocol::PacketType type)
        {
            (void)type;
            stats_.dropped_packets.fetch_add(1, std::memory_order_relaxed);
        }

    } // namespace pipeline
} // namespace receiver
