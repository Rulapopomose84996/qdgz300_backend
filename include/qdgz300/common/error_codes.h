// include/qdgz300/common/error_codes.h
// 统一错误码枚举
#pragma once
#include <cstdint>
#include <string_view>

namespace qdgz300
{

    enum class ErrorCode : uint32_t
    {
        OK = 0,

        // ═══ M01 Receiver (1xxx) ═══
        RECV_SOCKET_INIT_FAILED = 1001,
        RECV_BIND_FAILED = 1002,
        RECV_AFFINITY_MISMATCH = 1003,
        RECV_MAGIC_MISMATCH = 1010,
        RECV_VERSION_MISMATCH = 1011,
        RECV_PACKET_TYPE_UNKNOWN = 1012,
        RECV_PAYLOAD_LEN_MISMATCH = 1013,
        RECV_SOURCE_ID_INVALID = 1014,
        RECV_CRC_FAILED = 1020,
        RECV_REASM_TIMEOUT = 1030,
        RECV_REASM_OVERSIZE = 1031,
        RECV_REASM_TOTALFRAGS_EXCEED = 1032,
        RECV_LATE_FRAGMENT = 1033,
        RECV_DUPLICATE_FRAGMENT = 1034,
        RECV_QUEUE_FULL = 1040,
        RECV_MEMORY_POOL_EXHAUSTED = 1050,

        // ═══ M02 SignalProc (2xxx) ═══
        GPU_STREAM_INIT_FAILED = 2001,
        GPU_H2D_FAILED = 2010,
        GPU_KERNEL_LAUNCH_FAILED = 2011,
        GPU_D2H_FAILED = 2012,
        GPU_COMPUTE_TIMEOUT = 2020,
        GPU_COMPUTE_TIMEOUT_PERSISTENT = 2021,
        GPU_PINNED_ALLOC_FAILED = 2030,

        // ═══ M03 DataProc (3xxx) ═══
        TRACK_ASSOCIATION_FAILED = 3001,
        TRACK_ID_OVERFLOW = 3010,
        FUSION_HANDOVER_FAILED = 3020,

        // ═══ M04 Gateway (4xxx) ═══
        GW_TCP_BIND_FAILED = 4001,
        GW_TCP_ACCEPT_FAILED = 4002,
        GW_HANDSHAKE_FAILED = 4010,
        GW_SERIALIZE_FAILED = 4020,
        GW_TRUNCATED = 4021,
        GW_UDP_SEND_FAILED = 4030,
        GW_OUTPUT_QUEUE_FULL = 4040,

        // ═══ Orchestrator / Control (5xxx) ═══
        ORC_INVALID_TRANSITION = 5001,
        ORC_TRANSITION_TOO_FAST = 5002,
        ORC_COMMAND_BRIDGE_TIMEOUT = 5010,
        ORC_COMMAND_BRIDGE_MAX_RETRY = 5011,
        ORC_EVENT_THROTTLED = 5020,

        // ═══ Config (6xxx) ═══
        CFG_VALIDATION_FAILED = 6001,
        CFG_FROZEN_PARAM_MODIFIED = 6002,
        CFG_SNAPSHOT_VERSION_CONFLICT = 6010,

        // ═══ TimeSync (7xxx) ═══
        TIME_SYNC_LOST = 7001,
        TIME_HOLDOVER_EXPIRED = 7002,

        // ═══ Health (8xxx) ═══
        HEALTH_BOOT_CHECK_FAILED = 8001,
        HEALTH_WARMUP_TIMEOUT = 8002,
        HEALTH_RUNTIME_ANOMALY = 8010,

        // ═══ System (9xxx) ═══
        MEMORY_POOL_EXHAUSTED = 9001,
        QUEUE_OVERFLOW_PERSISTENT = 9002,
        INPUT_SENSOR_MISSING = 9003,
        REASM_TIMEOUT_VIOLATION = 9004,
    };

