#include "qdgz300/m01_receiver/protocol/packet_parser.h"
#include "qdgz300/m01_receiver/protocol/payload_codec.h"
#include "qdgz300/m01_receiver/protocol/protocol_types.h"
#include "qdgz300/m01_receiver/protocol/validator.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

using receiver::protocol::CommonHeader;
using receiver::protocol::ControlBeamItem;
using receiver::protocol::ControlPayload;
using receiver::protocol::HeartbeatPayload;
using receiver::protocol::PacketParser;
using receiver::protocol::PacketType;
using receiver::protocol::ParsedPacket;
using receiver::protocol::PayloadCodec;
using receiver::protocol::PROTOCOL_MAGIC;
using receiver::protocol::PROTOCOL_VERSION;
using receiver::protocol::RmaPayload;
using receiver::protocol::ScheduleApplyAckPayload;
using receiver::protocol::ValidationResult;
using receiver::protocol::Validator;

namespace
{
    struct ParsedPacketWithStorage
    {
        std::vector<uint8_t> raw;
        std::vector<uint8_t> payload_storage;
        ParsedPacket packet{};
    };

    std::vector<uint8_t> wrap_packet(uint8_t type,
                                     const std::vector<uint8_t> &payload,
                                     uint8_t dest_id = 0x01,
                                     uint32_t magic = PROTOCOL_MAGIC,
                                     uint8_t version = PROTOCOL_VERSION,
                                     uint8_t reserved1 = 0,
                                     uint16_t reserved2 = 0,
                                     std::array<uint8_t, 4> reserved3 = {0, 0, 0, 0})
    {
        std::vector<uint8_t> packet(receiver::protocol::COMMON_HEADER_SIZE + payload.size(), 0);
        CommonHeader header{};
        header.magic = magic;
        header.sequence_number = 7;
        header.timestamp = 1700000000000ULL;
        header.payload_len = static_cast<uint16_t>(payload.size());
        header.packet_type = type;
        header.protocol_version = version;
        header.source_id = 0x11;
        header.dest_id = dest_id;
        header.control_epoch = 1;
        header.reserved1 = reserved1;
        header.reserved2 = reserved2;
        std::memcpy(header.reserved3, reserved3.data(), reserved3.size());
        std::memcpy(packet.data(), &header, sizeof(header));
        if (!payload.empty())
        {
            std::memcpy(packet.data() + receiver::protocol::COMMON_HEADER_SIZE, payload.data(), payload.size());
        }
        return packet;
    }

    ParsedPacketWithStorage parse_wrapped_packet(const std::vector<uint8_t> &raw)
    {
        ParsedPacketWithStorage out{};
        out.raw = raw;
        PacketParser parser;
        const auto parsed = parser.parse(out.raw.data(), out.raw.size());
        EXPECT_TRUE(parsed.has_value());
        if (parsed.has_value())
        {
            out.packet = parsed.value();
        }
        return out;
    }

    ParsedPacketWithStorage make_manual_packet(uint32_t magic,
                                               uint8_t version,
                                               uint8_t dest_id,
                                               uint8_t packet_type,
                                               uint16_t payload_len,
                                               size_t total_size)
    {
        ParsedPacketWithStorage out{};
        out.payload_storage.resize(payload_len);
        out.packet.header.magic = magic;
        out.packet.header.protocol_version = version;
        out.packet.header.dest_id = dest_id;
        out.packet.header.source_id = 0x11;
        out.packet.header.packet_type = packet_type;
        out.packet.header.payload_len = payload_len;
        out.packet.payload = out.payload_storage.empty() ? nullptr : out.payload_storage.data();
        out.packet.total_size = total_size;
        return out;
    }

    std::vector<uint8_t> make_control_payload()
    {
        ControlPayload control{};
        control.table_header.frame_counter = 10;
        control.table_header.frame_beam_total = 1;
        control.table_header.beams_per_second = 1;
        control.table_header.cpi_count_base = 100;
        ControlBeamItem beam{};
        beam.beam_id = 1;
        control.beam_items.push_back(beam);
        std::vector<uint8_t> payload;
        EXPECT_TRUE(PayloadCodec::encode_control_payload(control, payload));
        return payload;
    }

