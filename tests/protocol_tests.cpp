#include "qdgz300/m01_receiver/protocol/crc32c.h"
#include "qdgz300/m01_receiver/protocol/packet_parser.h"
#include "qdgz300/m01_receiver/protocol/payload_codec.h"
#include "qdgz300/m01_receiver/protocol/protocol_types.h"
#include "qdgz300/m01_receiver/protocol/validator.h"

#include <cstring>
#include <vector>

#include <gtest/gtest.h>

using receiver::protocol::CommonHeader;
using receiver::protocol::ControlBeamItem;
using receiver::protocol::ControlPayload;
using receiver::protocol::HeartbeatPayload;
using receiver::protocol::PacketParser;
using receiver::protocol::PacketType;
using receiver::protocol::PayloadCodec;
using receiver::protocol::PROTOCOL_MAGIC;
using receiver::protocol::PROTOCOL_VERSION;
using receiver::protocol::RmaPayload;
using receiver::protocol::ScheduleApplyAckPayload;
using receiver::protocol::ValidationResult;
using receiver::protocol::Validator;

namespace
{
    std::vector<uint8_t> wrap_packet(uint8_t type, const std::vector<uint8_t> &payload, uint8_t dest_id = 0x01)
    {
        std::vector<uint8_t> packet(32 + payload.size(), 0);
        CommonHeader header{};
        header.magic = PROTOCOL_MAGIC;
        header.sequence_number = 100;
        header.timestamp = 1700000000000ULL;
        header.payload_len = static_cast<uint16_t>(payload.size());
        header.packet_type = type;
        header.protocol_version = PROTOCOL_VERSION;
        header.source_id = 0x11;
        header.dest_id = dest_id;
        header.control_epoch = 1;
        std::memcpy(packet.data(), &header, sizeof(header));
        if (!payload.empty())
        {
            std::memcpy(packet.data() + 32, payload.data(), payload.size());
        }
        return packet;
    }
}

TEST(ProtocolTests, Crc32cKnownVector)
{
    const char *text = "123456789";
    const uint32_t crc = receiver::protocol::crc32c(reinterpret_cast<const uint8_t *>(text), 9);
    EXPECT_EQ(crc, 0xE3069283u);
}

TEST(ProtocolTests, ControlCodecRoundTripAndCorruption)
{
    ControlPayload control{};
    control.table_header.frame_counter = 7;
    control.table_header.frame_beam_total = 2;
    control.table_header.beams_per_second = 2;
    control.table_header.cpi_count_base = 1000;

    ControlBeamItem beam0{};
    beam0.beam_id = 1;
    beam0.work_freq_index = 10;
    beam0.data_rate_mbps = 100;
    ControlBeamItem beam1{};
    beam1.beam_id = 2;
    beam1.work_freq_index = 12;
    beam1.data_rate_mbps = 120;
    control.beam_items.push_back(beam0);
    control.beam_items.push_back(beam1);

    std::vector<uint8_t> encoded;
    ASSERT_TRUE(PayloadCodec::encode_control_payload(control, encoded));
    EXPECT_TRUE(PayloadCodec::verify_control_crc(encoded.data(), encoded.size()));

    ControlPayload decoded{};
    ASSERT_TRUE(PayloadCodec::decode_control_payload(encoded.data(), encoded.size(), decoded));
    EXPECT_EQ(decoded.table_header.beams_per_second, 2);
    ASSERT_EQ(decoded.beam_items.size(), 2u);
    EXPECT_EQ(decoded.beam_items[1].beam_id, 2);

    encoded.back() ^= 0xFF;
    EXPECT_FALSE(PayloadCodec::verify_control_crc(encoded.data(), encoded.size()));
}

