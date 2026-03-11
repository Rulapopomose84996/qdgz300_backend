#include "qdgz300/m01_receiver/delivery/rawblock_adapter.h"
#include "qdgz300/m01_receiver/delivery/raw_block.h"
#include "qdgz300/m01_receiver/delivery/stub_consumer.h"
#include "qdgz300/m01_receiver/pipeline/dispatcher.h"
#include "qdgz300/m01_receiver/pipeline/reassembler.h"
#include "qdgz300/m01_receiver/pipeline/reorderer.h"
#include "qdgz300/m01_receiver/protocol/packet_parser.h"
#include "qdgz300/m01_receiver/protocol/protocol_types.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

using receiver::delivery::RawBlock;
using receiver::delivery::RawBlockAdapter;
using receiver::delivery::RawBlockFlags;
using receiver::delivery::StubConsumer;
using receiver::delivery::StubConsumerConfig;
using receiver::pipeline::Dispatcher;
using receiver::pipeline::OrderedPacket;
using receiver::pipeline::ReassembledFrame;
using receiver::pipeline::Reassembler;
using receiver::pipeline::ReassemblerConfig;
using receiver::pipeline::ReorderConfig;
using receiver::pipeline::Reorderer;
using receiver::protocol::DataSpecificHeader;
using receiver::protocol::ExecutionSnapshot;
using receiver::protocol::PacketParser;
using receiver::protocol::PacketType;
using receiver::protocol::ParsedPacket;
using receiver::protocol::PROTOCOL_MAGIC;
using receiver::protocol::PROTOCOL_VERSION;

namespace
{
    struct ParsedPacketWithStorage
    {
        std::vector<uint8_t> raw;
        ParsedPacket parsed;
    };

    struct FaceHarness
    {
        std::shared_ptr<RawBlockAdapter::RawBlockQueue> queue;
        std::unique_ptr<RawBlockAdapter> adapter;
        std::unique_ptr<StubConsumer> consumer;
        std::unique_ptr<Reorderer> reorderer;
        std::unique_ptr<Reassembler> reassembler;
        std::unique_ptr<Dispatcher> dispatcher;
    };

    std::vector<uint8_t> make_data_packet(uint32_t seq,
                                          uint32_t frame_counter,
                                          uint16_t frag_index,
                                          uint16_t total_frags,
                                          const std::vector<uint8_t> &payload,
                                          uint16_t tail_raw_bytes)
    {
        const size_t common_size = receiver::protocol::COMMON_HEADER_SIZE;
        const size_t specific_size = sizeof(DataSpecificHeader);
        const bool is_tail = (frag_index + 1 == total_frags);
        const size_t snapshot_size = is_tail ? sizeof(ExecutionSnapshot) : 0;
        const uint16_t payload_len = static_cast<uint16_t>(specific_size + snapshot_size + payload.size());
        std::vector<uint8_t> packet(common_size + payload_len, 0);

        const uint64_t now_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
        const uint16_t control_epoch = 1;

        std::memcpy(packet.data() + 0, &PROTOCOL_MAGIC, sizeof(PROTOCOL_MAGIC));
        std::memcpy(packet.data() + 4, &seq, sizeof(seq));
        std::memcpy(packet.data() + 8, &now_ms, sizeof(now_ms));
        std::memcpy(packet.data() + 16, &payload_len, sizeof(payload_len));
        packet[18] = static_cast<uint8_t>(PacketType::DATA);
        packet[19] = PROTOCOL_VERSION;
        packet[20] = 0x11;
        packet[21] = 0x01;
        std::memcpy(packet.data() + 22, &control_epoch, sizeof(control_epoch));

        DataSpecificHeader specific{};
        specific.frame_counter = frame_counter;
        specific.cpi_count = 1;
        specific.pulse_index = 1;
        specific.sample_offset = static_cast<uint16_t>(frag_index * 4);
        specific.sample_count = 4;
        specific.data_timestamp = now_ms;
        specific.health_summary = 0;
        specific.set_channel_mask_data_type_compat(0x0001, 0x00);
        specific.beam_id = 1;
        specific.frag_index = frag_index;
        specific.total_frags = total_frags;
        specific.tail_frag_payload_bytes = tail_raw_bytes;
        std::memcpy(packet.data() + common_size, &specific, sizeof(specific));

        size_t offset = common_size + specific_size;
        if (is_tail)
        {
            ExecutionSnapshot snapshot{};
            snapshot.work_freq_index = 1;
            std::memcpy(packet.data() + offset, &snapshot, sizeof(snapshot));
            offset += sizeof(snapshot);
        }

        if (!payload.empty())
        {
            std::memcpy(packet.data() + offset, payload.data(), payload.size());
        }

        return packet;
    }

