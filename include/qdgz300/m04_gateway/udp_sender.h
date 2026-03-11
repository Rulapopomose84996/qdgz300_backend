#pragma once

#include "qdgz300/m04_gateway/trackdata_serializer.h"

#include <cstdint>
#include <vector>

namespace qdgz300::m04
{
    class UdpSender
    {
    public:
        explicit UdpSender(uint16_t port) : port_(port) {}

        bool send_frame(const TrackFrameData &frame);
        const std::vector<uint8_t> &last_payload() const { return last_payload_; }
        uint16_t port() const { return port_; }

    private:
        uint16_t port_{0};
        TrackDataSerializer serializer_{};
        std::vector<uint8_t> last_payload_{};
    };
}
