// include/qdgz300/common/event_types.h
// 事件枚举 + AlertEvent JSON 结构
#pragma once
#include <cstdint>
#include <string>
#include "qdgz300/common/system_state.h"

namespace qdgz300
{

    /// 告警事件结构（JSON 序列化）
    struct AlertEvent
    {
        uint64_t timestamp_ns;
        uint64_t run_id;
        uint64_t trace_id;
        EventDomain domain;
        Severity severity;
        std::string source_module;
        std::string event_code;
        std::string message;
        std::string detail_json;
    };

    /// 节流配置
    struct ThrottleConfig
    {
        uint32_t gpu_timeout_window_ms = 1000;
        uint32_t queue_overflow_window_ms = 1000;
        uint32_t time_sync_window_ms = 5000;
        uint32_t udp_loss_window_ms = 5000;
        // SEV-1 事件永不节流
    };

} // namespace qdgz300
