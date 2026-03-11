#define private public
#include "qdgz300/m02_signal_proc/gpu_pipeline.h"
#include "qdgz300/common/plot_batch_utils.h"
#undef private

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

using qdgz300::ErrorCode;
using qdgz300::GPU_INFLIGHT_MAX;
using qdgz300::PlotBatch;
using qdgz300::RawBlock;
using qdgz300::m02::CudaStreamPool;
using qdgz300::m02::GpuPipeline;
using qdgz300::m02::PinnedBufferPool;
using qdgz300::T_GPU_MAX_MS;
using qdgz300::free_plot_batch;

class GpuPipelineInflightTest : public ::testing::Test
{
protected:
    CudaStreamPool streams_;
    PinnedBufferPool bufs_;
    GpuPipeline pipeline_{streams_, bufs_};
};

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
            block.payload = nullptr;
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

TEST_F(GpuPipelineInflightTest, InitialState)
{
    EXPECT_EQ(pipeline_.inflight_count(0), 0u);
    EXPECT_EQ(pipeline_.inflight_count(1), 0u);
    EXPECT_EQ(pipeline_.inflight_count(2), 0u);
    EXPECT_EQ(pipeline_.find_free_slot(0), 0);
    EXPECT_FALSE(pipeline_.has_timeout(0));
}

TEST_F(GpuPipelineInflightTest, InvalidStream)
{
    EXPECT_EQ(pipeline_.inflight_count(3), 0u);
    EXPECT_EQ(pipeline_.inflight_count(255), 0u);
    EXPECT_EQ(pipeline_.find_free_slot(3), -1);
    EXPECT_EQ(pipeline_.find_free_slot(255), -1);
    EXPECT_FALSE(pipeline_.has_timeout(3));
    EXPECT_FALSE(pipeline_.has_timeout(255));
}

TEST_F(GpuPipelineInflightTest, HandleTimeoutEmpty)
{
    pipeline_.handle_timeout(0);
    pipeline_.handle_timeout(255);

    EXPECT_EQ(pipeline_.inflight_count(0), 0u);
    EXPECT_FALSE(pipeline_.has_timeout(0));
}

TEST_F(GpuPipelineInflightTest, FindFreeSlot)
{
    EXPECT_EQ(pipeline_.find_free_slot(0), 0);

    pipeline_.inflight_[0][0].occupied = true;
    EXPECT_EQ(pipeline_.inflight_count(0), 1u);
    EXPECT_EQ(pipeline_.find_free_slot(0), 1);

    pipeline_.inflight_[0][1].occupied = true;
    EXPECT_EQ(pipeline_.inflight_count(0), 2u);
    EXPECT_EQ(pipeline_.find_free_slot(0), -1);
}

TEST_F(GpuPipelineInflightTest, HasTimeout)
{
    auto &slot = pipeline_.inflight_[1][0];
    slot.occupied = true;
    slot.completed = false;
    slot.submit_time = std::chrono::steady_clock::now() - std::chrono::milliseconds(T_GPU_MAX_MS + 1);

    EXPECT_TRUE(pipeline_.has_timeout(1));

    pipeline_.handle_timeout(1);
    EXPECT_EQ(pipeline_.inflight_count(1), 0u);
    EXPECT_FALSE(pipeline_.has_timeout(1));
}

TEST_F(GpuPipelineInflightTest, PollEmptyStream)
{
    PlotBatch *result = nullptr;
    EXPECT_FALSE(pipeline_.poll_completion(1, &result));
    EXPECT_EQ(result, nullptr);
}

TEST_F(GpuPipelineInflightTest, SubmitThenPoll)
{
    auto owned = make_block(77u, 2u, 64, 150.0f);
    ASSERT_NE(owned.block.payload, nullptr);

    EXPECT_EQ(pipeline_.submit_cpi(&owned.block, 0), ErrorCode::OK);
    EXPECT_EQ(pipeline_.inflight_count(0), 1u);
    EXPECT_TRUE(pipeline_.inflight_[0][0].completed);

    PlotBatch *result = nullptr;
    EXPECT_TRUE(pipeline_.poll_completion(0, &result));
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->array_id, owned.block.array_id);
    EXPECT_EQ(result->cpi_seq, owned.block.cpi_seq);
    EXPECT_GE(result->plot_count, 0u);
    EXPECT_EQ(pipeline_.inflight_count(0), 0u);

    free_plot_batch(result);
}

