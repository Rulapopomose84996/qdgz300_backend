// include/qdgz300/common/config_snapshot.h
// 配置快照结构（版本化 + 原子交换）
#pragma once
#include <cstdint>

namespace qdgz300
{

    /// 三层变更分类
    enum class ChangeLevel : uint8_t
    {
        FROZEN = 0,
        STATIC = 1,
        DYNAMIC_NEXT = 2,
        DYNAMIC_IMMEDIATE = 3,
    };

    /// 配置快照 — 版本化，原子交换
    struct ConfigSnapshot
    {
        uint64_t version;
        uint64_t apply_timestamp_ns;

        // ═══ M01 Receiver ═══
        uint32_t t_reasm_ms;
        uint32_t rawcpi_q_capacity;
        uint32_t rec_q_capacity;
        bool recording_enabled;
        uint8_t recording_array_filter;

        // ═══ M02 SignalProc ═══
        uint32_t t_gpu_max_ms;
        uint32_t t_gpu_warn_ms;

        // ═══ M03 DataProc ═══
        double d_handover_m;
        double v_handover_ms;
        double theta_handover_deg;

        // ═══ M04 Gateway ═══
        uint32_t hmi_ping_interval_ms;
        uint32_t hmi_pong_timeout_ms;

        // ═══ 控制面 ═══
        uint32_t rto_ms;
        uint32_t max_retry;

        // ═══ 日志 ═══
        uint8_t log_level;
    };

} // namespace qdgz300
