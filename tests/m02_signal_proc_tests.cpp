#include "qdgz300/common/constants.h"
#include "qdgz300/common/error_codes.h"
#include "qdgz300/common/plot_batch_utils.h"
#include "qdgz300/common/types.h"
#include "qdgz300/m02_signal_proc/cuda_stream_pool.h"
#include "qdgz300/m02_signal_proc/gpu_pipeline.h"
#include "qdgz300/m02_signal_proc/pinned_buffer_pool.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>

#include <gtest/gtest.h>

using qdgz300::ErrorCode;
using qdgz300::GPU_INFLIGHT_MAX;
using qdgz300::GPU_STREAM_COUNT;
using qdgz300::PlotBatch;
using qdgz300::RAW_BLOCK_PAYLOAD_SIZE;
using qdgz300::RawBlock;
using qdgz300::free_plot_batch;
using qdgz300::m02::CudaStreamPool;
using qdgz300::m02::GpuPipeline;
using qdgz300::m02::PinnedBufferPool;

namespace
{
    constexpr size_t kTestPayloadSize = 8192;
    constexpr float kSignalAmplitude = 200.0f;
    constexpr double kPi = 3.14159265358979323846;
}

class GpuPipelineCpuTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        stream_pool_.init();
        buffer_pool_.init(RAW_BLOCK_PAYLOAD_SIZE, GPU_STREAM_COUNT, GPU_INFLIGHT_MAX);
        pipeline_ = std::make_unique<GpuPipeline>(stream_pool_, buffer_pool_);
    }

    void TearDown() override
    {
        pipeline_.reset();
        buffer_pool_.destroy();
        stream_pool_.destroy();
    }

    RawBlock make_test_block(uint8_t payload_slot,
                             uint8_t array_id,
                             uint32_t cpi_seq,
                             size_t payload_size)
    {
        EXPECT_LT(payload_slot, payload_store_.size());
        EXPECT_LE(payload_size, payload_store_[payload_slot].size());

        RawBlock block{};
        block.array_id = array_id;
        block.cpi_seq = cpi_seq;
        block.data_size = static_cast<uint32_t>(payload_size);
        block.data_ts = 1234567890ULL + cpi_seq;
        block.ingest_ts = 9876543210ULL + cpi_seq;
        block.flags = 0;
        block.payload = payload_store_[payload_slot].data();
        fill_sine_wave(payload_store_[payload_slot].data(), payload_size);
        return block;
    }

    void fill_sine_wave(uint8_t *buf, size_t size)
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

    CudaStreamPool stream_pool_;
    PinnedBufferPool buffer_pool_;
    std::unique_ptr<GpuPipeline> pipeline_;
    std::array<std::array<uint8_t, kTestPayloadSize>, GPU_STREAM_COUNT> payload_store_{};
};

TEST_F(GpuPipelineCpuTest, SubmitAndPollSingleCpi)
{
    auto block = make_test_block(0, 1, 42, 4096);

    const auto ec = pipeline_->submit_cpi(&block, 0);
    EXPECT_EQ(ec, ErrorCode::OK);
    EXPECT_EQ(pipeline_->inflight_count(0), 1u);

    PlotBatch *result = nullptr;
    EXPECT_TRUE(pipeline_->poll_completion(0, &result));
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->array_id, 1);
    EXPECT_EQ(result->cpi_seq, 42u);
    EXPECT_EQ(result->data_ts, block.data_ts);
    EXPECT_GE(result->plot_count, 0u);
    EXPECT_EQ(pipeline_->inflight_count(0), 0u);

    free_plot_batch(result);
}

TEST_F(GpuPipelineCpuTest, PlotFieldsCorrect)
{
    auto block = make_test_block(0, 2, 99, 4096);

    ASSERT_EQ(pipeline_->submit_cpi(&block, 1), ErrorCode::OK);

    PlotBatch *result = nullptr;
    ASSERT_TRUE(pipeline_->poll_completion(1, &result));
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->array_id, 2);

    for (uint16_t i = 0; i < result->plot_count; ++i)
    {
        EXPECT_EQ(result->plots[i].array_id, 2);
        EXPECT_GT(result->plots[i].amplitude, 100.0f);
        EXPECT_GE(result->plots[i].snr_db, 0.0f);
    }

    free_plot_batch(result);
}

