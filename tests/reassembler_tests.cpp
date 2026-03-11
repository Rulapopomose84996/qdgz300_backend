#include "qdgz300/m01_receiver/pipeline/reassembler.h"
#include "qdgz300/m01_receiver/protocol/packet_parser.h"
#include "qdgz300/m01_receiver/protocol/protocol_types.h"

#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

using receiver::pipeline::ReassembledFrame;
using receiver::pipeline::Reassembler;
using receiver::pipeline::ReassemblerConfig;
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

        ParsedPacketWithStorage() = default;
        ParsedPacketWithStorage(const ParsedPacketWithStorage &) = delete;
        ParsedPacketWithStorage &operator=(const ParsedPacketWithStorage &) = delete;

        ParsedPacketWithStorage(ParsedPacketWithStorage &&other) noexcept
            : raw(std::move(other.raw)), parsed(other.parsed)
        {
            if (!raw.empty())
            {
                parsed.payload = raw.data() + receiver::protocol::COMMON_HEADER_SIZE;
            }
        }

        ParsedPacketWithStorage &operator=(ParsedPacketWithStorage &&other) noexcept
        {
            if (this != &other)
            {
                raw = std::move(other.raw);
                parsed = other.parsed;
                if (!raw.empty())
                {
                    parsed.payload = raw.data() + receiver::protocol::COMMON_HEADER_SIZE;
                }
            }
            return *this;
        }
    };

    std::vector<uint8_t> make_data_packet(uint32_t seq,
                                          uint32_t frame_counter,
                                          uint16_t beam_id,
                                          uint16_t frag_index,
                                          uint16_t total_frags,
                                          const std::vector<uint8_t> &raw_payload,
                                          uint16_t tail_raw_bytes)
    {
        const size_t common_size = 32;
        const size_t specific_size = sizeof(DataSpecificHeader);
        const bool is_tail = (frag_index + 1 == total_frags);
        const size_t snapshot_size = is_tail ? sizeof(ExecutionSnapshot) : 0;
        const uint16_t payload_len = static_cast<uint16_t>(specific_size + snapshot_size + raw_payload.size());

        std::vector<uint8_t> packet(common_size + payload_len, 0);
        const uint64_t timestamp_ms = 1700000000000ULL;
        const uint16_t control_epoch = 1;

        std::memcpy(packet.data() + 0, &PROTOCOL_MAGIC, sizeof(PROTOCOL_MAGIC));
        std::memcpy(packet.data() + 4, &seq, sizeof(seq));
        std::memcpy(packet.data() + 8, &timestamp_ms, sizeof(timestamp_ms));
        std::memcpy(packet.data() + 16, &payload_len, sizeof(payload_len));
        packet[18] = static_cast<uint8_t>(PacketType::DATA);
        packet[19] = PROTOCOL_VERSION;
        packet[20] = 0x11;
        packet[21] = 0x01;
        std::memcpy(packet.data() + 22, &control_epoch, sizeof(control_epoch));

        DataSpecificHeader specific{};
        specific.frame_counter = frame_counter;
        specific.cpi_count = 10;
        specific.pulse_index = 20;
        specific.sample_offset = 0;
        specific.sample_count = 2;
        specific.data_timestamp = timestamp_ms;
        specific.health_summary = 0;
        specific.set_channel_mask_data_type_compat(0x0001, 0x00);
        specific.beam_id = beam_id;
        specific.frag_index = frag_index;
        specific.total_frags = total_frags;
        specific.tail_frag_payload_bytes = tail_raw_bytes;

        std::memcpy(packet.data() + common_size, &specific, sizeof(specific));

        size_t offset = common_size + specific_size;
        if (is_tail)
        {
            ExecutionSnapshot snapshot{};
            snapshot.work_freq_index = 11;
            std::memcpy(packet.data() + offset, &snapshot, sizeof(snapshot));
            offset += sizeof(snapshot);
        }

        if (!raw_payload.empty())
        {
            std::memcpy(packet.data() + offset, raw_payload.data(), raw_payload.size());
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
        if (!parsed)
        {
            out.parsed = ParsedPacket{};
            return out;
        }
        out.parsed = *parsed;
        return out;
    }
}

class ReassemblerMatrixTest : public ::testing::Test
{
protected:
    ReassemblerMatrixTest()
    {
        config.timeout_ms = 10;
        config.sample_count_fixed = 2; // non-tail missing fragment size = 2 * 4 * 1 = 8
        config.max_total_frags = 1024;
        config.max_contexts = 1024;
        reassembler = std::make_unique<Reassembler>(config, [&](ReassembledFrame &&frame)
                                                    { frames.push_back(std::move(frame)); });
    }

