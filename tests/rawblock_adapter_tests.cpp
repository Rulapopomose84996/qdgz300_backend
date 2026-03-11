#include "qdgz300/m01_receiver/delivery/rawblock_adapter.h"
#include "qdgz300/m01_receiver/protocol/protocol_types.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

using qdgz300::m01::delivery::RawBlockAdapter;
using receiver::delivery::RAW_BLOCK_PAYLOAD_SIZE;
using receiver::delivery::RawBlockFlags;
using receiver::pipeline::OrderedPacket;
using receiver::protocol::PacketType;
using receiver::protocol::PROTOCOL_MAGIC;
using receiver::protocol::PROTOCOL_VERSION;

namespace
{
    OrderedPacket make_ordered_packet(uint32_t seq,
                                      size_t payload_size,
                                      bool is_zero_filled = false,
                                      uint64_t timestamp_ms = 1234)
    {
        OrderedPacket packet{};
        packet.packet.header.magic = PROTOCOL_MAGIC;
        packet.packet.header.protocol_version = PROTOCOL_VERSION;
        packet.packet.header.packet_type = static_cast<uint8_t>(PacketType::DATA);
        packet.packet.header.sequence_number = seq;
        packet.packet.header.timestamp = timestamp_ms;
        packet.packet.header.payload_len = static_cast<uint16_t>(
            std::min(payload_size, static_cast<size_t>(UINT16_MAX)));
        packet.packet.total_size = receiver::protocol::COMMON_HEADER_SIZE + payload_size;
        packet.payload_size = payload_size;
        packet.is_zero_filled = is_zero_filled;
        packet.sequence_number = seq;

        if (payload_size > 0)
        {
            packet.owned_payload = std::make_unique<uint8_t[]>(payload_size);
            for (size_t i = 0; i < payload_size; ++i)
            {
                packet.owned_payload[i] = static_cast<uint8_t>(i & 0xFFu);
            }
            packet.packet.payload = packet.owned_payload.get();
        }

        return packet;
    }
}

TEST(RawBlockAdapterTests, BasicAdaptation)
{
    auto queue = std::make_shared<RawBlockAdapter::RawBlockQueue>();
    RawBlockAdapter adapter(queue, 2);

    EXPECT_TRUE(adapter.adapt_and_push(make_ordered_packet(42, 1024)));

    auto maybe_block = queue->try_pop();
    ASSERT_TRUE(maybe_block.has_value());
    ASSERT_TRUE(*maybe_block);

    const auto &block = *maybe_block.value();
    EXPECT_EQ(block.cpi_seq, 42u);
    EXPECT_EQ(block.data_size, 1024u);
    EXPECT_EQ(block.array_id, 2u);
    EXPECT_EQ(block.data_ts, 1234u * 1000000ULL);
    EXPECT_EQ(block.fragment_count, 1u);
    EXPECT_EQ(block.payload[0], 0u);
    EXPECT_EQ(block.payload[1], 1u);
}

TEST(RawBlockAdapterTests, ZeroFilledPacket)
{
    auto queue = std::make_shared<RawBlockAdapter::RawBlockQueue>();
    RawBlockAdapter adapter(queue, 1);

    EXPECT_TRUE(adapter.adapt_and_push(make_ordered_packet(7, 64, true)));

    auto maybe_block = queue->try_pop();
    ASSERT_TRUE(maybe_block.has_value());
    ASSERT_TRUE(*maybe_block);
    EXPECT_NE(maybe_block.value()->flags & static_cast<uint32_t>(RawBlockFlags::INCOMPLETE_FRAME), 0u);
}

TEST(RawBlockAdapterTests, EmptyPayload)
{
    auto queue = std::make_shared<RawBlockAdapter::RawBlockQueue>();
    RawBlockAdapter adapter(queue, 1);

    OrderedPacket packet = make_ordered_packet(11, 0);
    packet.owned_payload.reset();
    packet.packet.payload = nullptr;

    EXPECT_TRUE(adapter.adapt_and_push(std::move(packet)));

    auto maybe_block = queue->try_pop();
    ASSERT_TRUE(maybe_block.has_value());
    ASSERT_TRUE(*maybe_block);
    EXPECT_EQ(maybe_block.value()->data_size, 0u);
}

TEST(RawBlockAdapterTests, OversizedPayload)
{
    auto queue = std::make_shared<RawBlockAdapter::RawBlockQueue>();
    RawBlockAdapter adapter(queue, 3);

    EXPECT_TRUE(adapter.adapt_and_push(make_ordered_packet(9, 3 * 1024 * 1024)));

    auto maybe_block = queue->try_pop();
    ASSERT_TRUE(maybe_block.has_value());
    ASSERT_TRUE(*maybe_block);

    const auto &block = *maybe_block.value();
    EXPECT_EQ(block.data_size, static_cast<uint32_t>(RAW_BLOCK_PAYLOAD_SIZE));
    EXPECT_NE(block.flags & static_cast<uint32_t>(RawBlockFlags::INCOMPLETE_FRAME), 0u);
    EXPECT_EQ(block.payload[0], 0u);
    EXPECT_EQ(block.payload[1], 1u);
    EXPECT_EQ(block.payload[RAW_BLOCK_PAYLOAD_SIZE - 1],
              static_cast<uint8_t>((RAW_BLOCK_PAYLOAD_SIZE - 1) & 0xFFu));
}

TEST(RawBlockAdapterTests, QueueOverflow)
{
    auto queue = std::make_shared<RawBlockAdapter::RawBlockQueue>();
    RawBlockAdapter adapter(queue, 1);

    bool observed_drop_oldest = false;
    for (uint32_t i = 0; i < 70; ++i)
    {
        observed_drop_oldest = !adapter.adapt_and_push(make_ordered_packet(i, 32)) || observed_drop_oldest;
    }

    EXPECT_TRUE(observed_drop_oldest);
    EXPECT_GT(adapter.dropped_count(), 0u);
}

TEST(RawBlockAdapterTests, FieldMappingConsistency)
{
    auto queue = std::make_shared<RawBlockAdapter::RawBlockQueue>();
    RawBlockAdapter adapter(queue, 4);

    for (uint32_t seq = 100; seq < 105; ++seq)
    {
        EXPECT_TRUE(adapter.adapt_and_push(make_ordered_packet(seq, 16, false, 2000 + seq)));
    }

    for (uint32_t seq = 100; seq < 105; ++seq)
    {
        auto maybe_block = queue->try_pop();
        ASSERT_TRUE(maybe_block.has_value());
        ASSERT_TRUE(*maybe_block);
        EXPECT_EQ(maybe_block.value()->cpi_seq, seq);
        EXPECT_EQ(maybe_block.value()->array_id, 4u);
        EXPECT_EQ(maybe_block.value()->data_ts, (2000ULL + seq) * 1000000ULL);
    }
}

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
