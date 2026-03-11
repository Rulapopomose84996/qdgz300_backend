// tests/common_config_snapshot_tests.cpp
// ConfigSnapshot 单元测试
#include <gtest/gtest.h>
#include "qdgz300/common/config_snapshot.h"
#include "qdgz300/common/constants.h"

using namespace qdgz300;

TEST(ConfigSnapshotTest, ChangeLevelEnum)
{
    EXPECT_EQ(static_cast<uint8_t>(ChangeLevel::FROZEN), 0);
    EXPECT_EQ(static_cast<uint8_t>(ChangeLevel::STATIC), 1);
    EXPECT_EQ(static_cast<uint8_t>(ChangeLevel::DYNAMIC_NEXT), 2);
    EXPECT_EQ(static_cast<uint8_t>(ChangeLevel::DYNAMIC_IMMEDIATE), 3);
}

TEST(ConfigSnapshotTest, DefaultInit)
{
    ConfigSnapshot snap{};
    EXPECT_EQ(snap.version, 0u);
    EXPECT_EQ(snap.apply_timestamp_ns, 0u);
}

TEST(ConfigSnapshotTest, FrozenValuesCopy)
{
    // 验证 ConfigSnapshot 能正确保存冻结值
    ConfigSnapshot snap{};
    snap.version = 1;
    snap.t_reasm_ms = T_REASM_MS;
    snap.t_gpu_max_ms = T_GPU_MAX_MS;
    snap.d_handover_m = D_HANDOVER_M;
    snap.v_handover_ms = V_HANDOVER_MS;
    snap.theta_handover_deg = THETA_HANDOVER_DEG;
    snap.hmi_ping_interval_ms = HMI_PING_INTERVAL_MS;
    snap.hmi_pong_timeout_ms = HMI_PONG_TIMEOUT_MS;
    snap.rto_ms = RTO_MS;
    snap.max_retry = MAX_RETRY;

    EXPECT_EQ(snap.t_reasm_ms, 100u);
    EXPECT_EQ(snap.t_gpu_max_ms, 50u);
    EXPECT_DOUBLE_EQ(snap.d_handover_m, 100.0);
    EXPECT_DOUBLE_EQ(snap.v_handover_ms, 10.0);
    EXPECT_DOUBLE_EQ(snap.theta_handover_deg, 15.0);
    EXPECT_EQ(snap.hmi_ping_interval_ms, 1000u);
    EXPECT_EQ(snap.rto_ms, 2500u);
    EXPECT_EQ(snap.max_retry, 3u);
}

TEST(ConfigSnapshotTest, Copyable)
{
    ConfigSnapshot a{};
    a.version = 42;
    a.t_reasm_ms = 100;
    a.recording_enabled = true;

    ConfigSnapshot b = a;
    EXPECT_EQ(b.version, 42u);
    EXPECT_EQ(b.t_reasm_ms, 100u);
    EXPECT_EQ(b.recording_enabled, true);
}
