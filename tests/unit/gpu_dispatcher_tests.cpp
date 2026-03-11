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
using qdgz300::m02::CudaStreamPool;
using qdgz300::m02::GpuDispatcher;
using qdgz300::m02::GpuPipeline;
using qdgz300::m02::PinnedBufferPool;
using qdgz300::m02::SignalProcMetrics;

using RawQueue = qdgz300::SPSCQueue<RawBlock *, RAWCPI_Q_CAPACITY>;
using PlotQueue = qdgz300::SPSCQueue<PlotBatch *, PLOTS_Q_CAPACITY>;

namespace
{
    struct OwnedBlock
    {
        RawBlock block{};

        OwnedBlock() = default;
        OwnedBlock(const OwnedBlock &) = delete;
        OwnedBlock &operator=(const OwnedBlock &) = delete;
        OwnedBlock(OwnedBlock &&other) noexcept : block(other.block)
        {
            other.block.payload = nullptr;
        }
        OwnedBlock &operator=(OwnedBlock &&other) noexcept
        {
            if (this != &other)
            {
                std::free(block.payload);
                block = other.block;
                other.block.payload = nullptr;
            }
            return *this;
        }
        ~OwnedBlock()
        {
            std::free(block.payload);
        }
    };

    std::vector<float> make_iq_payload(size_t sample_count, float amplitude)
    {
        std::vector<float> iq(sample_count * 2, 0.0f);
        for (size_t i = 0; i < sample_count; ++i)
        {
            iq[2 * i] = amplitude * std::sin(static_cast<float>(i) * 0.1f);
            iq[2 * i + 1] = amplitude * std::cos(static_cast<float>(i) * 0.1f);
        }
        return iq;
    }

    uint8_t *alloc_payload_copy(const std::vector<float> &iq)
    {
        const size_t bytes = iq.size() * sizeof(float);
        void *ptr = aligned_alloc(64, ((bytes + 63) / 64) * 64);
        if (!ptr)
            return nullptr;

        std::memcpy(ptr, iq.data(), bytes);
        return static_cast<uint8_t *>(ptr);
    }

    OwnedBlock make_block(uint32_t cpi_seq, uint8_t array_id, size_t sample_count, float amplitude)
    {
        auto iq = make_iq_payload(sample_count, amplitude);

        OwnedBlock owned{};
        owned.block.data_ts = 1000u + cpi_seq;
        owned.block.array_id = array_id;
        owned.block.cpi_seq = cpi_seq;
        owned.block.data_size = static_cast<uint32_t>(iq.size() * sizeof(float));
        owned.block.payload = alloc_payload_copy(iq);
        return owned;
    }
} // namespace

class GpuDispatcherTest : public ::testing::Test
{
protected:
    GpuDispatcherTest()
        : dispatcher_(raw_queues_.data(), plot_queues_.data(), pipeline_, metrics_)
    {
    }

    void SetUp() override
    {
        bufs_.init(256, GPU_STREAM_COUNT, GPU_INFLIGHT_MAX);
        streams_.init();
    }

    void TearDown() override
    {
        dispatcher_.stop();

        for (auto &queue : plot_queues_)
        {
            while (auto batch = queue.try_pop())
                free_plot_batch(batch.value());
        }

        bufs_.destroy();
        streams_.destroy();
    }

    std::array<RawQueue, GPU_STREAM_COUNT> raw_queues_{};
    std::array<PlotQueue, GPU_STREAM_COUNT> plot_queues_{};
    CudaStreamPool streams_{};
    PinnedBufferPool bufs_{};
    GpuPipeline pipeline_{streams_, bufs_};
    SignalProcMetrics metrics_{};
    GpuDispatcher dispatcher_;
};

TEST_F(GpuDispatcherTest, StartStopDoesNotHang)
{
    dispatcher_.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    dispatcher_.stop();
}

TEST_F(GpuDispatcherTest, DispatchesRawBlockToPlotQueue)
{
    auto block = make_block(42u, 1u, 64, 150.0f);
    ASSERT_NE(block.block.payload, nullptr);
    ASSERT_TRUE(raw_queues_[0].try_push(&block.block));

    dispatcher_.start();

    PlotBatch *result = nullptr;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (std::chrono::steady_clock::now() < deadline)
    {
        auto popped = plot_queues_[0].try_pop();
        if (popped.has_value())
        {
            result = popped.value();
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    dispatcher_.stop();

    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->cpi_seq, block.block.cpi_seq);
    EXPECT_EQ(result->array_id, block.block.array_id);
    EXPECT_GE(result->plot_count, 0u);
    EXPECT_EQ(metrics_.gpu_frames_submitted.get(), 1u);
    EXPECT_EQ(metrics_.gpu_frames_completed.get(), 1u);
    EXPECT_EQ(metrics_.gpu_timeout_total.get(), 0u);

    free_plot_batch(result);
}

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
