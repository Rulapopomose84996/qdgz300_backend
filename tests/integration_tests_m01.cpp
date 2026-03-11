// M01 Integration Tests - 简化版集成测试（仅Data+Heartbeat）
// 匹配M01_Receiver_Design.md规范

#include "qdgz300/m01_receiver/pipeline/dispatcher.h"
#include "qdgz300/m01_receiver/pipeline/reassembler.h"
#include "qdgz300/m01_receiver/pipeline/reorderer.h"
#include "qdgz300/m01_receiver/protocol/packet_parser.h"
#include "qdgz300/m01_receiver/protocol/protocol_types.h"
#include "qdgz300/m01_receiver/protocol/validator.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

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
    // 创建Data包（单分片）
    std::vector<uint8_t> make_data_packet(uint32_t seq, uint32_t frame_counter, const std::vector<uint8_t> &payload)
    {
        const size_t common_size = receiver::protocol::COMMON_HEADER_SIZE;
        const size_t specific_size = sizeof(DataSpecificHeader);
        const size_t snapshot_size = sizeof(ExecutionSnapshot);
        const uint16_t payload_len = static_cast<uint16_t>(specific_size + snapshot_size + payload.size());

        std::vector<uint8_t> packet(common_size + payload_len, 0);

        const uint64_t now_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
        const uint16_t control_epoch = 1;
        const uint32_t magic = PROTOCOL_MAGIC;

        std::memcpy(packet.data() + 0, &magic, sizeof(magic));
        std::memcpy(packet.data() + 4, &seq, sizeof(seq));
        std::memcpy(packet.data() + 8, &now_ms, sizeof(now_ms));
        std::memcpy(packet.data() + 16, &payload_len, sizeof(payload_len));
        packet[18] = static_cast<uint8_t>(PacketType::DATA);
        packet[19] = PROTOCOL_VERSION;
        packet[20] = 0x11; // source_id
        packet[21] = 0x01; // dest_id
        std::memcpy(packet.data() + 22, &control_epoch, sizeof(control_epoch));

        DataSpecificHeader specific{};
        specific.frame_counter = frame_counter;
        specific.cpi_count = 1;
        specific.pulse_index = 1;
        specific.sample_offset = 0;
        specific.sample_count = 1;
        specific.data_timestamp = now_ms;
        specific.health_summary = 0;
        specific.set_channel_mask_data_type_compat(0x0001, 0x00);
        specific.beam_id = 1;
        specific.frag_index = 0;
        specific.total_frags = 1;
        specific.tail_frag_payload_bytes = static_cast<uint16_t>(payload.size());
        std::memcpy(packet.data() + common_size, &specific, sizeof(specific));

        ExecutionSnapshot snapshot{};
        snapshot.work_freq_index = 1;
        std::memcpy(packet.data() + common_size + specific_size, &snapshot, sizeof(snapshot));

        if (!payload.empty())
        {
            std::memcpy(packet.data() + common_size + specific_size + snapshot_size, payload.data(), payload.size());
        }
        return packet;
    }

    // 创建Heartbeat包
    std::vector<uint8_t> make_heartbeat_packet(uint32_t seq)
    {
        const size_t common_size = receiver::protocol::COMMON_HEADER_SIZE;
        const uint16_t payload_len = 64; // 心跳负载固定64字节
        std::vector<uint8_t> packet(common_size + payload_len, 0);

        const uint64_t now_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
        const uint16_t control_epoch = 1;
        const uint32_t magic = PROTOCOL_MAGIC;

        std::memcpy(packet.data() + 0, &magic, sizeof(magic));
        std::memcpy(packet.data() + 4, &seq, sizeof(seq));
        std::memcpy(packet.data() + 8, &now_ms, sizeof(now_ms));
        std::memcpy(packet.data() + 16, &payload_len, sizeof(payload_len));
        packet[18] = static_cast<uint8_t>(PacketType::HEARTBEAT);
        packet[19] = PROTOCOL_VERSION;
        packet[20] = 0x11;
        packet[21] = 0x01;
        std::memcpy(packet.data() + 22, &control_epoch, sizeof(control_epoch));

        // 简化Heartbeat负载（仅填充示例数据）
        for (size_t i = 0; i < payload_len; ++i)
        {
            packet[common_size + i] = static_cast<uint8_t>(i & 0xFF);
        }
        return packet;
    }

    // 创建多分片Data包
    std::vector<std::vector<uint8_t>> make_fragmented_data_packet(
        uint32_t base_seq,
        uint32_t frame_counter,
        uint16_t total_frags,
        size_t frag_size)
    {
        std::vector<std::vector<uint8_t>> fragments;
        const size_t common_size = receiver::protocol::COMMON_HEADER_SIZE;
        const size_t specific_size = sizeof(DataSpecificHeader);

        for (uint16_t frag_idx = 0; frag_idx < total_frags; ++frag_idx)
        {
            const bool is_tail = (frag_idx + 1 == total_frags);
            const size_t snapshot_size = is_tail ? sizeof(ExecutionSnapshot) : 0;
            const uint16_t payload_len = static_cast<uint16_t>(specific_size + snapshot_size + frag_size);

            std::vector<uint8_t> packet(common_size + payload_len, 0);

            const uint64_t now_ms = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count());
            const uint16_t control_epoch = 1;
            const uint32_t magic = PROTOCOL_MAGIC;
            const uint32_t seq = base_seq + frag_idx;

            std::memcpy(packet.data() + 0, &magic, sizeof(magic));
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
            specific.sample_offset = static_cast<uint16_t>(frag_idx * frag_size);
            specific.sample_count = 1;
            specific.data_timestamp = now_ms;
            specific.health_summary = 0;
            specific.set_channel_mask_data_type_compat(0x0001, 0x00);
            specific.beam_id = 1;
            specific.frag_index = frag_idx;
            specific.total_frags = total_frags;
            specific.tail_frag_payload_bytes = static_cast<uint16_t>(frag_size);
            std::memcpy(packet.data() + common_size, &specific, sizeof(specific));

            size_t offset = common_size + specific_size;
            if (is_tail)
            {
                ExecutionSnapshot snapshot{};
                snapshot.work_freq_index = 1;
                std::memcpy(packet.data() + offset, &snapshot, sizeof(snapshot));
                offset += sizeof(snapshot);
            }

            // 填充分片负载
            for (size_t i = 0; i < frag_size; ++i)
            {
                packet[offset + i] = static_cast<uint8_t>((frag_idx * 100 + i) & 0xFF);
            }

            fragments.push_back(std::move(packet));
        }
        return fragments;
    }
} // namespace

