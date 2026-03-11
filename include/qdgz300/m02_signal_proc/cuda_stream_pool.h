#pragma once
#include <cstdint>
#include "qdgz300/common/constants.h"

namespace qdgz300::m02
{

    class CudaStreamPool
    {
    public:
        void init();
        void *get_stream(uint8_t index) noexcept;
        void *get_event(uint8_t index) noexcept;
        void destroy() noexcept;

    private:
        void *streams_[GPU_STREAM_COUNT]{};
        void *events_[GPU_STREAM_COUNT]{};
    };

} // namespace qdgz300::m02
