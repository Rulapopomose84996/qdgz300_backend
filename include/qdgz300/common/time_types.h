// include/qdgz300/common/time_types.h
// 时间质量 Q0/Q1/Q2 + clock_domain
#pragma once
#include <cstdint>

namespace qdgz300
{

    /// 时间同步质量等级
    enum class TimeQuality : uint8_t
    {
        Q0_LOCKED = 0,   // 外部授时锁定
        Q1_HOLDOVER = 1, // 守时模式
        Q2_FREERUN = 2,  // 自由运行
    };

    /// 时间同步 FSM 状态
    enum class TimeSyncState : uint8_t
    {
        UNSYNC = 0,
        SYNCED = 1,
        HOLDOVER = 2,
        HOLDOVER_EXPIRED = 3,
    };

    /// 时钟域标识
    enum class ClockDomain : uint8_t
    {
        AUTHORITATIVE = 0,
        HOLDOVER = 1,
        APPROXIMATE = 2,
    };

} // namespace qdgz300