    ReassemblerConfig config{};
    std::vector<ReassembledFrame> frames;
    std::unique_ptr<Reassembler> reassembler;
};

TEST_F(ReassemblerMatrixTest, SingleFragmentCompleteFrameHasSnapshot)
{
    auto pkt = parse_packet(make_data_packet(1, 100, 1, 0, 1, {0xA1, 0xA2, 0xA3, 0xA4}, 4));
    reassembler->process_packet(pkt.parsed);

    ASSERT_EQ(frames.size(), 1u);
    EXPECT_TRUE(frames[0].is_complete);
    EXPECT_TRUE(frames[0].has_execution_snapshot);
    EXPECT_EQ(frames[0].total_size, 4u);
}

TEST_F(ReassemblerMatrixTest, MultiFragmentInOrder)
{
    auto f0 = parse_packet(make_data_packet(1, 101, 1, 0, 3, {1, 2, 3, 4, 5, 6, 7, 8}, 6));
    auto f1 = parse_packet(make_data_packet(2, 101, 1, 1, 3, {11, 12, 13, 14, 15, 16, 17, 18}, 6));
    auto f2 = parse_packet(make_data_packet(3, 101, 1, 2, 3, {21, 22, 23, 24, 25, 26}, 6));
    reassembler->process_packet(f0.parsed);
    reassembler->process_packet(f1.parsed);
    reassembler->process_packet(f2.parsed);

    ASSERT_EQ(frames.size(), 1u);
    EXPECT_TRUE(frames[0].is_complete);
    EXPECT_EQ(frames[0].total_size, 22u);
}

TEST_F(ReassemblerMatrixTest, MultiFragmentOutOfOrderSameContent)
{
    auto f2 = parse_packet(make_data_packet(1, 102, 1, 2, 3, {21, 22, 23, 24, 25, 26}, 6));
    auto f0 = parse_packet(make_data_packet(2, 102, 1, 0, 3, {1, 2, 3, 4, 5, 6, 7, 8}, 6));
    auto f1 = parse_packet(make_data_packet(3, 102, 1, 1, 3, {11, 12, 13, 14, 15, 16, 17, 18}, 6));
    reassembler->process_packet(f2.parsed);
    reassembler->process_packet(f0.parsed);
    reassembler->process_packet(f1.parsed);

    ASSERT_EQ(frames.size(), 1u);
    ASSERT_EQ(frames[0].total_size, 22u);
    const std::vector<uint8_t> expected = {
        1, 2, 3, 4, 5, 6, 7, 8,
        11, 12, 13, 14, 15, 16, 17, 18,
        21, 22, 23, 24, 25, 26};
    ASSERT_NE(frames[0].data, nullptr);
    EXPECT_EQ(std::memcmp(frames[0].data.get(), expected.data(), expected.size()), 0);
}

TEST_F(ReassemblerMatrixTest, MissingMiddleFragmentZeroFilledAfterTimeout)
{
    auto f0 = parse_packet(make_data_packet(1, 103, 1, 0, 3, {1, 2, 3, 4, 5, 6, 7, 8}, 6));
    auto f2 = parse_packet(make_data_packet(2, 103, 1, 2, 3, {21, 22, 23, 24, 25, 26}, 6));
    reassembler->process_packet(f0.parsed);
    reassembler->process_packet(f2.parsed);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    reassembler->check_timeouts();

    ASSERT_EQ(frames.size(), 1u);
    EXPECT_FALSE(frames[0].is_complete);
    EXPECT_EQ(frames[0].missing_fragments_count, 1u);
    EXPECT_EQ(frames[0].total_size, 22u);
    for (size_t i = 8; i < 16; ++i)
    {
        EXPECT_EQ(frames[0].data[i], 0u);
    }
}

TEST_F(ReassemblerMatrixTest, MissingTailFragmentZeroFilledWithTailBytes)
{
    auto f0 = parse_packet(make_data_packet(1, 104, 1, 0, 3, {1, 2, 3, 4, 5, 6, 7, 8}, 6));
    auto f1 = parse_packet(make_data_packet(2, 104, 1, 1, 3, {11, 12, 13, 14, 15, 16, 17, 18}, 6));
    reassembler->process_packet(f0.parsed);
    reassembler->process_packet(f1.parsed);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    reassembler->check_timeouts();

    ASSERT_EQ(frames.size(), 1u);
    EXPECT_FALSE(frames[0].is_complete);
    EXPECT_EQ(frames[0].total_size, 22u);
    for (size_t i = 16; i < 22; ++i)
    {
        EXPECT_EQ(frames[0].data[i], 0u);
    }
}