TEST(ProtocolTests, AckAndHeartbeatCodec)
{
    ScheduleApplyAckPayload ack{};
    ack.ack_frame_counter = 88;
    ack.acked_sequence_number = 1234;
    ack.ack_result = 0;
    ack.error_code = 0;
    ack.error_detail = 0;
    ack.applied_timestamp = 1700001234000ULL;

    std::vector<uint8_t> ack_encoded;
    ASSERT_TRUE(PayloadCodec::encode_schedule_apply_ack_payload(ack, ack_encoded));
    EXPECT_TRUE(PayloadCodec::verify_schedule_apply_ack_crc(ack_encoded.data(), ack_encoded.size()));

    HeartbeatPayload heartbeat{};
    heartbeat.system_status_alive = 0x3;
    heartbeat.system_status_state = 0x00012345;
    heartbeat.core_temp = 725;
    heartbeat.op_mode = 2;
    heartbeat.time_coord_source = 1;
    heartbeat.device_number[0] = 26;
    heartbeat.device_number[1] = 8;
    heartbeat.time_offset = 12;
    heartbeat.error_counters[0] = 1;
    heartbeat.error_counters[1] = 2;
    heartbeat.error_counters[2] = 3;
    heartbeat.error_counters[3] = 4;
    heartbeat.lo_lock_flags = 0x7;
    heartbeat.supply_current = 5;
    heartbeat.tr_comm_status = 0x3;
    heartbeat.voltage_stability_flags = 0x001F;
    heartbeat.panel_temp = 451;

    std::vector<uint8_t> hb_encoded;
    ASSERT_TRUE(PayloadCodec::encode_heartbeat_payload(heartbeat, hb_encoded));
    EXPECT_TRUE(PayloadCodec::verify_heartbeat_crc(hb_encoded.data(), hb_encoded.size()));
}

TEST(ProtocolTests, RmaCodecRoundTrip)
{
    RmaPayload rma{};
    rma.header.session_id = 0x12345678u;
    rma.header.session_token = 0x0123456789ABCDEFULL;
    rma.header.cmd_type = 0x01;
    rma.header.cmd_flags = 0x01;
    rma.header.cmd_seq = 9;
    rma.body = {0xAA, 0xBB, 0xCC, 0xDD};

    std::vector<uint8_t> encoded;
    ASSERT_TRUE(PayloadCodec::encode_rma_payload(rma, encoded));
    EXPECT_TRUE(PayloadCodec::verify_rma_crc(encoded.data(), encoded.size()));

    RmaPayload decoded{};
    ASSERT_TRUE(PayloadCodec::decode_rma_payload(encoded.data(), encoded.size(), decoded));
    EXPECT_EQ(decoded.header.session_id, rma.header.session_id);
    EXPECT_EQ(decoded.body.size(), rma.body.size());
}

TEST(ProtocolTests, ValidatorModesAndCrcMismatch)
{
    ControlPayload control{};
    control.table_header.frame_counter = 1;
    control.table_header.frame_beam_total = 1;
    control.table_header.beams_per_second = 1;
    control.table_header.cpi_count_base = 10;
    control.beam_items.push_back(ControlBeamItem{});

    std::vector<uint8_t> control_payload;
    ASSERT_TRUE(PayloadCodec::encode_control_payload(control, control_payload));

    const std::vector<uint8_t> raw = wrap_packet(static_cast<uint8_t>(PacketType::CONTROL), control_payload);
    PacketParser parser;
    auto parsed = parser.parse(raw.data(), raw.size());
    ASSERT_TRUE(parsed.has_value());

    Validator data_only_validator(0x01, Validator::Scope::DATA_PLANE_ONLY);
    EXPECT_EQ(data_only_validator.validate(*parsed), ValidationResult::UNSUPPORTED_PACKET_TYPE);

    Validator full_validator(0x01, Validator::Scope::FULL_PROTOCOL);
    EXPECT_EQ(full_validator.validate(*parsed), ValidationResult::OK);

    std::vector<uint8_t> broken_payload = control_payload;
    broken_payload.back() ^= 0x11;
    const std::vector<uint8_t> broken_raw = wrap_packet(static_cast<uint8_t>(PacketType::CONTROL), broken_payload);
    auto broken_parsed = parser.parse(broken_raw.data(), broken_raw.size());
    ASSERT_TRUE(broken_parsed.has_value());
    EXPECT_EQ(full_validator.validate(*broken_parsed), ValidationResult::CRC_MISMATCH);
}

TEST(ProtocolTests, SequenceWraparoundHalfInterval)
{
    EXPECT_TRUE(receiver::protocol::is_sequence_newer(0xFFFFFFFFu, 0xFFFFFFFEu));
    EXPECT_TRUE(receiver::protocol::is_sequence_newer(0x00000000u, 0xFFFFFFFFu));
    EXPECT_TRUE(receiver::protocol::is_sequence_newer(0x00000001u, 0xFFFFFFFEu));
    EXPECT_FALSE(receiver::protocol::is_sequence_newer(0xFFFFFFFEu, 0x00000001u));
    EXPECT_FALSE(receiver::protocol::is_sequence_newer(0x00000001u, 0x00000001u));
}
