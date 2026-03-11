// tests/common_types_tests.cpp
// types.h 数据结构单元测试 — #pragma pack + static_assert 验证
#include <gtest/gtest.h>
#include "qdgz300/common/types.h"
#include "qdgz300/common/constants.h"

#include <cstring>

using namespace qdgz300;

// ═══ 协议结构体大小验证 ═══

TEST(TypesTest, CommonHeaderSize)
{
    EXPECT_EQ(sizeof(CommonHeader), 32u);
    EXPECT_EQ(sizeof(CommonHeader), COMMON_HEADER_SIZE);
}

TEST(TypesTest, DataSpecificHeaderSize)
{
    EXPECT_EQ(sizeof(DataSpecificHeader), 40u);
    EXPECT_EQ(sizeof(DataSpecificHeader), DATA_SPECIFIC_HDR_SIZE);
}

TEST(TypesTest, ExecutionSnapshotSize)
{
    EXPECT_EQ(sizeof(ExecutionSnapshot), 40u);
    EXPECT_EQ(sizeof(ExecutionSnapshot), EXECUTION_SNAPSHOT_SIZE);
}

TEST(TypesTest, HeartbeatPayloadSize)
{
    EXPECT_EQ(sizeof(HeartbeatPayload), 48u);
    EXPECT_EQ(sizeof(HeartbeatPayload), HEARTBEAT_PAYLOAD_SIZE);
}

// ═══ 协议结构体字段偏移验证 ═══

TEST(TypesTest, CommonHeaderFieldOffsets)
{
    CommonHeader h{};
    auto base = reinterpret_cast<uintptr_t>(&h);

    EXPECT_EQ(reinterpret_cast<uintptr_t>(&h.magic) - base, 0u);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(&h.protocol_version) - base, 4u);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(&h.packet_type) - base, 6u);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(&h.source_id) - base, 7u);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(&h.dest_id) - base, 8u);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(&h.payload_len) - base, 10u);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(&h.sequence_number) - base, 12u);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(&h.timestamp) - base, 16u);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(&h.control_epoch) - base, 24u);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(&h.link_epoch) - base, 28u);
}

// ═══ 协议结构体二进制序列化测试 ═══

TEST(TypesTest, CommonHeaderBinarySerialization)
{
    CommonHeader h{};
    h.magic = PROTOCOL_MAGIC;
    h.protocol_version = PROTOCOL_VERSION;
    h.packet_type = PKT_TYPE_DATA;
    h.source_id = DACS_ARRAY_1;
    h.dest_id = SPS_ID;
    h.payload_len = 1000;
    h.sequence_number = 42;
    h.timestamp = 1234567890000ULL;
    h.control_epoch = 1;
    h.link_epoch = 2;

    // 序列化为字节数组
    uint8_t buf[32];
    std::memcpy(buf, &h, sizeof(h));

    // 反序列化
    CommonHeader h2{};
    std::memcpy(&h2, buf, sizeof(h2));

    EXPECT_EQ(h2.magic, PROTOCOL_MAGIC);
    EXPECT_EQ(h2.protocol_version, PROTOCOL_VERSION);
    EXPECT_EQ(h2.packet_type, PKT_TYPE_DATA);
    EXPECT_EQ(h2.source_id, DACS_ARRAY_1);
    EXPECT_EQ(h2.payload_len, 1000u);
    EXPECT_EQ(h2.sequence_number, 42u);
    EXPECT_EQ(h2.timestamp, 1234567890000ULL);
}

// ═══ 内部结构体测试 ═══

TEST(TypesTest, RawBlockFlags)
{
    RawBlock rb{};
    rb.flags = 0;

    rb.flags |= RawBlock::FLAG_INCOMPLETE_FRAME;
    EXPECT_TRUE(rb.flags & RawBlock::FLAG_INCOMPLETE_FRAME);
    EXPECT_FALSE(rb.flags & RawBlock::FLAG_SNAPSHOT_PRESENT);

    rb.flags |= RawBlock::FLAG_SNAPSHOT_PRESENT;
    EXPECT_TRUE(rb.flags & RawBlock::FLAG_SNAPSHOT_PRESENT);

    rb.flags |= RawBlock::FLAG_GPU_TIMEOUT;
    EXPECT_TRUE(rb.flags & RawBlock::FLAG_GPU_TIMEOUT);
    EXPECT_FALSE(rb.flags & RawBlock::FLAG_HEARTBEAT_RELATED);
}

TEST(TypesTest, TrackLifecycleEnum)
{
    EXPECT_EQ(static_cast<uint8_t>(TrackLifecycle::TENTATIVE), 0);
    EXPECT_EQ(static_cast<uint8_t>(TrackLifecycle::CONFIRMED), 1);
    EXPECT_EQ(static_cast<uint8_t>(TrackLifecycle::COASTING), 2);
    EXPECT_EQ(static_cast<uint8_t>(TrackLifecycle::LOST), 3);
    EXPECT_EQ(static_cast<uint8_t>(TrackLifecycle::DELETED), 4);
}

// ═══ 常量冻结值验证 ═══

TEST(ConstantsTest, ProtocolFrozenValues)
{
    EXPECT_EQ(PROTOCOL_MAGIC, 0x55AA55AAu);
    EXPECT_EQ(PROTOCOL_VERSION, 0x31u);
    EXPECT_EQ(T_REASM_MS, 100u);
    EXPECT_EQ(MAX_TOTALFRAGS, 1024u);
    EXPECT_EQ(MAX_REASM_BYTES_PER_KEY, 16u * 1024u * 1024u);
    EXPECT_EQ(MAX_UDP_PAYLOAD, 1200u);
    EXPECT_EQ(RTO_MS, 2500u);
    EXPECT_EQ(MAX_RETRY, 3u);
    EXPECT_EQ(T_GPU_MAX_MS, 50u);
    EXPECT_EQ(T1_DEGRADED_MS, 500u);
    EXPECT_EQ(T2_FAULT_MS, 2000u);
    EXPECT_EQ(T_TRANSITION_MIN_MS, 10000u);
}

TEST(ConstantsTest, HandoverThresholds)
{
    EXPECT_DOUBLE_EQ(D_HANDOVER_M, 100.0);
    EXPECT_DOUBLE_EQ(V_HANDOVER_MS, 10.0);
    EXPECT_DOUBLE_EQ(THETA_HANDOVER_DEG, 15.0);
}

TEST(ConstantsTest, HMIParameters)
{
    EXPECT_EQ(HMI_PING_INTERVAL_MS, 1000u);
    EXPECT_EQ(HMI_PONG_TIMEOUT_MS, 3000u);
}
