#pragma once

#include <cstdint>

namespace qdgz300::control
{
    struct OrchestratorMetrics
    {
        uint64_t transitions{0};
        uint64_t boot_count{0};
        uint64_t reset_count{0};
    };
}