TEST_F(ReassemblerMatrixTest, DuplicateFragmentDroppedAndCounted)
{
    auto f0 = parse_packet(make_data_packet(1, 105, 1, 0, 3, {1, 2, 3, 4, 5, 6, 7, 8}, 6));
    auto f1 = parse_packet(make_data_packet(2, 105, 1, 1, 3, {11, 12, 13, 14, 15, 16, 17, 18}, 6));
    auto f1dup = parse_packet(make_data_packet(3, 105, 1, 1, 3, {99, 99, 99, 99, 99, 99, 99, 99}, 6));
    auto f2 = parse_packet(make_data_packet(4, 105, 1, 2, 3, {21, 22, 23, 24, 25, 26}, 6));
    reassembler->process_packet(f0.parsed);
    reassembler->process_packet(f1.parsed);
    reassembler->process_packet(f1dup.parsed);
    reassembler->process_packet(f2.parsed);

    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(reassembler->get_statistics().duplicate_fragments.load(), 1u);
}

TEST_F(ReassemblerMatrixTest, LateFragmentDroppedAndCounted)
{
    auto f0 = parse_packet(make_data_packet(1, 106, 1, 0, 3, {1, 2, 3, 4, 5, 6, 7, 8}, 6));
    auto f2 = parse_packet(make_data_packet(2, 106, 1, 2, 3, {21, 22, 23, 24, 25, 26}, 6));
    reassembler->process_packet(f0.parsed);
    reassembler->process_packet(f2.parsed);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    reassembler->check_timeouts();

    const uint64_t contexts_after_timeout = reassembler->get_statistics().contexts_created.load();
    auto late = parse_packet(make_data_packet(3, 106, 1, 1, 3, {11, 12, 13, 14, 15, 16, 17, 18}, 6));
    reassembler->process_packet(late.parsed);

    EXPECT_EQ(reassembler->get_statistics().late_fragments.load(), 1u);
    EXPECT_EQ(reassembler->get_statistics().contexts_created.load(), contexts_after_timeout);
}

TEST_F(ReassemblerMatrixTest, TotalFragsExceedsLimitDropped)
{
    auto bad = parse_packet(make_data_packet(1, 107, 1, 0, 1025, {1, 2, 3, 4}, 4));
    reassembler->process_packet(bad.parsed);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    reassembler->check_timeouts();
    EXPECT_TRUE(frames.empty());
}

TEST_F(ReassemblerMatrixTest, ContextOverflowCounted)
{
    ReassemblerConfig cfg;
    cfg.timeout_ms = 1000;
    cfg.max_contexts = 2;
    cfg.max_total_frags = 1024;
    cfg.sample_count_fixed = 2;
    std::vector<ReassembledFrame> local_frames;
    Reassembler limited(cfg, [&](ReassembledFrame &&frame)
                        { local_frames.push_back(std::move(frame)); });

    auto c1 = parse_packet(make_data_packet(1, 201, 1, 0, 3, {1, 2, 3, 4, 5, 6, 7, 8}, 6));
    auto c2 = parse_packet(make_data_packet(2, 202, 1, 0, 3, {1, 2, 3, 4, 5, 6, 7, 8}, 6));
    auto c3 = parse_packet(make_data_packet(3, 203, 1, 0, 3, {1, 2, 3, 4, 5, 6, 7, 8}, 6));
    limited.process_packet(c1.parsed);
    limited.process_packet(c2.parsed);
    limited.process_packet(c3.parsed);

    EXPECT_EQ(limited.get_statistics().contexts_overflow.load(), 1u);
}

TEST_F(ReassemblerMatrixTest, ReasmBytesPerKeyLimitDropsExcessFragment)
{
    ReassemblerConfig cfg;
    cfg.timeout_ms = 10;
    cfg.max_contexts = 1024;
    cfg.max_total_frags = 1024;
    cfg.sample_count_fixed = 2;
    cfg.max_reasm_bytes_per_key = 10;

    std::vector<ReassembledFrame> local_frames;
    Reassembler limited(cfg, [&](ReassembledFrame &&frame)
                        { local_frames.push_back(std::move(frame)); });

    auto f0 = parse_packet(make_data_packet(1, 301, 1, 0, 2, {1, 2, 3, 4, 5, 6, 7, 8}, 6));
    auto f1 = parse_packet(make_data_packet(2, 301, 1, 1, 2, {11, 12, 13, 14, 15, 16}, 6));
    limited.process_packet(f0.parsed);
    limited.process_packet(f1.parsed);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    limited.check_timeouts();

    ASSERT_EQ(local_frames.size(), 1u);
    EXPECT_FALSE(local_frames[0].is_complete);
    EXPECT_EQ(local_frames[0].missing_fragments_count, 1u);
    EXPECT_EQ(limited.get_statistics().reasm_bytes_overflow.load(), 1u);
}
