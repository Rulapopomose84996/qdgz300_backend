#pragma once
#include "qdgz300/common/metrics.h"

namespace qdgz300::m02
{

    struct SignalProcMetrics
    {
        AtomicCounter gpu_frames_submitted{"gpu.frames_submitted"};
        AtomicCounter gpu_frames_completed{"gpu.frames_completed"};
        AtomicCounter gpu_timeout_total{"gpu.timeout_total"};
        AtomicCounter gpu_timeout_persistent{"gpu.timeout_persistent"};
        AtomicGauge gpu_utilization_pct{"gpu.utilization_pct"};
        AtomicGauge gpu_inflight_count{"gpu.inflight_count"};
        AtomicGauge gpu_queue_depth{"gpu.queue_depth"};
        AtomicCounter h2d_bytes_total{"gpu.h2d_bytes_total"};
        AtomicCounter d2h_bytes_total{"gpu.d2h_bytes_total"};
        AtomicCounter pinned_alloc_fail_total{"gpu.pinned_alloc_fail_total"};
    };

} // namespace qdgz300::m02
