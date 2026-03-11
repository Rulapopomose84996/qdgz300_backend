#pragma once

#include "qdgz300/common/types.h"

#include <cstddef>
#include <cstdint>

namespace qdgz300::m04
{
    using TrackFrameData = qdgz300::TrackFrame;

    class OutputThrottle
    {
    public:
        explicit OutputThrottle(uint32_t min_frame_gap_ms = 0) : min_frame_gap_ms_(min_frame_gap_ms) {}

        bool should_send(uint64_t frame_seq);
        TrackFrameData truncate(const TrackFrameData &frame, size_t max_tracks) const;

    private:
        uint32_t min_frame_gap_ms_{0};
        uint64_t last_frame_seq_{0};
    };
}
