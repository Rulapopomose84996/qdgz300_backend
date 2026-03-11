#pragma once

#include <cstdint>

namespace qdgz300::m03
{
    struct DataProcMetrics
    {
        uint64_t plot_batches_processed{0};
        uint64_t associations_made{0};
        uint64_t tracks_created{0};
        uint64_t tracks_deleted{0};
        uint64_t fusion_events{0};
    };
}
