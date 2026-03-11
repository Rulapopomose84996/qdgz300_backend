#include "qdgz300/m02_signal_proc/pinned_buffer_pool.h"
#include <spdlog/spdlog.h>
#include <cstdlib>

namespace qdgz300::m02
{

    namespace
    {
        constexpr size_t kBufferAlignment = 64;

        size_t align_up(size_t value, size_t alignment) noexcept
        {
            return ((value + alignment - 1) / alignment) * alignment;
        }
    } // namespace

    void PinnedBufferPool::init(size_t cpi_size, uint8_t stream_count, uint8_t pingpong_depth)
    {
        destroy();

        cpi_size_ = cpi_size;
        stream_count_ = stream_count;
        pingpong_depth_ = pingpong_depth;

        size_t total = cpi_size * stream_count * pingpong_depth;
#ifdef QDGZ300_HAS_GPU
        // TODO: cudaMallocHost for pinned memory
#else
        const size_t alloc_size = align_up(total, kBufferAlignment);
        h2d_buffers_ = static_cast<uint8_t *>(aligned_alloc(kBufferAlignment, alloc_size));
        d2h_buffers_ = static_cast<uint8_t *>(aligned_alloc(kBufferAlignment, alloc_size));
#endif
        spdlog::info("[M02] PinnedBufferPool initialized: {} streams x {} depth x {} bytes",
                     stream_count, pingpong_depth, cpi_size);
    }

    uint8_t *PinnedBufferPool::get_h2d_buffer(uint8_t stream_id, uint8_t slot) noexcept
    {
        if (!h2d_buffers_ || stream_id >= stream_count_ || slot >= pingpong_depth_)
            return nullptr;
        return h2d_buffers_ + (stream_id * pingpong_depth_ + slot) * cpi_size_;
    }

    uint8_t *PinnedBufferPool::get_d2h_buffer(uint8_t stream_id, uint8_t slot) noexcept
    {
        if (!d2h_buffers_ || stream_id >= stream_count_ || slot >= pingpong_depth_)
            return nullptr;
        return d2h_buffers_ + (stream_id * pingpong_depth_ + slot) * cpi_size_;
    }

    void PinnedBufferPool::destroy() noexcept
    {
#ifdef QDGZ300_HAS_GPU
        // TODO: cudaFreeHost
#else
        std::free(h2d_buffers_);
        std::free(d2h_buffers_);
#endif
        h2d_buffers_ = nullptr;
        d2h_buffers_ = nullptr;
        cpi_size_ = 0;
        stream_count_ = 0;
        pingpong_depth_ = 0;
        spdlog::info("[M02] PinnedBufferPool destroyed");
    }

} // namespace qdgz300::m02
