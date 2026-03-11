#include "qdgz300/m01_receiver/pipeline/reorderer.h"
#include "qdgz300/m01_receiver/protocol/protocol_types.h"

#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

using receiver::pipeline::OrderedPacket;
using receiver::pipeline::ReorderConfig;
using receiver::pipeline::Reorderer;
using receiver::protocol::PacketType;
using receiver::protocol::ParsedPacket;
using receiver::protocol::PROTOCOL_MAGIC;
using receiver::protocol::PROTOCOL_VERSION;

namespace
{
    ParsedPacket make_data_packet(uint32_t seq)
    {
        ParsedPacket packet{};
        packet.header.magic = PROTOCOL_MAGIC;
        packet.header.protocol_version = PROTOCOL_VERSION;
        packet.header.packet_type = static_cast<uint8_t>(PacketType::DATA);
        packet.header.sequence_number = seq;
        packet.header.payload_len = 0;
        packet.payload = nullptr;
        packet.total_size = receiver::protocol::COMMON_HEADER_SIZE;
        return packet;
    }
} // namespace

TEST(ReordererTests, InOrderDelivery)
{
    std::vector<uint32_t> output;
    ReorderConfig config{};
    config.enable_zero_fill = true;
    Reorderer reorderer(config, [&](OrderedPacket &&pkt) { output.push_back(pkt.sequence_number); });

    reorderer.insert(make_data_packet(0));
    reorderer.insert(make_data_packet(1));
    reorderer.insert(make_data_packet(2));
    reorderer.insert(make_data_packet(3));

    ASSERT_EQ(output.size(), 4u);
    EXPECT_EQ(output[0], 0u);
    EXPECT_EQ(output[1], 1u);
    EXPECT_EQ(output[2], 2u);
    EXPECT_EQ(output[3], 3u);
}

TEST(ReordererTests, OutOfOrderReorder)
{
    std::vector<uint32_t> output;
    ReorderConfig config{};
    config.enable_zero_fill = true;
    Reorderer reorderer(config, [&](OrderedPacket &&pkt) { output.push_back(pkt.sequence_number); });

    reorderer.insert(make_data_packet(0));
    reorderer.insert(make_data_packet(2));
    reorderer.insert(make_data_packet(3));
    reorderer.insert(make_data_packet(1));

    ASSERT_EQ(output.size(), 4u);
    EXPECT_EQ(output[0], 0u);
    EXPECT_EQ(output[1], 1u);
    EXPECT_EQ(output[2], 2u);
    EXPECT_EQ(output[3], 3u);
}

TEST(ReordererTests, DuplicateSequence)
{
    std::vector<uint32_t> output;
    ReorderConfig config{};
    Reorderer reorderer(config, [&](OrderedPacket &&pkt) { output.push_back(pkt.sequence_number); });

    reorderer.insert(make_data_packet(0));
    reorderer.insert(make_data_packet(0));
    reorderer.insert(make_data_packet(1));

    ASSERT_EQ(output.size(), 2u);
    EXPECT_EQ(output[0], 0u);
    EXPECT_EQ(output[1], 1u);
    EXPECT_EQ(reorderer.get_statistics().packets_duplicate, 1u);
}

TEST(ReordererTests, GapWithTimeout)
{
    std::vector<OrderedPacket> output;
    ReorderConfig config{};
    config.timeout_ms = 1;
    config.enable_zero_fill = true;
    Reorderer reorderer(config, [&](OrderedPacket &&pkt) { output.push_back(std::move(pkt)); });

    reorderer.insert(make_data_packet(0));
    reorderer.insert(make_data_packet(2));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    reorderer.check_timeout();

    ASSERT_EQ(output.size(), 3u);
    EXPECT_EQ(output[0].sequence_number, 0u);
    EXPECT_FALSE(output[0].is_zero_filled);
    EXPECT_EQ(output[1].sequence_number, 1u);
    EXPECT_TRUE(output[1].is_zero_filled);
    EXPECT_EQ(output[2].sequence_number, 2u);
    EXPECT_FALSE(output[2].is_zero_filled);
}

TEST(ReordererTests, WraparoundBoundary)
{
    std::vector<uint32_t> output;
    ReorderConfig config{};
    config.enable_zero_fill = false;
    Reorderer reorderer(config, [&](OrderedPacket &&pkt) { output.push_back(pkt.sequence_number); });

    reorderer.insert(make_data_packet(0xFFFFFFFEu));
    reorderer.insert(make_data_packet(0xFFFFFFFFu));
    reorderer.insert(make_data_packet(0x00000000u));
    reorderer.insert(make_data_packet(0x00000001u));

    ASSERT_EQ(output.size(), 4u);
    EXPECT_EQ(output[0], 0xFFFFFFFEu);
    EXPECT_EQ(output[1], 0xFFFFFFFFu);
    EXPECT_EQ(output[2], 0x00000000u);
    EXPECT_EQ(output[3], 0x00000001u);
}

TEST(ReordererTests, WindowOverflow)
{
    std::vector<OrderedPacket> output;
    ReorderConfig config{};
    config.window_size = 4;
    config.timeout_ms = 1;
    config.enable_zero_fill = true;
    Reorderer reorderer(config, [&](OrderedPacket &&pkt) { output.push_back(std::move(pkt)); });

    reorderer.insert(make_data_packet(0));
    reorderer.insert(make_data_packet(100));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    reorderer.check_timeout();

    ASSERT_EQ(output.size(), 2u);
    EXPECT_EQ(output[0].sequence_number, 0u);
    EXPECT_EQ(output[1].sequence_number, 1u);
    EXPECT_TRUE(output[1].is_zero_filled);
    EXPECT_EQ(reorderer.get_statistics().packets_duplicate, 1u);
}

TEST(ReordererTests, FlushRemaining)
{
    std::vector<OrderedPacket> output;
    ReorderConfig config{};
    config.enable_zero_fill = true;
    Reorderer reorderer(config, [&](OrderedPacket &&pkt) { output.push_back(std::move(pkt)); });

    reorderer.insert(make_data_packet(0));
    reorderer.insert(make_data_packet(2));
    const size_t drained = reorderer.flush();

    ASSERT_EQ(output.size(), 2u);
    EXPECT_EQ(output[0].sequence_number, 0u);
    EXPECT_EQ(output[1].sequence_number, 2u);
    EXPECT_FALSE(output[1].is_zero_filled);
    EXPECT_EQ(drained, 1u);
}

TEST(ReordererTests, StatisticsAccuracy)
{
    std::vector<OrderedPacket> output;
    ReorderConfig config{};
    config.timeout_ms = 1;
    config.enable_zero_fill = true;
    Reorderer reorderer(config, [&](OrderedPacket &&pkt) { output.push_back(std::move(pkt)); });

    reorderer.insert(make_data_packet(0));
    reorderer.insert(make_data_packet(2));
    reorderer.insert(make_data_packet(2));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    reorderer.check_timeout();

    const auto stats = reorderer.get_statistics();
    EXPECT_EQ(stats.packets_in_order, 2u);
    EXPECT_EQ(stats.packets_out_of_order, 1u);
    EXPECT_EQ(stats.packets_duplicate, 1u);
    EXPECT_EQ(stats.packets_zero_filled, 1u);
}
