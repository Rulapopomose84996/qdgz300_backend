// tests/common_system_state_tests.cpp
// SystemState FSM + 19 条转移规则单元测试
#include <gtest/gtest.h>
#include "qdgz300/common/system_state.h"

using namespace qdgz300;

// ═══ 枚举值测试 ═══

TEST(SystemStateTest, EnumValues)
{
    EXPECT_EQ(static_cast<uint8_t>(SystemState::Init), 0);
    EXPECT_EQ(static_cast<uint8_t>(SystemState::Standby), 1);
    EXPECT_EQ(static_cast<uint8_t>(SystemState::Running), 2);
    EXPECT_EQ(static_cast<uint8_t>(SystemState::Degraded), 3);
    EXPECT_EQ(static_cast<uint8_t>(SystemState::Fault), 4);
}

TEST(SystemStateTest, StateNames)
{
    EXPECT_EQ(state_name(SystemState::Init), "Init");
    EXPECT_EQ(state_name(SystemState::Standby), "Standby");
    EXPECT_EQ(state_name(SystemState::Running), "Running");
    EXPECT_EQ(state_name(SystemState::Degraded), "Degraded");
    EXPECT_EQ(state_name(SystemState::Fault), "Fault");
}

// ═══ 转移规则表验证 ═══

TEST(SystemStateTest, TransitionTableSize)
{
    // 数组中有 17 条规则（#1-#17），加上 #18/#19 由 is_transition_allowed 隐式处理
    EXPECT_GE(TRANSITION_TABLE_SIZE, 17u);
}

TEST(SystemStateTest, TransitionInitToStandby)
{
    EXPECT_TRUE(is_transition_allowed(SystemState::Init, SystemState::Standby));
}

TEST(SystemStateTest, TransitionInitToFault)
{
    EXPECT_TRUE(is_transition_allowed(SystemState::Init, SystemState::Fault));
}

TEST(SystemStateTest, TransitionStandbyToRunning)
{
    EXPECT_TRUE(is_transition_allowed(SystemState::Standby, SystemState::Running));
}

TEST(SystemStateTest, TransitionRunningToDegraded)
{
    EXPECT_TRUE(is_transition_allowed(SystemState::Running, SystemState::Degraded));
}

TEST(SystemStateTest, TransitionRunningToFault)
{
    EXPECT_TRUE(is_transition_allowed(SystemState::Running, SystemState::Fault));
}

TEST(SystemStateTest, TransitionDegradedToRunning)
{
    EXPECT_TRUE(is_transition_allowed(SystemState::Degraded, SystemState::Running));
}

TEST(SystemStateTest, TransitionDegradedToFault)
{
    EXPECT_TRUE(is_transition_allowed(SystemState::Degraded, SystemState::Fault));
}

TEST(SystemStateTest, TransitionFaultToInit)
{
    EXPECT_TRUE(is_transition_allowed(SystemState::Fault, SystemState::Init));
}

TEST(SystemStateTest, TransitionFaultToStandby)
{
    EXPECT_TRUE(is_transition_allowed(SystemState::Fault, SystemState::Standby));
}

// ═══ 非法转移测试 ═══

TEST(SystemStateTest, IllegalTransitionStandbyToDegraded)
{
    // Standby → Degraded 不在表中（不通过 Fault 路径）
    EXPECT_FALSE(is_transition_allowed(SystemState::Standby, SystemState::Degraded));
}

TEST(SystemStateTest, IllegalTransitionInitToRunning)
{
    // Init → Running 不存在（必须先经过 Standby）
    EXPECT_FALSE(is_transition_allowed(SystemState::Init, SystemState::Running));
}

TEST(SystemStateTest, IllegalTransitionInitToDegraded)
{
    EXPECT_FALSE(is_transition_allowed(SystemState::Init, SystemState::Degraded));
}

// ═══ 规则 #18: 所有状态 → Fault 应总是允许（MemoryExhausted 路径）═══

TEST(SystemStateTest, AnyStateToFaultAlwaysAllowed)
{
    EXPECT_TRUE(is_transition_allowed(SystemState::Init, SystemState::Fault));
    EXPECT_TRUE(is_transition_allowed(SystemState::Standby, SystemState::Fault));
    EXPECT_TRUE(is_transition_allowed(SystemState::Running, SystemState::Fault));
    EXPECT_TRUE(is_transition_allowed(SystemState::Degraded, SystemState::Fault));
    EXPECT_TRUE(is_transition_allowed(SystemState::Fault, SystemState::Fault));
}

// ═══ 转移规则查表测试 ═══

TEST(SystemStateTest, TransitionTableContainsExpectedRules)
{
    // 验证转移表中包含已知规则
    bool found_init_to_standby = false;
    bool found_running_to_degraded_trackgap = false;
    bool found_degraded_to_running_recovery = false;

    for (size_t i = 0; i < TRANSITION_TABLE_SIZE; ++i)
    {
        const auto &t = TRANSITION_TABLE[i];
        if (t.from == SystemState::Init && t.to == SystemState::Standby)
        {
            found_init_to_standby = true;
            EXPECT_EQ(t.severity, Severity::SEV_3);
        }
        if (t.from == SystemState::Running && t.to == SystemState::Degraded &&
            t.event == EventDomain::TrackDataGap)
        {
            found_running_to_degraded_trackgap = true;
            EXPECT_EQ(t.severity, Severity::SEV_2);
        }
        if (t.from == SystemState::Degraded && t.to == SystemState::Running &&
            t.event == EventDomain::Recovery)
        {
            found_degraded_to_running_recovery = true;
            EXPECT_EQ(t.severity, Severity::SEV_3);
        }
    }

    EXPECT_TRUE(found_init_to_standby);
    EXPECT_TRUE(found_running_to_degraded_trackgap);
    EXPECT_TRUE(found_degraded_to_running_recovery);
}
