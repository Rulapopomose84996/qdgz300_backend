#pragma once
#include <cstddef>
#include <cstdint>

namespace qdgz300::m02
{

    class PinnedBufferPool
    {
    public:
        void init(size_t cpi_size, uint8_t stream_count, uint8_t pingpong_depth);
        uint8_t *get_h2d_buffer(uint8_t stream_id, uint8_t slot) noexcept;
        uint8_t *get_d2h_buffer(uint8_t stream_id, uint8_t slot) noexcept;
        void destroy() noexcept;

    private:
        uint8_t *h2d_buffers_{nullptr};
        uint8_t *d2h_buffers_{nullptr};
        size_t cpi_size_{0};
        uint8_t stream_count_{0};
        uint8_t pingpong_depth_{0};
    };

} // namespace qdgz300::m02