// ===== 基础功能测试 =====

TEST(IntegrationTests_M01, DispatcherFiltersOnlyDataAndHeartbeat)
{
    size_t data_count = 0;
    size_t heartbeat_count = 0;

    Dispatcher dispatcher([&](const ParsedPacket &packet)
                          {
        if (packet.header.packet_type == static_cast<uint8_t>(PacketType::DATA)) {
            ++data_count;
        } else if (packet.header.packet_type == static_cast<uint8_t>(PacketType::HEARTBEAT)) {
            ++heartbeat_count;
        } });

    PacketParser parser;
    auto data_raw = make_data_packet(1, 100, {0x01, 0x02, 0x03});
    auto heartbeat_raw = make_heartbeat_packet(2);

    auto data_parsed = parser.parse(data_raw.data(), data_raw.size());
    auto heartbeat_parsed = parser.parse(heartbeat_raw.data(), heartbeat_raw.size());

    ASSERT_TRUE(data_parsed.has_value());
    ASSERT_TRUE(heartbeat_parsed.has_value());

    dispatcher.dispatch(*data_parsed);
    dispatcher.dispatch(*heartbeat_parsed);

    EXPECT_EQ(data_count, 1u);
    EXPECT_EQ(heartbeat_count, 1u);
}

TEST(IntegrationTests_M01, DispatcherPrioritizesHeartbeatInBatch)
{
    std::vector<uint8_t> dispatch_order;
    std::mutex order_mutex;

    Dispatcher dispatcher([&](const ParsedPacket &packet)
                          {
        std::lock_guard<std::mutex> lock(order_mutex);
        dispatch_order.push_back(packet.header.packet_type); });

    PacketParser parser;
    std::vector<ParsedPacket> batch;

    // 批量：Data, Data, Heartbeat, Data
    auto data1 = parser.parse_move(make_data_packet(1, 100, {0xAA}));
    auto data2 = parser.parse_move(make_data_packet(2, 101, {0xBB}));
    auto heartbeat = parser.parse_move(make_heartbeat_packet(3));
    auto data3 = parser.parse_move(make_data_packet(4, 102, {0xCC}));

    ASSERT_TRUE(data1.has_value());
    ASSERT_TRUE(data2.has_value());
    ASSERT_TRUE(heartbeat.has_value());
    ASSERT_TRUE(data3.has_value());

    batch.push_back(*data1);
    batch.push_back(*data2);
    batch.push_back(*heartbeat);
    batch.push_back(*data3);

    dispatcher.dispatch_batch(batch.data(), batch.size());

    ASSERT_EQ(dispatch_order.size(), 4u);
    // Heartbeat应优先处理
    EXPECT_EQ(dispatch_order[0], static_cast<uint8_t>(PacketType::HEARTBEAT));
}

