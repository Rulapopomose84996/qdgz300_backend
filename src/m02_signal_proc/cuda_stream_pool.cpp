#include "qdgz300/m02_signal_proc/cuda_stream_pool.h"
#include <spdlog/spdlog.h>

namespace qdgz300::m02
{

    void CudaStreamPool::init()
    {
#ifdef QDGZ300_HAS_GPU
        // TODO: cudaStreamCreate for each stream, cudaEventCreate for each event
#else
        for (uint8_t i = 0; i < GPU_STREAM_COUNT; ++i)
        {
            streams_[i] = nullptr;
            events_[i] = nullptr;
        }
#endif
        spdlog::info("[M02] CudaStreamPool initialized ({} streams, GPU={})",
                     GPU_STREAM_COUNT,
#ifdef QDGZ300_HAS_GPU
                     "yes"
#else
                     "no (CPU fallback)"
#endif
        );
    }

    void *CudaStreamPool::get_stream(uint8_t index) noexcept
    {
        if (index >= GPU_STREAM_COUNT)
            return nullptr;
        return streams_[index];
    }

    void *CudaStreamPool::get_event(uint8_t index) noexcept
    {
        if (index >= GPU_STREAM_COUNT)
            return nullptr;
        return events_[index];
    }

    void CudaStreamPool::destroy() noexcept
    {
#ifdef QDGZ300_HAS_GPU
        // TODO: cudaStreamDestroy, cudaEventDestroy
#else
        for (uint8_t i = 0; i < GPU_STREAM_COUNT; ++i)
        {
            streams_[i] = nullptr;
            events_[i] = nullptr;
        }
#endif
        spdlog::info("[M02] CudaStreamPool destroyed");
    }

} // namespace qdgz300::m02
