#ifndef RECEIVER_PROTOCOL_CRC32C_H
#define RECEIVER_PROTOCOL_CRC32C_H

#include <cstddef>
#include <cstdint>

namespace receiver
{
    namespace protocol
    {

        uint32_t crc32c(const uint8_t *data, size_t length);

    } // namespace protocol
} // namespace receiver

#endif // RECEIVER_PROTOCOL_CRC32C_H