    ParsedPacketWithStorage parse_packet(std::vector<uint8_t> raw)
    {
        ParsedPacketWithStorage out;
        out.raw = std::move(raw);

        PacketParser parser;
        auto parsed = parser.parse(out.raw.data(), out.raw.size());
        EXPECT_TRUE(parsed.has_value());
        if (parsed.has_value())
        {
            out.parsed = *parsed;
        }
        return out;
    }

    std::vector<uint8_t> make_payload(size_t size, uint8_t seed)
    {
        std::vector<uint8_t> payload(size);
        for (size_t i = 0; i < size; ++i)
        {
            payload[i] = static_cast<uint8_t>(seed + static_cast<uint8_t>(i));
        }
        return payload;
    }

    std::shared_ptr<RawBlock> wait_and_pop(const std::shared_ptr<RawBlockAdapter::RawBlockQueue> &queue,
                                           std::chrono::milliseconds timeout = std::chrono::milliseconds(300))
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            auto item = queue->try_pop();
            if (item.has_value())
            {
                return *item;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return nullptr;
    }

    FaceHarness make_face_harness(uint8_t array_id, bool print_summary = false)
    {
        FaceHarness face;
        face.queue = std::make_shared<RawBlockAdapter::RawBlockQueue>();
        face.adapter = std::make_unique<RawBlockAdapter>(face.queue, array_id);

        StubConsumerConfig sc_cfg;
        sc_cfg.array_id = array_id;
        sc_cfg.print_summary = print_summary;
        sc_cfg.write_to_file = false;
        sc_cfg.stats_interval_ms = 100;
        face.consumer = std::make_unique<StubConsumer>(sc_cfg, face.queue);

        ReorderConfig reorder_cfg;
        reorder_cfg.window_size = 64;
        reorder_cfg.timeout_ms = 50;
        reorder_cfg.enable_zero_fill = true;
        face.reorderer = std::make_unique<Reorderer>(
            reorder_cfg,
            [adapter = face.adapter.get()](OrderedPacket &&pkt)
            {
                adapter->adapt_and_push(std::move(pkt));
            });

        ReassemblerConfig reasm_cfg;
        reasm_cfg.timeout_ms = 50;
        reasm_cfg.max_contexts = 1024;
        reasm_cfg.max_total_frags = 1024;
        reasm_cfg.sample_count_fixed = 4;
        reasm_cfg.max_reasm_bytes_per_key = 16u * 1024u * 1024u;
        reasm_cfg.numa_node = 0;
        reasm_cfg.cache_align_bytes = 64;
        reasm_cfg.prefetch_hints_enabled = false;

        face.reassembler = std::make_unique<Reassembler>(
            reasm_cfg,
            [reorderer = face.reorderer.get()](ReassembledFrame &&frame)
            {
                receiver::protocol::CommonHeader header{};
                header.magic = PROTOCOL_MAGIC;
                header.protocol_version = PROTOCOL_VERSION;
                header.packet_type = static_cast<uint8_t>(PacketType::DATA);
                header.source_id = frame.key.source_id;
                header.control_epoch = frame.key.control_epoch;
                header.sequence_number = frame.key.frame_counter;
                header.payload_len = static_cast<uint16_t>(frame.total_size);
                header.timestamp = frame.data_timestamp;
                header.ext_flags = frame.is_complete ? 0u : 0x01u;
                reorderer->insert_owned(header, std::move(frame.data), frame.total_size);
            });

        face.dispatcher = std::make_unique<Dispatcher>(
            [reassembler = face.reassembler.get()](const ParsedPacket &packet)
            {
                reassembler->process_packet(packet);
            });

        return face;
    }
} // namespace

class RawBlockDeliveryTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        face_ = make_face_harness(1, false);
    }

    void dispatch_single(uint32_t sequence_number, uint8_t seed = 0x10)
    {
        auto parsed = parse_packet(make_data_packet(
            sequence_number, sequence_number, 0, 1, make_payload(12, seed), 12));
        ASSERT_NE(face_.dispatcher, nullptr);
        face_.dispatcher->dispatch(parsed.parsed);
    }

    void dispatch_fragments(uint32_t base_seq,
                            uint32_t frame_counter,
                            uint16_t total_frags,
                            size_t non_tail_size,
                            size_t tail_size)
    {
        for (uint16_t frag = 0; frag < total_frags; ++frag)
        {
            const bool is_tail = (frag + 1 == total_frags);
            const size_t payload_size = is_tail ? tail_size : non_tail_size;
            auto parsed = parse_packet(make_data_packet(
                base_seq + frag,
                frame_counter,
                frag,
                total_frags,
                make_payload(payload_size, static_cast<uint8_t>(0x20 + frag)),
                static_cast<uint16_t>(tail_size)));
            face_.dispatcher->dispatch(parsed.parsed);
        }
    }

    FaceHarness face_;
};

