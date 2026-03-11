// src/common/system_state.cpp
// 5-State FSM 转移规则表实现
#include "qdgz300/common/system_state.h"

namespace qdgz300
{

    // 19 条决策矩阵转移规则（冻结）
    const StateTransition TRANSITION_TABLE[] = {
        // #1: Init → Standby (Boot Health All Pass)
        {SystemState::Init, EventDomain::BootHealth, Severity::SEV_3, SystemState::Standby, "6 项检查全过"},
        // #2: Init → Fault (Boot Health Fail)
        {SystemState::Init, EventDomain::BootHealth, Severity::SEV_1, SystemState::Fault, "任一检查失败"},
        // #3: Standby → Running (Warmup All Met)
        {SystemState::Standby, EventDomain::Warmup, Severity::SEV_3, SystemState::Running, "3 条件满足"},
        // #4: Running → Degraded (TrackDataGap >= T1)
        {SystemState::Running, EventDomain::TrackDataGap, Severity::SEV_2, SystemState::Degraded, "500ms 无 TrackData"},
        // #5: Running → Degraded (GPU Timeout persist)
        {SystemState::Running, EventDomain::GpuTimeout, Severity::SEV_2, SystemState::Degraded, "GPU 连续超时"},
        // #6: Running → Degraded (Queue Overflow >= 10s)
        {SystemState::Running, EventDomain::QueueOverflow, Severity::SEV_2, SystemState::Degraded, "队列持续溢出"},
        // #7: Running → Degraded (TimeSync Loss)
        {SystemState::Running, EventDomain::TimeSyncLoss, Severity::SEV_2, SystemState::Degraded, "时间质量降级"},
        // #8: Running → Degraded (Sensor Missing 1of3)
        {SystemState::Running, EventDomain::SensorMissing, Severity::SEV_2, SystemState::Degraded, "单阵面断流"},
        // #9: Running → Fault (Memory Exhausted)
        {SystemState::Running, EventDomain::MemoryExhausted, Severity::SEV_1, SystemState::Fault, "内存池耗尽"},
        // #10: Running → Fault (Manual Stop)
        {SystemState::Running, EventDomain::ManualCommand, Severity::SEV_3, SystemState::Fault, "人工强制停止"},
        // #11: Degraded → Running (Recovery)
        {SystemState::Degraded, EventDomain::Recovery, Severity::SEV_3, SystemState::Running, "所有降级条件解除"},
        // #12: Degraded → Fault (TrackDataGap >= T2)
        {SystemState::Degraded, EventDomain::TrackDataGap, Severity::SEV_1, SystemState::Fault, "2s 无 TrackData"},
        // #13: Degraded → Fault (Sensor Missing 3of3)
        {SystemState::Degraded, EventDomain::SensorMissing, Severity::SEV_1, SystemState::Fault, "全阵面断流"},
        // #14: Degraded → Fault (Memory Exhausted)
        {SystemState::Degraded, EventDomain::MemoryExhausted, Severity::SEV_1, SystemState::Fault, "内存池耗尽"},
        // #15: Degraded → Fault (Manual Stop)
        {SystemState::Degraded, EventDomain::ManualCommand, Severity::SEV_3, SystemState::Fault, "人工强制停止"},
        // #16: Fault → Init (Manual Reset)
        {SystemState::Fault, EventDomain::ManualCommand, Severity::SEV_3, SystemState::Init, "人工重启"},
        // #17: Fault → Standby (Manual Standby)
        {SystemState::Fault, EventDomain::ManualCommand, Severity::SEV_3, SystemState::Standby, "人工恢复到就绪"},
    };

    const size_t TRANSITION_TABLE_SIZE = sizeof(TRANSITION_TABLE) / sizeof(TRANSITION_TABLE[0]);

    bool is_transition_allowed(SystemState from, SystemState to) noexcept
    {
        // 规则 #18: * → Fault on MemoryExhausted（always allowed）
        if (to == SystemState::Fault)
            return true;

        for (size_t i = 0; i < TRANSITION_TABLE_SIZE; ++i)
        {
            if (TRANSITION_TABLE[i].from == from && TRANSITION_TABLE[i].to == to)
            {
                return true;
            }
        }
        return false;
    }

} // namespace qdgz300
