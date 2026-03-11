// tests/common_event_types_tests.cpp
// EventDomain / Severity / AlertEvent / ThrottleConfig 测试
#include <gtest/gtest.h>
#include "qdgz300/common/event_types.h"
#include "qdgz300/common/system_state.h"

using namespace qdgz300;

TEST(EventTypesTest, EventDomainValues)
{
    EXPECT_EQ(static_cast<uint8_t>(EventDomain::BootHealth), 0);
    EXPECT_EQ(static_cast<uint8_t>(EventDomain::Warmup), 1);
    EXPECT_EQ(static_cast<uint8_t>(EventDomain::TrackDataGap), 2);
    EXPECT_EQ(static_cast<uint8_t>(EventDomain::GpuTimeout), 3);
    EXPECT_EQ(static_cast<uint8_t>(EventDomain::QueueOverflow), 4);
    EXPECT_EQ(static_cast<uint8_t>(EventDomain::TimeSyncLoss), 5);
    EXPECT_EQ(static_cast<uint8_t>(EventDomain::SensorMissing), 6);
    EXPECT_EQ(static_cast<uint8_t>(EventDomain::MemoryExhausted), 7);
    EXPECT_EQ(static_cast<uint8_t>(EventDomain::ManualCommand), 8);
    EXPECT_EQ(static_cast<uint8_t>(EventDomain::Recovery), 9);
}

TEST(EventTypesTest, SeverityValues)
{
    EXPECT_EQ(static_cast<uint8_t>(Severity::SEV_1), 1);
    EXPECT_EQ(static_cast<uint8_t>(Severity::SEV_2), 2);
    EXPECT_EQ(static_cast<uint8_t>(Severity::SEV_3), 3);
}

TEST(EventTypesTest, AlertEventConstruction)
{
    AlertEvent evt;
    evt.timestamp_ns = 1000000000ULL;
    evt.run_id = 42;
    evt.trace_id = 100;
    evt.domain = EventDomain::GpuTimeout;
    evt.severity = Severity::SEV_2;
    evt.source_module = "m02_signal_proc";
    evt.event_code = "GPU_COMPUTE_TIMEOUT";
    evt.message = "GPU processing exceeded 50ms limit";
    evt.detail_json = R"({"elapsed_ms":55,"stream_id":1})";

    EXPECT_EQ(evt.domain, EventDomain::GpuTimeout);
    EXPECT_EQ(evt.severity, Severity::SEV_2);
    EXPECT_EQ(evt.source_module, "m02_signal_proc");
    EXPECT_FALSE(evt.detail_json.empty());
}

TEST(EventTypesTest, ThrottleConfigDefaults)
{
    ThrottleConfig tc;
    EXPECT_EQ(tc.gpu_timeout_window_ms, 1000u);
    EXPECT_EQ(tc.queue_overflow_window_ms, 1000u);
    EXPECT_EQ(tc.time_sync_window_ms, 5000u);
    EXPECT_EQ(tc.udp_loss_window_ms, 5000u);
}

TEST(EventTypesTest, ThrottleConfigCustom)
{
    ThrottleConfig tc;
    tc.gpu_timeout_window_ms = 2000;
    tc.queue_overflow_window_ms = 500;
    EXPECT_EQ(tc.gpu_timeout_window_ms, 2000u);
    EXPECT_EQ(tc.queue_overflow_window_ms, 500u);
}