TEST(IntegrationTests_M01, ReordererOutputsInSequenceOrder)
{
    std::vector<uint32_t> output_seqs;
    std::mutex output_mutex;

    ReorderConfig config{32, 50, true};
    Reorderer reorderer(config, [&](OrderedPacket &&pkt)
                        {
        std::lock_guard<std::mutex> lock(output_mutex);
        output_seqs.push_back(pkt.sequence_number); });

    PacketParser parser;

    // 乱序输入：3, 1, 2
    std::vector<uint32_t> input_order = {3, 1, 2};
    for (uint32_t seq : input_order)
    {
        auto raw = make_data_packet(seq, 100 + seq, {static_cast<uint8_t>(seq)});
        auto parsed = parser.parse(raw.data(), raw.size());
        ASSERT_TRUE(parsed.has_value());
        reorderer.insert(*parsed);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    ASSERT_EQ(output_seqs.size(), 3u);
    EXPECT_EQ(output_seqs[0], 1u);
    EXPECT_EQ(output_seqs[1], 2u);
    EXPECT_EQ(output_seqs[2], 3u);
}

TEST(IntegrationTests_M01, ReordererSequenceWraparound)
{
    std::vector<uint32_t> output_seqs;
    std::mutex output_mutex;

    ReorderConfig config{32, 50, true};
    Reorderer reorderer(config, [&](OrderedPacket &&pkt)
                        {
        std::lock_guard<std::mutex> lock(output_mutex);
        output_seqs.push_back(pkt.sequence_number); });

    PacketParser parser;

    // 序列号环绕：UINT32_MAX-1, UINT32_MAX, 0, 1
    std::vector<uint32_t> wraparound_seqs = {UINT32_MAX - 1, UINT32_MAX, 0, 1};
    for (uint32_t seq : wraparound_seqs)
    {
        auto raw = make_data_packet(seq, 100, {0xFF});
        auto parsed = parser.parse(raw.data(), raw.size());
        ASSERT_TRUE(parsed.has_value());
        reorderer.insert(*parsed);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    ASSERT_EQ(output_seqs.size(), 4u);
    EXPECT_EQ(output_seqs[0], UINT32_MAX - 1);
    EXPECT_EQ(output_seqs[1], UINT32_MAX);
    EXPECT_EQ(output_seqs[2], 0u);
    EXPECT_EQ(output_seqs[3], 1u);
}

TEST(IntegrationTests_M01, ReassemblerSingleFragmentImmediate)
{
    size_t frame_count = 0;
    std::mutex frame_mutex;

    ReassemblerConfig config{10, 10000, 1024, 1};
    Reassembler reassembler(config, [&](ReassembledFrame &&frame)
                            {
        std::lock_guard<std::mutex> lock(frame_mutex);
        ++frame_count;
        EXPECT_EQ(frame.key.frame_counter, 200u);
        EXPECT_EQ(frame.fragment_count, 1u);
        EXPECT_GT(frame.total_size, 0u); });

    PacketParser parser;
    auto raw = make_data_packet(1, 200, {0xAA, 0xBB, 0xCC});
    auto parsed = parser.parse(raw.data(), raw.size());
    ASSERT_TRUE(parsed.has_value());

    reassembler.process_packet(*parsed);

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(frame_count, 1u);
}

TEST(IntegrationTests_M01, ReassemblerMultiFragmentComplete)
{
    size_t frame_count = 0;
    std::mutex frame_mutex;

    ReassemblerConfig config{10, 10000, 2048, 1};
    Reassembler reassembler(config, [&](ReassembledFrame &&frame)
                            {
        std::lock_guard<std::mutex> lock(frame_mutex);
        ++frame_count;
        EXPECT_EQ(frame.key.frame_counter, 300u);
        EXPECT_EQ(frame.fragment_count, 3u); });

    PacketParser parser;
    auto fragments = make_fragmented_data_packet(10, 300, 3, 512);

    // 乱序提交分片：2, 0, 1
    std::vector<size_t> submit_order = {2, 0, 1};
    for (size_t idx : submit_order)
    {
        auto parsed = parser.parse(fragments[idx].data(), fragments[idx].size());
        ASSERT_TRUE(parsed.has_value());
        reassembler.process_packet(*parsed);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(frame_count, 1u);
}

TEST(IntegrationTests_M01, ReassemblerTimeoutIncompleteFrame)
{
    size_t frame_count = 0;
    std::mutex frame_mutex;

    ReassemblerConfig config{10, 50, 2048, 1}; // 50ms超时
    Reassembler reassembler(config, [&](ReassembledFrame &&)
                            {
        std::lock_guard<std::mutex> lock(frame_mutex);
        ++frame_count; });

    PacketParser parser;
    auto fragments = make_fragmented_data_packet(20, 400, 3, 512);

    // 仅提交2个分片（缺失第1个）
    auto parsed0 = parser.parse(fragments[0].data(), fragments[0].size());
    auto parsed2 = parser.parse(fragments[2].data(), fragments[2].size());

    ASSERT_TRUE(parsed0.has_value());
    ASSERT_TRUE(parsed2.has_value());

    reassembler.process_packet(*parsed0);
    reassembler.process_packet(*parsed2);

    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    reassembler.check_timeouts();

    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    reassembler.check_timeouts();

    // 不完整帧不应输出
    EXPECT_EQ(frame_count, 0u);
}

// ===== 端到端集成测试 =====

TEST(IntegrationTests_M01, EndToEnd_DataPipeline)
{
    std::vector<uint32_t> final_output_seqs;
    std::mutex output_mutex;

    // 构建完整管道：Dispatcher -> Reassembler -> Reorderer
    ReorderConfig reorder_cfg{64, 50, true};
    Reorderer reorderer(reorder_cfg, [&](OrderedPacket &&pkt)
                        {
        std::lock_guard<std::mutex> lock(output_mutex);
        final_output_seqs.push_back(pkt.sequence_number); });

    ReassemblerConfig reasm_cfg{20, 5000, 2048, 1};
    Reassembler reassembler(reasm_cfg, [&](ReassembledFrame &&frame)
                            {
        ParsedPacket pkt;
        pkt.header.sequence_number = frame.key.frame_counter;
        pkt.header.packet_type = static_cast<uint8_t>(PacketType::DATA);
        reorderer.insert(pkt); });

    Dispatcher dispatcher([&](const ParsedPacket &packet)
                          {
        if (packet.header.packet_type == static_cast<uint8_t>(PacketType::DATA)) {
            reassembler.process_packet(packet);
        } });

    PacketParser parser;

    // 提交3个乱序单分片Data包
    std::vector<uint32_t> submit_seqs = {103, 101, 102};
    for (uint32_t seq : submit_seqs)
    {
        auto raw = make_data_packet(seq, seq, {static_cast<uint8_t>(seq)});
        auto parsed = parser.parse(raw.data(), raw.size());
        ASSERT_TRUE(parsed.has_value());
        dispatcher.dispatch(*parsed);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    ASSERT_EQ(final_output_seqs.size(), 3u);
    EXPECT_EQ(final_output_seqs[0], 101u);
    EXPECT_EQ(final_output_seqs[1], 102u);
    EXPECT_EQ(final_output_seqs[2], 103u);
}

TEST(IntegrationTests_M01, EndToEnd_MixedDataAndHeartbeat)
{
    size_t data_output_count = 0;
    size_t heartbeat_handled_count = 0;
    std::mutex output_mutex;

    ReorderConfig reorder_cfg{64, 50, true};
    Reorderer reorderer(reorder_cfg, [&](OrderedPacket &&)
                        {
        std::lock_guard<std::mutex> lock(output_mutex);
        ++data_output_count; });

    ReassemblerConfig reasm_cfg{20, 5000, 2048, 1};
    Reassembler reassembler(reasm_cfg, [&](ReassembledFrame &&frame)
                            {
        ParsedPacket pkt;
        pkt.header.sequence_number = frame.key.frame_counter;
        pkt.header.packet_type = static_cast<uint8_t>(PacketType::DATA);
        reorderer.insert(pkt); });

    Dispatcher dispatcher([&](const ParsedPacket &packet)
                          {
        if (packet.header.packet_type == static_cast<uint8_t>(PacketType::DATA)) {
            reassembler.process_packet(packet);
        } else if (packet.header.packet_type == static_cast<uint8_t>(PacketType::HEARTBEAT)) {
            std::lock_guard<std::mutex> lock(output_mutex);
            ++heartbeat_handled_count;
        } });

    PacketParser parser;

    // 混合Data和Heartbeat
    auto data1 = make_data_packet(1, 501, {0x01});
    auto heartbeat1 = make_heartbeat_packet(2);
    auto data2 = make_data_packet(3, 502, {0x02});
    auto heartbeat2 = make_heartbeat_packet(4);

    for (auto &raw : {data1, heartbeat1, data2, heartbeat2})
    {
        auto parsed = parser.parse(raw.data(), raw.size());
        ASSERT_TRUE(parsed.has_value());
        dispatcher.dispatch(*parsed);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EXPECT_EQ(heartbeat_handled_count, 2u);
    EXPECT_EQ(data_output_count, 2u);
}