    std::vector<uint8_t> make_ack_payload()
    {
        ScheduleApplyAckPayload ack{};
        ack.ack_frame_counter = 1;
        ack.acked_sequence_number = 7;
        ack.ack_result = 0;
        ack.error_code = 0;
        ack.error_detail = 0;
        ack.applied_timestamp = 1700000000000ULL;
        std::vector<uint8_t> payload;
        EXPECT_TRUE(PayloadCodec::encode_schedule_apply_ack_payload(ack, payload));
        return payload;
    }

    std::vector<uint8_t> make_heartbeat_payload()
    {
        HeartbeatPayload hb{};
        hb.system_status_alive = 1;
        hb.system_status_state = 2;
        hb.core_temp = 500;
        hb.op_mode = 2;
        std::vector<uint8_t> payload;
        EXPECT_TRUE(PayloadCodec::encode_heartbeat_payload(hb, payload));
        return payload;
    }

    std::vector<uint8_t> make_rma_payload()
    {
        RmaPayload rma{};
        rma.header.session_id = 0x1234;
        rma.header.session_token = 0xABCDEF1234567890ULL;
        rma.header.cmd_type = 0x01;
        rma.header.cmd_flags = 0;
        rma.header.cmd_seq = 1;
        rma.body = {0xAA, 0xBB};
        std::vector<uint8_t> payload;
        EXPECT_TRUE(PayloadCodec::encode_rma_payload(rma, payload));
        return payload;
    }
} // namespace

TEST(ValidatorTests, ValidMagicAndVersion)
{
    auto parsed = parse_wrapped_packet(wrap_packet(static_cast<uint8_t>(PacketType::DATA), {}));
    Validator validator(0x01, Validator::Scope::FULL_PROTOCOL);
    EXPECT_EQ(validator.validate(parsed.packet), ValidationResult::OK);
}

TEST(ValidatorTests, InvalidMagicVariants)
{
    Validator validator(0x01, Validator::Scope::FULL_PROTOCOL);
    auto p0 = make_manual_packet(0x00000000u, PROTOCOL_VERSION, 0x01, static_cast<uint8_t>(PacketType::DATA), 0, 32);
    auto p1 = make_manual_packet(0xFFFFFFFFu, PROTOCOL_VERSION, 0x01, static_cast<uint8_t>(PacketType::DATA), 0, 32);
    auto p2 = make_manual_packet(0x55AA55ABu, PROTOCOL_VERSION, 0x01, static_cast<uint8_t>(PacketType::DATA), 0, 32);
    EXPECT_EQ(validator.validate(p0.packet), ValidationResult::INVALID_MAGIC);
    EXPECT_EQ(validator.validate(p1.packet), ValidationResult::INVALID_MAGIC);
    EXPECT_EQ(validator.validate(p2.packet), ValidationResult::INVALID_MAGIC);
}

TEST(ValidatorTests, VersionMismatch)
{
    Validator validator(0x01, Validator::Scope::FULL_PROTOCOL);
    auto p0 = make_manual_packet(PROTOCOL_MAGIC, 0x21, 0x01, static_cast<uint8_t>(PacketType::DATA), 0, 32);
    auto p1 = make_manual_packet(PROTOCOL_MAGIC, 0x41, 0x01, static_cast<uint8_t>(PacketType::DATA), 0, 32);
    EXPECT_EQ(validator.validate(p0.packet), ValidationResult::INVALID_VERSION);
    EXPECT_EQ(validator.validate(p1.packet), ValidationResult::INVALID_VERSION);
}

TEST(ValidatorTests, VersionMinorDifference)
{
    Validator validator(0x01, Validator::Scope::FULL_PROTOCOL);
    auto packet = make_manual_packet(PROTOCOL_MAGIC, 0x32, 0x01, static_cast<uint8_t>(PacketType::DATA), 0, 32);
    EXPECT_EQ(validator.validate(packet.packet), ValidationResult::OK);
}

TEST(ValidatorTests, DestIdMatchLocal)
{
    Validator validator(0x22, Validator::Scope::FULL_PROTOCOL);
    auto packet = make_manual_packet(PROTOCOL_MAGIC, PROTOCOL_VERSION, 0x22, static_cast<uint8_t>(PacketType::DATA), 0, 32);
    EXPECT_EQ(validator.validate(packet.packet), ValidationResult::OK);
}

