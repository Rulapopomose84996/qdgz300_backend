#pragma once

#include "qdgz300/common/types.h"

#include <vector>

namespace qdgz300::m03
{
    using TrackSnapshot = qdgz300::Track;
    using TrackFrameData = qdgz300::TrackFrame;

    class TrackFrameBuilder
    {
    public:
        TrackFrameData build(uint64_t frame_seq,
                             const std::vector<TrackSnapshot> &tracks,
                             uint32_t coverage_mask) const;
    };
}
