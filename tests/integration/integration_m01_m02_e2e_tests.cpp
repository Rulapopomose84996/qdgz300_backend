#include "qdgz300/common/plot_batch_utils.h"
#include "qdgz300/common/spsc_queue.h"
#include "qdgz300/common/types.h"
#include "qdgz300/m01_receiver/delivery/rawblock_adapter.h"
#include "qdgz300/m01_receiver/pipeline/dispatcher.h"
#include "qdgz300/m01_receiver/pipeline/reassembler.h"
#include "qdgz300/m01_receiver/pipeline/reorderer.h"
#include "qdgz300/m01_receiver/protocol/packet_parser.h"
#include "qdgz300/m01_receiver/protocol/protocol_types.h"
#include "qdgz300/m02_signal_proc/cuda_stream_pool.h"
#include "qdgz300/m02_signal_proc/gpu_dispatcher.h"
#include "qdgz300/m02_signal_proc/gpu_pipeline.h"
#include "qdgz300/m02_signal_proc/pinned_buffer_pool.h"
#include "qdgz300/m02_signal_proc/signal_proc_metrics.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

using qdgz300::GPU_INFLIGHT_MAX;
using qdgz300::GPU_STREAM_COUNT;
using qdgz300::PLOTS_Q_CAPACITY;
using qdgz300::RAWCPI_Q_CAPACITY;
using qdgz300::PlotBatch;
using qdgz300::RawBlock;
using qdgz300::free_plot_batch;
using qdgz300::m01::delivery::RawBlockAdapter;
using qdgz300::m02::CudaStreamPool;
using qdgz300::m02::GpuDispatcher;
using qdgz300::m02::GpuPipeline;
using qdgz300::m02::PinnedBufferPool;
using qdgz300::m02::SignalProcMetrics;
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

using M02RawQueue = qdgz300::SPSCQueue<RawBlock *, RAWCPI_Q_CAPACITY>;
using PlotQueue = qdgz300::SPSCQueue<PlotBatch *, PLOTS_Q_CAPACITY>;

namespace
{
    struct ParsedPacketWithStorage
    {
        std::vector<uint8_t> raw;
        ParsedPacket parsed;
    };

    struct BridgeEntry
    {
        std::shared_ptr<receiver::delivery::RawBlock> owner;
        std::unique_ptr<RawBlock> bridged;
    };

    std::vector<uint8_t> make_iq_payload(size_t size_bytes, uint8_t seed)
    {
        std::vector<uint8_t> payload(size_bytes, 0);
        auto *iq = reinterpret_cast<float *>(payload.data());
        const size_t samples = size_bytes / (2 * sizeof(float));
        for (size_t i = 0; i < samples; ++i)
        {
            const float t = static_cast<float>(i) / static_cast<float>(samples == 0 ? 1 : samples);
            iq[2 * i] = 200.0f * std::sin(6.283185307179586f * t);
            iq[2 * i + 1] = 200.0f * std::cos(6.283185307179586f * t);
        }
        if (!payload.empty())
            payload.back() = seed;
        return payload;
    }

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
            std::memcpy(packet.data() + offset, payload.data(), payload.size());

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
            out.parsed = *parsed;
        return out;
    }
} // namespace

