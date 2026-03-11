#pragma once

#include <cstdint>

namespace qdgz300::m04
{
    struct GatewayMetrics
    {
        uint64_t frames_sent{0};
        uint64_t frames_dropped{0};
        uint64_t serialize_errors{0};
        uint32_t alive_sessions{0};
    };
}
