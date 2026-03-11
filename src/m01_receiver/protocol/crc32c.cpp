#include "qdgz300/m01_receiver/protocol/crc32c.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#endif

namespace receiver
{
    namespace protocol
    {
        namespace
        {
            constexpr uint32_t kInitXor = 0xFFFFFFFFu;
            constexpr uint32_t kPolyReflected = 0x82F63B78u;

            std::array<uint32_t, 256> make_table()
            {
                std::array<uint32_t, 256> table{};
                for (uint32_t i = 0; i < 256; ++i)
                {
                    uint32_t crc = i;
                    for (int bit = 0; bit < 8; ++bit)
                    {
                        const uint32_t mask = static_cast<uint32_t>(-(static_cast<int32_t>(crc & 1u)));
                        crc = (crc >> 1) ^ (kPolyReflected & mask);
                    }
                    table[i] = crc;
                }
                return table;
            }

            uint32_t crc32c_sw(uint32_t crc, const uint8_t *data, size_t length)
            {
                static const std::array<uint32_t, 256> table = make_table();
                for (size_t i = 0; i < length; ++i)
                {
                    const uint8_t idx = static_cast<uint8_t>((crc ^ data[i]) & 0xFFu);
                    crc = table[idx] ^ (crc >> 8);
                }
                return crc;
            }

#if defined(__ARM_FEATURE_CRC32)
            uint32_t crc32c_arm(uint32_t crc, const uint8_t *data, size_t length)
            {
                size_t i = 0;
                while (i + sizeof(uint64_t) <= length)
                {
                    uint64_t v = 0;
                    std::memcpy(&v, data + i, sizeof(v));
                    crc = static_cast<uint32_t>(__crc32cd(crc, v));
                    i += sizeof(uint64_t);
                }

                while (i < length)
                {
                    crc = static_cast<uint32_t>(__crc32cb(crc, data[i]));
                    ++i;
                }
                return crc;
            }
#endif
        } // namespace

        uint32_t crc32c(const uint8_t *data, size_t length)
        {
            if (data == nullptr && length != 0)
            {
                return 0;
            }

            uint32_t crc = kInitXor;

#if defined(__ARM_FEATURE_CRC32)
            crc = crc32c_arm(crc, data, length);
#else
            crc = crc32c_sw(crc, data, length);
#endif

            return crc ^ kInitXor;
        }

    } // namespace protocol
} // namespace receiver