class M01M02EndToEndTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        rawblock_queue_ = std::make_shared<RawBlockAdapter::RawBlockQueue>();
        rawblock_adapter_ = std::make_unique<RawBlockAdapter>(rawblock_queue_, 1);

        ReorderConfig reorder_cfg{};
        reorder_cfg.window_size = 64;
        reorder_cfg.timeout_ms = 100;
        reorder_cfg.enable_zero_fill = true;
        reorderer_ = std::make_unique<Reorderer>(
            reorder_cfg,
            [this](OrderedPacket &&pkt)
            {
                rawblock_adapter_->adapt_and_push(std::move(pkt));
            });

        ReassemblerConfig reasm_cfg{};
        reasm_cfg.timeout_ms = 100;
        reasm_cfg.max_contexts = 64;
        reasm_cfg.max_total_frags = 1024;
        reasm_cfg.sample_count_fixed = 4;
        reasm_cfg.max_reasm_bytes_per_key = 16u * 1024u * 1024u;
        reasm_cfg.numa_node = 0;
        reasm_cfg.cache_align_bytes = 64;
        reasm_cfg.prefetch_hints_enabled = false;
        Reorderer *reorderer_ptr = reorderer_.get();
        reassembler_ = std::make_unique<Reassembler>(
            reasm_cfg,
            [reorderer_ptr](ReassembledFrame &&frame)
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
                reorderer_ptr->insert_owned(header, std::move(frame.data), frame.total_size);
            });

        Reassembler *reassembler_ptr = reassembler_.get();
        dispatcher_m01_ = std::make_unique<Dispatcher>(
            [reassembler_ptr](const ParsedPacket &pkt)
            {
                reassembler_ptr->process_packet(pkt);
            },
            [](const ParsedPacket &)
            {
            });

        stream_pool_.init();
        buffer_pool_.init(4096, GPU_STREAM_COUNT, GPU_INFLIGHT_MAX);
        pipeline_ = std::make_unique<GpuPipeline>(stream_pool_, buffer_pool_);
        dispatcher_m02_ = std::make_unique<GpuDispatcher>(
            m02_raw_qs_.data(), plots_qs_.data(), *pipeline_, metrics_);
    }

    void TearDown() override
    {
        if (dispatcher_m02_)
            dispatcher_m02_->stop();

        for (auto &queue : plots_qs_)
        {
            while (auto opt = queue.try_pop())
                free_plot_batch(opt.value());
        }

        bridge_entries_.clear();
        pipeline_.reset();
        buffer_pool_.destroy();
        stream_pool_.destroy();
    }

    void dispatch_packet(std::vector<uint8_t> raw)
    {
        auto parsed = parse_packet(std::move(raw));
        dispatcher_m01_->dispatch(parsed.parsed);
    }

    std::shared_ptr<receiver::delivery::RawBlock> wait_and_pop_rawblock(
        std::chrono::milliseconds timeout = std::chrono::milliseconds(500))
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            auto item = rawblock_queue_->try_pop();
            if (item.has_value())
                return item.value();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return nullptr;
    }

    RawBlock *bridge_to_m02(const std::shared_ptr<receiver::delivery::RawBlock> &src)
    {
        BridgeEntry entry{};
        entry.owner = src;
        entry.bridged = std::make_unique<RawBlock>();
        entry.bridged->ingest_ts = src->ingest_ts;
        entry.bridged->data_ts = src->data_ts;
        entry.bridged->array_id = src->array_id;
        entry.bridged->cpi_seq = src->cpi_seq;
        entry.bridged->fragment_count = src->fragment_count;
        entry.bridged->data_size = src->data_size;
        entry.bridged->flags = src->flags;
        entry.bridged->payload = const_cast<uint8_t *>(src->payload);

        RawBlock *raw = entry.bridged.get();
        bridge_entries_.push_back(std::move(entry));
        return raw;
    }

    PlotBatch *wait_and_pop_plot(uint8_t queue_idx,
                                 std::chrono::milliseconds timeout = std::chrono::milliseconds(1000))
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            auto item = plots_qs_[queue_idx].try_pop();
            if (item.has_value())
                return item.value();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return nullptr;
    }

    void bridge_all_available(uint8_t queue_idx)
    {
        while (auto block = rawblock_queue_->try_pop())
            m02_raw_qs_[queue_idx].drop_oldest_push(bridge_to_m02(block.value()));
    }

    std::shared_ptr<RawBlockAdapter::RawBlockQueue> rawblock_queue_;
    std::unique_ptr<RawBlockAdapter> rawblock_adapter_;
    std::unique_ptr<Reorderer> reorderer_;
    std::unique_ptr<Reassembler> reassembler_;
    std::unique_ptr<Dispatcher> dispatcher_m01_;

    std::array<M02RawQueue, GPU_STREAM_COUNT> m02_raw_qs_{};
    std::array<PlotQueue, GPU_STREAM_COUNT> plots_qs_{};
    CudaStreamPool stream_pool_;
    PinnedBufferPool buffer_pool_;
    std::unique_ptr<GpuPipeline> pipeline_;
    SignalProcMetrics metrics_{};
    std::unique_ptr<GpuDispatcher> dispatcher_m02_;
    std::vector<BridgeEntry> bridge_entries_{};
};

