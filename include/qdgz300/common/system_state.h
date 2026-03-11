// include/qdgz300/common/system_state.h
// 5-State FSM 枚举 + 19 条转移规则
#pragma once
#include <cstdint>
#include <cstddef>
#include <string_view>

namespace qdgz300
{

    /// 系统运行状态（冻结，V1 不可删改）
    enum class SystemState : uint8_t
    {
        Init = 0,
        Standby = 1,
        Running = 2,
        Degraded = 3,
        Fault = 4,
    };

    constexpr std::string_view state_name(SystemState s) noexcept
    {
        switch (s)
        {
        case SystemState::Init:
            return "Init";
        case SystemState::Standby:
            return "Standby";
        case SystemState::Running:
            return "Running";
        case SystemState::Degraded:
            return "Degraded";
        case SystemState::Fault:
            return "Fault";
        }
        return "Unknown";
    }

    /// 事件域枚举
    enum class EventDomain : uint8_t
    {
        BootHealth = 0,
        Warmup = 1,
        TrackDataGap = 2,
        GpuTimeout = 3,
        QueueOverflow = 4,
        TimeSyncLoss = 5,
        SensorMissing = 6,
        MemoryExhausted = 7,
        ManualCommand = 8,
        Recovery = 9,
    };

    /// 严重级别
    enum class Severity : uint8_t
    {
        SEV_3 = 3, // 信息级（记录）
        SEV_2 = 2, // 警告级（Degraded）
        SEV_1 = 1, // 严重级（Fault）
    };

    /// 状态转移规则
    struct StateTransition
    {
        SystemState from;
        EventDomain event;
        Severity severity;
        SystemState to;
        const char *description;
    };

    /// 19 条决策矩阵转移规则（冻结）
    extern const StateTransition TRANSITION_TABLE[];
    extern const size_t TRANSITION_TABLE_SIZE;

    /// 检查指定转移是否合法
    bool is_transition_allowed(SystemState from, SystemState to) noexcept;

} // namespace qdgz300
