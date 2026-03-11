#include "qdgz300/m04_gateway/udp_sender.h"

namespace qdgz300::m04
{
    bool UdpSender::send_frame(const TrackFrameData &frame)
    {
        last_payload_ = serializer_.serialize(frame);
        return !last_payload_.empty();
    }
}
