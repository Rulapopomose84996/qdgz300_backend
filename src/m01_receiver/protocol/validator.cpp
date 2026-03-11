#include "qdgz300/m01_receiver/protocol/validator.h"
#include "qdgz300/m01_receiver/protocol/crc32c.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace receiver
{
    namespace protocol
    {
        namespace
        {
            constexpr size_t kHeartbeatPayloadLen = HEARTBEAT_PAYLOAD_SIZE;
            constexpr size_t kHeartbeatCrcOffset = 44;
            constexpr size_t kHeartbeatDataLen = 44;

            bool verify_heartbeat_crc(const uint8_t *payload, size_t payload_len)
            {
                if (payload == nullptr || payload_len < kHeartbeatPayloadLen)
                {
                    return false;
                }
                uint32_t wire_crc = 0;
                std::memcpy(&wire_crc, payload + kHeartbeatCrcOffset, sizeof(wire_crc));
                const uint32_t calc_crc = crc32c(payload, kHeartbeatDataLen);
                return wire_crc == calc_crc;
            }
        } // namespace

        Validator::Validator(uint8_t local_device_id, Scope scope)
            : local_device_id_(local_device_id), scope_(scope)
        {
        }

        ValidationResult Validator::validate(const ParsedPacket &packet)
        {
            if (!packet.header.is_valid_magic())
            {
                return ValidationResult::INVALID_MAGIC;
            }

            if (!packet.header.is_valid_version())
            {
                return ValidationResult::INVALID_VERSION;
            }

            if (!check_dest_id(packet.header.dest_id))
            {
                return ValidationResult::INVALID_DEST_ID;
            }

            const PacketType type = packet.header.get_packet_type();
            if (scope_ == Scope::DATA_PLANE_ONLY && !is_data_packet(type))
            {
                return ValidationResult::UNSUPPORTED_PACKET_TYPE;
            }
            if (scope_ == Scope::DATA_AND_HEARTBEAT &&
                type != PacketType::DATA &&
                type != PacketType::HEARTBEAT)
            {
                return ValidationResult::UNSUPPORTED_PACKET_TYPE;
            }

            const size_t expected = COMMON_HEADER_SIZE + static_cast<size_t>(packet.header.payload_len);
            if (packet.total_size != expected)
            {
                return ValidationResult::PAYLOAD_LEN_MISMATCH;
            }

            (void)check_reserved_fields(packet.header);
            return validate_payload_by_type(packet);
        }

        bool Validator::check_dest_id(uint8_t dest_id) const
        {
            if (dest_id == static_cast<uint8_t>(DeviceID::RESERVED))
            {
                return false;
            }
            return dest_id == local_device_id_ ||
                   dest_id == static_cast<uint8_t>(DeviceID::BROADCAST);
        }

        bool Validator::is_data_packet(PacketType type) const
        {
            return type == PacketType::DATA;
        }

        bool Validator::check_reserved_fields(const CommonHeader &header) const
        {
            if (header.reserved1 != 0 || header.reserved2 != 0)
            {
                return false;
            }

            for (uint8_t value : header.reserved3)
            {
                if (value != 0)
                {
                    return false;
                }
            }
            return true;
        }

        ValidationResult Validator::validate_payload_by_type(const ParsedPacket &packet)
        {
            const PacketType type = packet.header.get_packet_type();
            if (type != PacketType::DATA && type != PacketType::HEARTBEAT)
            {
                return ValidationResult::UNSUPPORTED_PACKET_TYPE;
            }

            if (type == PacketType::DATA)
            {
                return ValidationResult::OK;
            }

            if (type == PacketType::HEARTBEAT)
            {
                return verify_heartbeat_crc(packet.payload, packet.header.payload_len)
                           ? ValidationResult::OK
                           : ValidationResult::CRC_MISMATCH;
            }

            return ValidationResult::UNSUPPORTED_PACKET_TYPE;
        }

    } // namespace protocol
} // namespace receiver