TEST_F(GpuPipelineInflightTest, DoubleSubmitPoll)
{
    auto first = make_block(10u, 1u, 32, 150.0f);
    auto second = make_block(11u, 1u, 32, 150.0f);
    ASSERT_NE(first.block.payload, nullptr);
    ASSERT_NE(second.block.payload, nullptr);

    EXPECT_EQ(pipeline_.submit_cpi(&first.block, 0), ErrorCode::OK);
    EXPECT_EQ(pipeline_.submit_cpi(&second.block, 0), ErrorCode::OK);
    EXPECT_EQ(pipeline_.inflight_count(0), 2u);

    PlotBatch *result = nullptr;
    EXPECT_TRUE(pipeline_.poll_completion(0, &result));
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->cpi_seq, first.block.cpi_seq);
    free_plot_batch(result);

    result = nullptr;
    EXPECT_TRUE(pipeline_.poll_completion(0, &result));
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->cpi_seq, second.block.cpi_seq);
    free_plot_batch(result);

    EXPECT_EQ(pipeline_.inflight_count(0), 0u);
}

TEST_F(GpuPipelineInflightTest, HandleTimeoutClears)
{
    auto owned = make_block(88u, 1u, 32, 150.0f);
    ASSERT_NE(owned.block.payload, nullptr);

    EXPECT_EQ(pipeline_.submit_cpi(&owned.block, 0), ErrorCode::OK);
    ASSERT_NE(pipeline_.inflight_[0][0].output, nullptr);

    PlotBatch *timed_out_batch = pipeline_.inflight_[0][0].output;
    pipeline_.inflight_[0][0].completed = false;
    pipeline_.inflight_[0][0].submit_time =
        std::chrono::steady_clock::now() - std::chrono::milliseconds(100);

    EXPECT_TRUE(pipeline_.has_timeout(0));
    pipeline_.handle_timeout(0);

    EXPECT_EQ(pipeline_.inflight_count(0), 0u);
    EXPECT_FALSE(pipeline_.has_timeout(0));
    EXPECT_TRUE((timed_out_batch->flags & RawBlock::FLAG_GPU_TIMEOUT) != 0u);

    free_plot_batch(timed_out_batch);
}

TEST_F(GpuPipelineInflightTest, InflightFullReject)
{
    auto first = make_block(20u, 1u, 16, 150.0f);
    auto second = make_block(21u, 1u, 16, 150.0f);
    auto third = make_block(22u, 1u, 16, 150.0f);
    ASSERT_NE(first.block.payload, nullptr);
    ASSERT_NE(second.block.payload, nullptr);
    ASSERT_NE(third.block.payload, nullptr);

    EXPECT_EQ(pipeline_.submit_cpi(&first.block, 0), ErrorCode::OK);
    EXPECT_EQ(pipeline_.submit_cpi(&second.block, 0), ErrorCode::OK);
    EXPECT_EQ(pipeline_.inflight_count(0), static_cast<uint32_t>(GPU_INFLIGHT_MAX));
    EXPECT_EQ(pipeline_.submit_cpi(&third.block, 0), ErrorCode::GPU_H2D_FAILED);
}

TEST_F(GpuPipelineInflightTest, SubmitCpiHandlesMisalignedTailBytes)
{
    auto iq = make_iq_payload(8, 50.0f);

    RawBlock block{};
    block.data_ts = 1u;
    block.array_id = 1u;
    block.cpi_seq = 2u;
    block.data_size = static_cast<uint32_t>(iq.size() * sizeof(float) - 3);
    block.payload = alloc_payload_copy(iq);
    ASSERT_NE(block.payload, nullptr);

    EXPECT_EQ(pipeline_.submit_cpi(&block, 0), ErrorCode::OK);

    PlotBatch *result = nullptr;
    EXPECT_TRUE(pipeline_.poll_completion(0, &result));
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->plot_count, 0u);

    free_plot_batch(result);
    std::free(block.payload);
}

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