    constexpr std::string_view error_name(ErrorCode code) noexcept
    {
        switch (code)
        {
        case ErrorCode::OK:
            return "OK";
        // M01 Receiver (1xxx)
        case ErrorCode::RECV_SOCKET_INIT_FAILED:
            return "RECV_SOCKET_INIT_FAILED";
        case ErrorCode::RECV_BIND_FAILED:
            return "RECV_BIND_FAILED";
        case ErrorCode::RECV_AFFINITY_MISMATCH:
            return "RECV_AFFINITY_MISMATCH";
        case ErrorCode::RECV_MAGIC_MISMATCH:
            return "RECV_MAGIC_MISMATCH";
        case ErrorCode::RECV_VERSION_MISMATCH:
            return "RECV_VERSION_MISMATCH";
        case ErrorCode::RECV_PACKET_TYPE_UNKNOWN:
            return "RECV_PACKET_TYPE_UNKNOWN";
        case ErrorCode::RECV_PAYLOAD_LEN_MISMATCH:
            return "RECV_PAYLOAD_LEN_MISMATCH";
        case ErrorCode::RECV_SOURCE_ID_INVALID:
            return "RECV_SOURCE_ID_INVALID";
        case ErrorCode::RECV_CRC_FAILED:
            return "RECV_CRC_FAILED";
        case ErrorCode::RECV_REASM_TIMEOUT:
            return "RECV_REASM_TIMEOUT";
        case ErrorCode::RECV_REASM_OVERSIZE:
            return "RECV_REASM_OVERSIZE";
        case ErrorCode::RECV_REASM_TOTALFRAGS_EXCEED:
            return "RECV_REASM_TOTALFRAGS_EXCEED";
        case ErrorCode::RECV_LATE_FRAGMENT:
            return "RECV_LATE_FRAGMENT";
        case ErrorCode::RECV_DUPLICATE_FRAGMENT:
            return "RECV_DUPLICATE_FRAGMENT";
        case ErrorCode::RECV_QUEUE_FULL:
            return "RECV_QUEUE_FULL";
        case ErrorCode::RECV_MEMORY_POOL_EXHAUSTED:
            return "RECV_MEMORY_POOL_EXHAUSTED";
        // M02 SignalProc (2xxx)
        case ErrorCode::GPU_STREAM_INIT_FAILED:
            return "GPU_STREAM_INIT_FAILED";
        case ErrorCode::GPU_H2D_FAILED:
            return "GPU_H2D_FAILED";
        case ErrorCode::GPU_KERNEL_LAUNCH_FAILED:
            return "GPU_KERNEL_LAUNCH_FAILED";
        case ErrorCode::GPU_D2H_FAILED:
            return "GPU_D2H_FAILED";
        case ErrorCode::GPU_COMPUTE_TIMEOUT:
            return "GPU_COMPUTE_TIMEOUT";
        case ErrorCode::GPU_COMPUTE_TIMEOUT_PERSISTENT:
            return "GPU_COMPUTE_TIMEOUT_PERSISTENT";
        case ErrorCode::GPU_PINNED_ALLOC_FAILED:
            return "GPU_PINNED_ALLOC_FAILED";
        // M03 DataProc (3xxx)
        case ErrorCode::TRACK_ASSOCIATION_FAILED:
            return "TRACK_ASSOCIATION_FAILED";
        case ErrorCode::TRACK_ID_OVERFLOW:
            return "TRACK_ID_OVERFLOW";
        case ErrorCode::FUSION_HANDOVER_FAILED:
            return "FUSION_HANDOVER_FAILED";
        // M04 Gateway (4xxx)
        case ErrorCode::GW_TCP_BIND_FAILED:
            return "GW_TCP_BIND_FAILED";
        case ErrorCode::GW_TCP_ACCEPT_FAILED:
            return "GW_TCP_ACCEPT_FAILED";
        case ErrorCode::GW_HANDSHAKE_FAILED:
            return "GW_HANDSHAKE_FAILED";
        case ErrorCode::GW_SERIALIZE_FAILED:
            return "GW_SERIALIZE_FAILED";
        case ErrorCode::GW_TRUNCATED:
            return "GW_TRUNCATED";
        case ErrorCode::GW_UDP_SEND_FAILED:
            return "GW_UDP_SEND_FAILED";
        case ErrorCode::GW_OUTPUT_QUEUE_FULL:
            return "GW_OUTPUT_QUEUE_FULL";
        // Orchestrator (5xxx)
        case ErrorCode::ORC_INVALID_TRANSITION:
            return "ORC_INVALID_TRANSITION";
        case ErrorCode::ORC_TRANSITION_TOO_FAST:
            return "ORC_TRANSITION_TOO_FAST";
        case ErrorCode::ORC_COMMAND_BRIDGE_TIMEOUT:
            return "ORC_COMMAND_BRIDGE_TIMEOUT";
        case ErrorCode::ORC_COMMAND_BRIDGE_MAX_RETRY:
            return "ORC_COMMAND_BRIDGE_MAX_RETRY";
        case ErrorCode::ORC_EVENT_THROTTLED:
            return "ORC_EVENT_THROTTLED";
        // Config (6xxx)
        case ErrorCode::CFG_VALIDATION_FAILED:
            return "CFG_VALIDATION_FAILED";
        case ErrorCode::CFG_FROZEN_PARAM_MODIFIED:
            return "CFG_FROZEN_PARAM_MODIFIED";
        case ErrorCode::CFG_SNAPSHOT_VERSION_CONFLICT:
            return "CFG_SNAPSHOT_VERSION_CONFLICT";
        // TimeSync (7xxx)
        case ErrorCode::TIME_SYNC_LOST:
            return "TIME_SYNC_LOST";
        case ErrorCode::TIME_HOLDOVER_EXPIRED:
            return "TIME_HOLDOVER_EXPIRED";
        // Health (8xxx)
        case ErrorCode::HEALTH_BOOT_CHECK_FAILED:
            return "HEALTH_BOOT_CHECK_FAILED";
        case ErrorCode::HEALTH_WARMUP_TIMEOUT:
            return "HEALTH_WARMUP_TIMEOUT";
        case ErrorCode::HEALTH_RUNTIME_ANOMALY:
            return "HEALTH_RUNTIME_ANOMALY";
        // System (9xxx)
        case ErrorCode::MEMORY_POOL_EXHAUSTED:
            return "MEMORY_POOL_EXHAUSTED";
        case ErrorCode::QUEUE_OVERFLOW_PERSISTENT:
            return "QUEUE_OVERFLOW_PERSISTENT";
        case ErrorCode::INPUT_SENSOR_MISSING:
            return "INPUT_SENSOR_MISSING";
        case ErrorCode::REASM_TIMEOUT_VIOLATION:
            return "REASM_TIMEOUT_VIOLATION";
        }
        return "UNKNOWN";
    }

} // namespace qdgz300