TEST(ValidatorTests, DestIdBroadcast)
{
    Validator validator(0x01, Validator::Scope::FULL_PROTOCOL);
    auto packet = make_manual_packet(PROTOCOL_MAGIC, PROTOCOL_VERSION, 0x10, static_cast<uint8_t>(PacketType::DATA), 0, 32);
    EXPECT_EQ(validator.validate(packet.packet), ValidationResult::OK);
}

TEST(ValidatorTests, DestIdMismatch)
{
    Validator validator(0x01, Validator::Scope::FULL_PROTOCOL);
    auto packet = make_manual_packet(PROTOCOL_MAGIC, PROTOCOL_VERSION, 0xFF, static_cast<uint8_t>(PacketType::DATA), 0, 32);
    EXPECT_EQ(validator.validate(packet.packet), ValidationResult::INVALID_DEST_ID);
}

TEST(ValidatorTests, PayloadLenZero)
{
    Validator validator(0x01, Validator::Scope::FULL_PROTOCOL);
    auto packet = make_manual_packet(PROTOCOL_MAGIC, PROTOCOL_VERSION, 0x01, static_cast<uint8_t>(PacketType::DATA), 0, 32);
    EXPECT_EQ(validator.validate(packet.packet), ValidationResult::OK);
}

TEST(ValidatorTests, PayloadLenMismatch)
{
    Validator validator(0x01, Validator::Scope::FULL_PROTOCOL);
    auto packet = make_manual_packet(PROTOCOL_MAGIC, PROTOCOL_VERSION, 0x01, static_cast<uint8_t>(PacketType::DATA), 4, 32);
    EXPECT_EQ(validator.validate(packet.packet), ValidationResult::PAYLOAD_LEN_MISMATCH);
}

TEST(ValidatorTests, DataPlaneOnlyRejectsControl)
{
    const auto control_payload = make_control_payload();
    auto parsed = parse_wrapped_packet(wrap_packet(static_cast<uint8_t>(PacketType::CONTROL), control_payload));
    Validator validator(0x01, Validator::Scope::DATA_PLANE_ONLY);
    EXPECT_EQ(validator.validate(parsed.packet), ValidationResult::UNSUPPORTED_PACKET_TYPE);
}

TEST(ValidatorTests, FullProtocolAliasAcceptsOnlyDataAndHeartbeat)
{
    Validator validator(0x01, Validator::Scope::FULL_PROTOCOL);

    auto control = parse_wrapped_packet(wrap_packet(static_cast<uint8_t>(PacketType::CONTROL), make_control_payload()));
    auto ack = parse_wrapped_packet(wrap_packet(static_cast<uint8_t>(PacketType::ACK), make_ack_payload()));
    auto data = parse_wrapped_packet(wrap_packet(static_cast<uint8_t>(PacketType::DATA), {}));
    auto heartbeat = parse_wrapped_packet(wrap_packet(static_cast<uint8_t>(PacketType::HEARTBEAT), make_heartbeat_payload()));
    auto rma = parse_wrapped_packet(wrap_packet(static_cast<uint8_t>(PacketType::RMA), make_rma_payload()));

    EXPECT_EQ(validator.validate(control.packet), ValidationResult::UNSUPPORTED_PACKET_TYPE);
    EXPECT_EQ(validator.validate(ack.packet), ValidationResult::UNSUPPORTED_PACKET_TYPE);
    EXPECT_EQ(validator.validate(data.packet), ValidationResult::OK);
    EXPECT_EQ(validator.validate(heartbeat.packet), ValidationResult::OK);
    EXPECT_EQ(validator.validate(rma.packet), ValidationResult::UNSUPPORTED_PACKET_TYPE);
}

TEST(ValidatorTests, ReservedFieldsNonZeroWarning)
{
    auto parsed = parse_wrapped_packet(wrap_packet(
        static_cast<uint8_t>(PacketType::DATA), {}, 0x01, PROTOCOL_MAGIC, PROTOCOL_VERSION, 1, 2, {3, 4, 5, 6}));
    Validator validator(0x01, Validator::Scope::FULL_PROTOCOL);
    EXPECT_FALSE(validator.check_reserved_fields(parsed.packet.header));
    EXPECT_EQ(validator.validate(parsed.packet), ValidationResult::OK);
}