TEST_F(M01M02EndToEndTest, SingleCpiFullPipeline)
{
    dispatch_packet(make_data_packet(42, 42, 0, 1, make_iq_payload(1024, 0x11), 1024));

    auto block = wait_and_pop_rawblock();
    ASSERT_NE(block, nullptr);
    ASSERT_EQ(block->cpi_seq, 42u);

    RawBlock *bridged = bridge_to_m02(block);
    ASSERT_EQ(pipeline_->submit_cpi(bridged, 0), qdgz300::ErrorCode::OK);

    PlotBatch *result = nullptr;
    ASSERT_TRUE(pipeline_->poll_completion(0, &result));
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->array_id, 1);
    EXPECT_EQ(result->cpi_seq, 42u);
    EXPECT_NE(result->data_ts, 0u);

    free_plot_batch(result);
}

TEST_F(M01M02EndToEndTest, MultiFragmentCpiFullPipeline)
{
    const std::vector<std::vector<uint8_t>> fragments = {
        make_iq_payload(16, 0x21),
        make_iq_payload(16, 0x22),
        make_iq_payload(16, 0x23),
        make_iq_payload(10, 0x24)};

    for (uint16_t frag = 0; frag < fragments.size(); ++frag)
    {
        dispatch_packet(make_data_packet(
            100 + frag,
            100,
            frag,
            static_cast<uint16_t>(fragments.size()),
            fragments[frag],
            static_cast<uint16_t>(fragments.back().size())));
    }

    auto block = wait_and_pop_rawblock();
    ASSERT_NE(block, nullptr);
    EXPECT_EQ(block->data_size, 16u + 16u + 16u + 10u);

    RawBlock *bridged = bridge_to_m02(block);
    ASSERT_EQ(pipeline_->submit_cpi(bridged, 0), qdgz300::ErrorCode::OK);

    PlotBatch *result = nullptr;
    ASSERT_TRUE(pipeline_->poll_completion(0, &result));
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->cpi_seq, 100u);

    free_plot_batch(result);
}

TEST_F(M01M02EndToEndTest, MultiCpiSequence)
{
    for (uint32_t seq = 0; seq < 20; ++seq)
    {
        dispatch_packet(make_data_packet(seq, seq, 0, 1, make_iq_payload(512, static_cast<uint8_t>(seq)), 512));
    }

    bridge_all_available(0);
    dispatcher_m02_->start();

    std::vector<uint32_t> observed;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    while (observed.size() < 20 && std::chrono::steady_clock::now() < deadline)
    {
        if (auto *batch = wait_and_pop_plot(0, std::chrono::milliseconds(20)))
        {
            observed.push_back(batch->cpi_seq);
            free_plot_batch(batch);
        }
        bridge_all_available(0);
    }

    dispatcher_m02_->stop();

    ASSERT_EQ(observed.size(), 20u);
    for (size_t i = 0; i < observed.size(); ++i)
        EXPECT_EQ(observed[i], i);
}

TEST_F(M01M02EndToEndTest, SharedPtrToRawPtrBridge)
{
    dispatch_packet(make_data_packet(7, 7, 0, 1, make_iq_payload(1024, 0x31), 1024));

    auto block = wait_and_pop_rawblock();
    ASSERT_NE(block, nullptr);
    ASSERT_EQ(block.use_count(), 1);

    RawBlock *bridged = bridge_to_m02(block);
    EXPECT_GE(block.use_count(), 2);

    ASSERT_EQ(pipeline_->submit_cpi(bridged, 0), qdgz300::ErrorCode::OK);
    PlotBatch *result = nullptr;
    ASSERT_TRUE(pipeline_->poll_completion(0, &result));
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(block->cpi_seq, 7u);
    EXPECT_EQ(block->data_size, 1024u);

    free_plot_batch(result);
}

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
