#include "qdgz300/common/constants.h"
#include "qdgz300/common/plot_batch_utils.h"
#include "qdgz300/common/spsc_queue.h"
#include "qdgz300/common/types.h"
#include "qdgz300/m02_signal_proc/cuda_stream_pool.h"
#include "qdgz300/m02_signal_proc/gpu_dispatcher.h"
#include "qdgz300/m02_signal_proc/gpu_pipeline.h"
#include "qdgz300/m02_signal_proc/pinned_buffer_pool.h"
#include "qdgz300/m02_signal_proc/signal_proc_metrics.h"

#include <array>
#include <chrono>
#include <cmath>
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
using qdgz300::RAW_BLOCK_PAYLOAD_SIZE;
using qdgz300::PlotBatch;
using qdgz300::RawBlock;
using qdgz300::free_plot_batch;
using qdgz300::m02::CudaStreamPool;
using qdgz300::m02::GpuDispatcher;
using qdgz300::m02::GpuPipeline;
using qdgz300::m02::PinnedBufferPool;
using qdgz300::m02::SignalProcMetrics;

using RawQueue = qdgz300::SPSCQueue<RawBlock *, RAWCPI_Q_CAPACITY>;
using PlotQueue = qdgz300::SPSCQueue<PlotBatch *, PLOTS_Q_CAPACITY>;

namespace
{
    constexpr size_t kPayloadBytes = 1024;
    constexpr float kSignalAmplitude = 200.0f;
    constexpr double kPi = 3.14159265358979323846;
}

class GpuDispatcherIntegrationTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        stream_pool_.init();
        buffer_pool_.init(RAW_BLOCK_PAYLOAD_SIZE, GPU_STREAM_COUNT, GPU_INFLIGHT_MAX);
        pipeline_ = std::make_unique<GpuPipeline>(stream_pool_, buffer_pool_);
        dispatcher_ = std::make_unique<GpuDispatcher>(
            rawcpi_qs_.data(), plots_qs_.data(), *pipeline_, metrics_);
    }

    void TearDown() override
    {
        if (dispatcher_)
            dispatcher_->stop();

        for (uint8_t i = 0; i < GPU_STREAM_COUNT; ++i)
        {
            while (auto opt = plots_qs_[i].try_pop())
                free_plot_batch(opt.value());
        }

        pipeline_.reset();
        buffer_pool_.destroy();
        stream_pool_.destroy();

        for (RawBlock *block : allocated_blocks_)
        {
            if (!block)
                continue;
            std::free(block->payload);
            block->payload = nullptr;
            delete block;
        }
        allocated_blocks_.clear();
    }

    void fill_sine_iq(uint8_t *buf, size_t size)
    {
        auto *iq = reinterpret_cast<float *>(buf);
        const size_t samples = size / (2 * sizeof(float));
        for (size_t i = 0; i < samples; ++i)
        {
            const float t = static_cast<float>(i) / static_cast<float>(samples == 0 ? 1 : samples);
            iq[2 * i] = kSignalAmplitude * std::sin(static_cast<float>(2.0 * kPi * t));
            iq[2 * i + 1] = kSignalAmplitude * std::cos(static_cast<float>(2.0 * kPi * t));
        }
    }

    void push_test_blocks(uint8_t queue_idx, uint32_t count, uint32_t start_seq)
    {
        ASSERT_LT(queue_idx, GPU_STREAM_COUNT);

        for (uint32_t i = 0; i < count; ++i)
        {
            auto *block = new RawBlock{};
            block->array_id = static_cast<uint8_t>(queue_idx + 1);
            block->cpi_seq = start_seq + i;
            block->data_size = static_cast<uint32_t>(kPayloadBytes);
            block->data_ts = 100000u + i;
            block->ingest_ts = 200000u + i;
            block->payload = static_cast<uint8_t *>(aligned_alloc(64, kPayloadBytes));
            ASSERT_NE(block->payload, nullptr);
            fill_sine_iq(block->payload, kPayloadBytes);

            allocated_blocks_.push_back(block);
            rawcpi_qs_[queue_idx].drop_oldest_push(block);
        }
    }

    size_t drain_plot_queue(uint8_t queue_idx)
    {
        size_t count = 0;
        while (auto opt = plots_qs_[queue_idx].try_pop())
        {
            free_plot_batch(opt.value());
            ++count;
        }
        return count;
    }

    std::array<RawQueue, GPU_STREAM_COUNT> rawcpi_qs_{};
    std::array<PlotQueue, GPU_STREAM_COUNT> plots_qs_{};
    CudaStreamPool stream_pool_;
    PinnedBufferPool buffer_pool_;
    std::unique_ptr<GpuPipeline> pipeline_;
    SignalProcMetrics metrics_;
    std::unique_ptr<GpuDispatcher> dispatcher_;
    std::vector<RawBlock *> allocated_blocks_{};
};

TEST_F(GpuDispatcherIntegrationTest, SingleStreamProcessing)
{
    push_test_blocks(0, 5, 100);

    dispatcher_->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    dispatcher_->stop();

    EXPECT_EQ(drain_plot_queue(0), 5u);
}

TEST_F(GpuDispatcherIntegrationTest, ThreeStreamParallel)
{
    push_test_blocks(0, 10, 0);
    push_test_blocks(1, 10, 100);
    push_test_blocks(2, 10, 200);

    dispatcher_->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    dispatcher_->stop();

    for (uint8_t i = 0; i < GPU_STREAM_COUNT; ++i)
    {
        size_t count = 0;
        while (auto opt = plots_qs_[i].try_pop())
        {
            PlotBatch *batch = opt.value();
            ASSERT_NE(batch, nullptr);
            EXPECT_EQ(batch->array_id, i + 1);
            EXPECT_GE(batch->plot_count, 0u);
            free_plot_batch(batch);
            ++count;
        }
        EXPECT_EQ(count, 10u);
    }
}

TEST_F(GpuDispatcherIntegrationTest, MetricsAccumulation)
{
    push_test_blocks(0, 3, 0);

    dispatcher_->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    dispatcher_->stop();

    EXPECT_GE(metrics_.gpu_frames_submitted.get(), 3u);
    EXPECT_GE(metrics_.gpu_frames_completed.get(), 3u);
}

TEST_F(GpuDispatcherIntegrationTest, EmptyQueuesNoBusyWait)
{
    dispatcher_->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const auto stop_begin = std::chrono::steady_clock::now();
    dispatcher_->stop();
    const auto stop_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - stop_begin);

    EXPECT_LT(stop_elapsed.count(), 100);
}

TEST_F(GpuDispatcherIntegrationTest, StartStopIdempotent)
{
    dispatcher_->start();
    dispatcher_->stop();
    dispatcher_->start();
    dispatcher_->stop();
}

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
