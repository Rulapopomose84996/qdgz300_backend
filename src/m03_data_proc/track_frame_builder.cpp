#include "qdgz300/m03_data_proc/track_frame_builder.h"

#include <cstring>
#include <memory>

namespace qdgz300::m03
{
    TrackFrameData TrackFrameBuilder::build(uint64_t frame_seq,
                                           const std::vector<TrackSnapshot> &tracks,
                                           uint32_t coverage_mask) const
    {
        TrackFrameData frame{};
        frame.frame_seq = frame_seq;
        frame.data_timestamp_ns = tracks.empty() ? 0 : tracks.front().update_ts;
        frame.backend_instance_id = 1;
        frame.clock_domain = 0;
        frame.coverage_mask = static_cast<uint8_t>(coverage_mask & 0xFFu);
        frame.track_count = static_cast<uint16_t>(tracks.size());
        frame.system_quality_flags = 0;
        frame.is_truncated = false;
        frame.dropped_count = 0;
        frame.tracks = nullptr;

        if (!tracks.empty())
        {
            auto copied = std::make_unique<Track[]>(tracks.size());
            std::memcpy(copied.get(), tracks.data(), sizeof(Track) * tracks.size());
            frame.tracks = copied.release();
        }

        return frame;
    }
}
