#include "qdgz300/m01_receiver/pipeline/dispatcher.h"
#include "qdgz300/m01_receiver/protocol/payload_codec.h"
#include "qdgz300/m01_receiver/protocol/protocol_types.h"

#include <chrono>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

using receiver::pipeline::Dispatcher;
using receiver::protocol::COMMON_HEADER_SIZE;
using receiver::protocol::PacketType;
using receiver::protocol::ParsedPacket;

namespace
{
    ParsedPacket make_packet(uint32_t seq, PacketType type)
    {
        static std::vector<uint8_t> data_payload_storage(8, 0xAB);
        static std::vector<uint8_t> heartbeat_payload_storage = []()
        {
            receiver::protocol::HeartbeatPayload heartbeat{};
            heartbeat.system_status_alive = 1;
            heartbeat.system_status_state = 2;
            heartbeat.op_mode = 3;
            heartbeat.core_temp = 25;
            std::vector<uint8_t> payload;
            receiver::protocol::PayloadCodec::encode_heartbeat_payload(heartbeat, payload);
            return payload;
        }();

        ParsedPacket packet{};
        packet.header.magic = receiver::protocol::PROTOCOL_MAGIC;
        packet.header.sequence_number = seq;
        packet.header.packet_type = static_cast<uint8_t>(type);
        packet.header.protocol_version = receiver::protocol::PROTOCOL_VERSION;
        if (type == PacketType::HEARTBEAT)
        {
            packet.header.payload_len = static_cast<uint16_t>(heartbeat_payload_storage.size());
            packet.payload = heartbeat_payload_storage.data();
            packet.total_size = COMMON_HEADER_SIZE + heartbeat_payload_storage.size();
        }
        else
        {
            packet.header.payload_len = static_cast<uint16_t>(data_payload_storage.size());
            packet.payload = data_payload_storage.data();
            packet.total_size = COMMON_HEADER_SIZE + data_payload_storage.size();
        }
        return packet;
    }
} // namespace

TEST(QosPriorityTests, QosRoutingHeartbeatBeforeData)
{
    std::vector<char> order;
    Dispatcher dispatcher(
        [&](const ParsedPacket &) { order.push_back('D'); },
        nullptr,
        nullptr,
        [&](const ParsedPacket &) { order.push_back('H'); },
        nullptr);

    std::vector<ParsedPacket> batch;
    batch.push_back(make_packet(1, PacketType::DATA));
    batch.push_back(make_packet(2, PacketType::DATA));
    batch.push_back(make_packet(3, PacketType::HEARTBEAT));
    batch.push_back(make_packet(4, PacketType::DATA));
    dispatcher.dispatch_batch(batch);

    ASSERT_EQ(order.size(), 4u);
    EXPECT_EQ(order.front(), 'H');
}

TEST(QosPriorityTests, HeartbeatNotStarvedUnderDataFlood)
{
    uint32_t heartbeat_count = 0;
    uint32_t data_count = 0;
    Dispatcher dispatcher(
        [&](const ParsedPacket &) { ++data_count; },
        nullptr,
        nullptr,
        [&](const ParsedPacket &) { ++heartbeat_count; },
        nullptr);

    std::vector<ParsedPacket> batch;
    for (uint32_t i = 0; i < 2000; ++i)
    {
        batch.push_back(make_packet(i + 1, PacketType::DATA));
    }
    batch.push_back(make_packet(5000, PacketType::HEARTBEAT));
    dispatcher.dispatch_batch(batch);

    EXPECT_EQ(heartbeat_count, 1u);
    EXPECT_EQ(data_count, 2000u);
}

TEST(QosPriorityTests, HeartbeatLatencyUnder1msSimulated)
{
    std::chrono::steady_clock::time_point hb_time{};
    Dispatcher dispatcher(
        [&](const ParsedPacket &) {},
        nullptr,
        nullptr,
        [&](const ParsedPacket &)
        { hb_time = std::chrono::steady_clock::now(); },
        nullptr);

    const auto start = std::chrono::steady_clock::now();
    dispatcher.dispatch_with_priority(make_packet(1, PacketType::HEARTBEAT));
    const auto end = hb_time;

    const auto latency_us =
        std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    EXPECT_LT(latency_us, 1000);
}

TEST(QosPriorityTests, DataThroughputMaintains200kppsWithQosEnabled)
{
    uint32_t data_count = 0;
    Dispatcher dispatcher(
        [&](const ParsedPacket &) { ++data_count; },
        nullptr,
        nullptr,
        [&](const ParsedPacket &) {},
        nullptr);

    std::vector<ParsedPacket> batch;
    batch.reserve(200000 + 1000);
    for (uint32_t i = 0; i < 200000; ++i)
    {
        batch.push_back(make_packet(i + 1, PacketType::DATA));
        if ((i % 200) == 0)
        {
            batch.push_back(make_packet(1000000 + i, PacketType::HEARTBEAT));
        }
    }

    const auto begin = std::chrono::steady_clock::now();
    dispatcher.dispatch_batch(batch);
    const auto end = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration_cast<std::chrono::duration<double>>(end - begin).count();
    const double pps = (seconds > 0.0) ? (static_cast<double>(data_count) / seconds) : 0.0;

    EXPECT_EQ(data_count, 200000u);
    EXPECT_GT(pps, 200000.0);
}
