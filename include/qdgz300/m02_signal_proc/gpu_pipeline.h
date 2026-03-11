#pragma once

#include "qdgz300/common/error_codes.h"
#include "qdgz300/common/types.h"
#include "qdgz300/m02_signal_proc/cuda_stream_pool.h"
#include "qdgz300/m02_signal_proc/pinned_buffer_pool.h"

#include <chrono>
#include <cstdint>

class GpuPipelineInflightTest;

namespace qdgz300::m02
{

    class GpuPipeline
    {
    public:
        GpuPipeline(CudaStreamPool &streams, PinnedBufferPool &bufs);
        ErrorCode submit_cpi(RawBlock *block, uint8_t stream_id) noexcept;
        bool poll_completion(uint8_t stream_id, PlotBatch **result) noexcept;
        void handle_timeout(uint8_t stream_id) noexcept;
        uint32_t inflight_count(uint8_t stream_id) const noexcept;
        bool has_timeout(uint8_t stream_id) const noexcept;

    private:
        struct InflightSlot
        {
            RawBlock *input{nullptr};
            PlotBatch *output{nullptr};
            bool occupied{false};
            bool completed{false};
            std::chrono::steady_clock::time_point submit_time{};
        };

        friend class ::GpuPipelineInflightTest;

        int8_t find_free_slot(uint8_t stream_id) const noexcept;

        CudaStreamPool &streams_;
        PinnedBufferPool &bufs_;
        InflightSlot inflight_[GPU_STREAM_COUNT][GPU_INFLIGHT_MAX];
        uint8_t next_slot_[GPU_STREAM_COUNT]{};
    };

} // namespace qdgz300::m02
