#include "qdgz300/m01_receiver/protocol/packet_parser.h"
#include <cstring>

namespace receiver
{
    namespace protocol
    {

        std::optional<ParsedPacket> PacketParser::parse(const uint8_t *buffer, size_t length)
        {
            if (buffer == nullptr || length < COMMON_HEADER_SIZE)
            {
                return std::nullopt;
            }

            ParsedPacket packet;
            parse_common_header(buffer, packet.header);
            if (!basic_validation(packet.header, length))
            {
                return std::nullopt;
            }

            packet.payload = buffer + COMMON_HEADER_SIZE;
            packet.total_size = length;
            return packet;
        }

        bool PacketParser::quick_check_magic(const uint8_t *buffer, size_t length)
        {
            if (buffer == nullptr || length < sizeof(uint32_t))
            {
                return false;
            }

            uint32_t magic = 0;
            std::memcpy(&magic, buffer, sizeof(magic));
            return magic == PROTOCOL_MAGIC;
        }

        void PacketParser::parse_common_header(const uint8_t *buffer, CommonHeader &header)
        {
            std::memcpy(&header, buffer, sizeof(header));
        }

        bool PacketParser::basic_validation(const CommonHeader &header, size_t total_length)
        {
            if (header.magic != PROTOCOL_MAGIC)
            {
                return false;
            }

            if ((header.protocol_version >> 4) != PROTOCOL_VERSION_MAJOR)
            {
                return false;
            }

            if (header.payload_len > MAX_PAYLOAD_SIZE)
            {
                return false;
            }

            if (COMMON_HEADER_SIZE + static_cast<size_t>(header.payload_len) != total_length)
            {
                return false;
            }

            return true;
        }

    } // namespace protocol
} // namespace receiver