TEST_F(GpuPipelineCpuTest, EmptyPayload)
{
    RawBlock block{};
    block.data_ts = 1234567890ULL;
    block.ingest_ts = 9876543210ULL;
    block.data_size = 0;
    block.payload = nullptr;

    EXPECT_EQ(pipeline_->submit_cpi(&block, 0), ErrorCode::OK);

    PlotBatch *result = nullptr;
    ASSERT_TRUE(pipeline_->poll_completion(0, &result));
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->plot_count, 0u);

    free_plot_batch(result);
}

TEST_F(GpuPipelineCpuTest, NullBlock)
{
    const auto ec = pipeline_->submit_cpi(nullptr, 0);
    EXPECT_EQ(ec, ErrorCode::GPU_H2D_FAILED);
}

TEST_F(GpuPipelineCpuTest, IncompleteFrameFlag)
{
    auto block = make_test_block(0, 1, 10, 4096);
    block.flags = RawBlock::FLAG_INCOMPLETE_FRAME;

    ASSERT_EQ(pipeline_->submit_cpi(&block, 0), ErrorCode::OK);

    PlotBatch *result = nullptr;
    ASSERT_TRUE(pipeline_->poll_completion(0, &result));
    ASSERT_NE(result, nullptr);
    EXPECT_NE(result->flags & RawBlock::FLAG_INCOMPLETE_FRAME, 0u);

    free_plot_batch(result);
}

TEST_F(GpuPipelineCpuTest, MultiStreamIndependence)
{
    auto block0 = make_test_block(0, 1, 100, 4096);
    auto block1 = make_test_block(1, 2, 200, 4096);
    auto block2 = make_test_block(2, 3, 300, 4096);

    ASSERT_EQ(pipeline_->submit_cpi(&block0, 0), ErrorCode::OK);
    ASSERT_EQ(pipeline_->submit_cpi(&block1, 1), ErrorCode::OK);
    ASSERT_EQ(pipeline_->submit_cpi(&block2, 2), ErrorCode::OK);

    PlotBatch *result0 = nullptr;
    PlotBatch *result1 = nullptr;
    PlotBatch *result2 = nullptr;

    ASSERT_TRUE(pipeline_->poll_completion(0, &result0));
    ASSERT_TRUE(pipeline_->poll_completion(1, &result1));
    ASSERT_TRUE(pipeline_->poll_completion(2, &result2));
    ASSERT_NE(result0, nullptr);
    ASSERT_NE(result1, nullptr);
    ASSERT_NE(result2, nullptr);

    EXPECT_EQ(result0->cpi_seq, 100u);
    EXPECT_EQ(result1->cpi_seq, 200u);
    EXPECT_EQ(result2->cpi_seq, 300u);
    EXPECT_EQ(result0->array_id, 1);
    EXPECT_EQ(result1->array_id, 2);
    EXPECT_EQ(result2->array_id, 3);

    free_plot_batch(result0);
    free_plot_batch(result1);
    free_plot_batch(result2);
}

TEST_F(GpuPipelineCpuTest, InflightMaxReject)
{
    auto block0 = make_test_block(0, 1, 11, 4096);
    auto block1 = make_test_block(1, 1, 12, 4096);
    auto block2 = make_test_block(2, 1, 13, 4096);

    ASSERT_EQ(pipeline_->submit_cpi(&block0, 0), ErrorCode::OK);
    ASSERT_EQ(pipeline_->submit_cpi(&block1, 0), ErrorCode::OK);
    EXPECT_EQ(pipeline_->submit_cpi(&block2, 0), ErrorCode::GPU_H2D_FAILED);

    PlotBatch *result = nullptr;
    ASSERT_TRUE(pipeline_->poll_completion(0, &result));
    ASSERT_NE(result, nullptr);
    free_plot_batch(result);

    EXPECT_EQ(pipeline_->submit_cpi(&block2, 0), ErrorCode::OK);

    result = nullptr;
    ASSERT_TRUE(pipeline_->poll_completion(0, &result));
    ASSERT_NE(result, nullptr);
    free_plot_batch(result);

    result = nullptr;
    ASSERT_TRUE(pipeline_->poll_completion(0, &result));
    ASSERT_NE(result, nullptr);
    free_plot_batch(result);
}

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
