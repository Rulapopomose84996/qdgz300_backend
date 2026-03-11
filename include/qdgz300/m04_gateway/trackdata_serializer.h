#pragma once

#include "qdgz300/common/types.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace qdgz300::m04
{
    using TrackFrameData = qdgz300::TrackFrame;

    class TrackDataSerializer
    {
    public:
        std::vector<uint8_t> serialize(const TrackFrameData &frame) const;
        TrackFrameData deserialize(const uint8_t *data, size_t len) const;
    };
}
