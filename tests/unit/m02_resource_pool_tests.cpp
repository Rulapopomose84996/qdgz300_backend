#include "qdgz300/common/constants.h"
#include "qdgz300/m02_signal_proc/cuda_stream_pool.h"
#include "qdgz300/m02_signal_proc/pinned_buffer_pool.h"

#include <gtest/gtest.h>

using qdgz300::GPU_INFLIGHT_MAX;
using qdgz300::GPU_STREAM_COUNT;
using qdgz300::RAW_BLOCK_PAYLOAD_SIZE;
using qdgz300::m02::CudaStreamPool;
using qdgz300::m02::PinnedBufferPool;

TEST(M02ResourcePoolTest, CudaStreamPoolInitDestroy)
{
    CudaStreamPool pool;
    pool.init();
    pool.destroy();
    pool.destroy();
}

TEST(M02ResourcePoolTest, CudaStreamPoolGetStreamReturnsNullInCpuMode)
{
    CudaStreamPool pool;
    pool.init();

    EXPECT_EQ(pool.get_stream(0), nullptr);
    EXPECT_EQ(pool.get_event(0), nullptr);

    pool.destroy();
}

TEST(M02ResourcePoolTest, CudaStreamPoolGetStreamOutOfRange)
{
    CudaStreamPool pool;
    pool.init();

    EXPECT_EQ(pool.get_stream(static_cast<uint8_t>(GPU_STREAM_COUNT)), nullptr);
    EXPECT_EQ(pool.get_event(static_cast<uint8_t>(GPU_STREAM_COUNT)), nullptr);

    pool.destroy();
}

TEST(M02ResourcePoolTest, PinnedBufferPoolInitGetDestroy)
{
    PinnedBufferPool pool;
    pool.init(RAW_BLOCK_PAYLOAD_SIZE, GPU_STREAM_COUNT, GPU_INFLIGHT_MAX);

    auto *h2d = pool.get_h2d_buffer(0, 0);
    auto *d2h = pool.get_d2h_buffer(0, 0);
    EXPECT_NE(h2d, nullptr);
    EXPECT_NE(d2h, nullptr);

    pool.destroy();
    EXPECT_EQ(pool.get_h2d_buffer(0, 0), nullptr);
    EXPECT_EQ(pool.get_d2h_buffer(0, 0), nullptr);

    pool.destroy();
}

TEST(M02ResourcePoolTest, PinnedBufferPoolDistinctBuffers)
{
    PinnedBufferPool pool;
    pool.init(RAW_BLOCK_PAYLOAD_SIZE, GPU_STREAM_COUNT, GPU_INFLIGHT_MAX);

    auto *a = pool.get_h2d_buffer(0, 0);
    auto *b = pool.get_h2d_buffer(0, 1);
    auto *c = pool.get_h2d_buffer(1, 0);

    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(c, nullptr);
    EXPECT_NE(a, b);
    EXPECT_NE(a, c);
    EXPECT_NE(b, c);

    pool.destroy();
}

TEST(M02ResourcePoolTest, PinnedBufferPoolOutOfRange)
{
    PinnedBufferPool pool;
    pool.init(RAW_BLOCK_PAYLOAD_SIZE, GPU_STREAM_COUNT, GPU_INFLIGHT_MAX);

    EXPECT_EQ(pool.get_h2d_buffer(static_cast<uint8_t>(GPU_STREAM_COUNT), 0), nullptr);
    EXPECT_EQ(pool.get_h2d_buffer(0, static_cast<uint8_t>(GPU_INFLIGHT_MAX)), nullptr);
    EXPECT_EQ(pool.get_d2h_buffer(static_cast<uint8_t>(GPU_STREAM_COUNT), 0), nullptr);
    EXPECT_EQ(pool.get_d2h_buffer(0, static_cast<uint8_t>(GPU_INFLIGHT_MAX)), nullptr);

    pool.destroy();
}

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