TEST_F(RawBlockDeliveryTest, SingleCpiEndToEnd)
{
    dispatch_single(42);

    auto block = wait_and_pop(face_.queue);
    ASSERT_NE(block, nullptr);
    EXPECT_EQ(block->cpi_seq, 42u);
    EXPECT_EQ(block->array_id, 1u);
    EXPECT_EQ(block->data_size, 12u);
    EXPECT_EQ(block->flags, 0u);
    EXPECT_FALSE(block->has_flag(RawBlockFlags::INCOMPLETE_FRAME));
}

TEST_F(RawBlockDeliveryTest, MultiFragmentCpi)
{
    dispatch_fragments(100, 100, 8, 16, 10);

    auto block = wait_and_pop(face_.queue);
    ASSERT_NE(block, nullptr);
    EXPECT_EQ(block->cpi_seq, 100u);
    EXPECT_EQ(block->array_id, 1u);
    EXPECT_EQ(block->data_size, 7u * 16u + 10u);
    EXPECT_EQ(block->fragment_count, 1u);
    EXPECT_EQ(block->flags, 0u);
}

TEST_F(RawBlockDeliveryTest, ZeroFilledPacket)
{
    dispatch_single(1, 0x31);
    dispatch_single(3, 0x33);

    auto first = wait_and_pop(face_.queue);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->cpi_seq, 1u);
    EXPECT_FALSE(first->has_flag(RawBlockFlags::INCOMPLETE_FRAME));

    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    face_.reorderer->check_timeout();

    auto gap = wait_and_pop(face_.queue);
    ASSERT_NE(gap, nullptr);
    EXPECT_EQ(gap->cpi_seq, 2u);
    EXPECT_TRUE(gap->has_flag(RawBlockFlags::INCOMPLETE_FRAME));

    auto third = wait_and_pop(face_.queue);
    ASSERT_NE(third, nullptr);
    EXPECT_EQ(third->cpi_seq, 3u);
    EXPECT_FALSE(third->has_flag(RawBlockFlags::INCOMPLETE_FRAME));
}

TEST_F(RawBlockDeliveryTest, QueueOverflow)
{
    for (uint32_t seq = 0; seq < 70; ++seq)
    {
        dispatch_single(seq, static_cast<uint8_t>(seq));
    }

    EXPECT_GT(face_.adapter->dropped_count(), 0u);
    auto item = wait_and_pop(face_.queue);
    ASSERT_NE(item, nullptr);
    EXPECT_LE(item->cpi_seq, 69u);
}

TEST_F(RawBlockDeliveryTest, StubConsumerConsumeAll)
{
    for (uint32_t seq = 0; seq < 10; ++seq)
    {
        dispatch_single(seq, static_cast<uint8_t>(seq + 1));
    }

    ASSERT_TRUE(face_.consumer->start());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    face_.consumer->stop();

    const auto &stats = face_.consumer->get_statistics();
    EXPECT_GE(stats.total_blocks.load(), 10u);
}

TEST_F(RawBlockDeliveryTest, ThreeFaceParallel)
{
    auto face1 = make_face_harness(1, false);
    auto face2 = make_face_harness(2, false);
    auto face3 = make_face_harness(3, false);

    ASSERT_TRUE(face1.consumer->start());
    ASSERT_TRUE(face2.consumer->start());
    ASSERT_TRUE(face3.consumer->start());

    auto producer = [](FaceHarness &face, uint32_t base_seq)
    {
        for (uint32_t i = 0; i < 50; ++i)
        {
            auto parsed = parse_packet(make_data_packet(
                base_seq + i,
                base_seq + i,
                0,
                1,
                make_payload(12, static_cast<uint8_t>(i)),
                12));
            face.dispatcher->dispatch(parsed.parsed);
        }
    };

    std::thread t1(producer, std::ref(face1), 0);
    std::thread t2(producer, std::ref(face2), 1000);
    std::thread t3(producer, std::ref(face3), 2000);
    t1.join();
    t2.join();
    t3.join();

    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    face1.consumer->stop();
    face2.consumer->stop();
    face3.consumer->stop();

    EXPECT_GE(face1.consumer->get_statistics().total_blocks.load(), 50u);
    EXPECT_GE(face2.consumer->get_statistics().total_blocks.load(), 50u);
    EXPECT_GE(face3.consumer->get_statistics().total_blocks.load(), 50u);
}

TEST_F(RawBlockDeliveryTest, CpiSeqConsistency)
{
    for (uint32_t seq = 0; seq < 100; ++seq)
    {
        dispatch_single(seq, static_cast<uint8_t>(seq));
        auto block = wait_and_pop(face_.queue);
        ASSERT_NE(block, nullptr);
        EXPECT_EQ(block->cpi_seq, seq);
    }
}

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
