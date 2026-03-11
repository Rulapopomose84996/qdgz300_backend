#include "qdgz300/m04_gateway/output_throttle.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

namespace qdgz300::m04
{
    bool OutputThrottle::should_send(uint64_t frame_seq)
    {
        if (last_frame_seq_ == 0)
        {
            last_frame_seq_ = frame_seq;
            return true;
        }

        if (frame_seq <= last_frame_seq_)
        {
            return false;
        }

        if (min_frame_gap_ms_ > 0 && (frame_seq - last_frame_seq_) < 1)
        {
            return false;
        }

        last_frame_seq_ = frame_seq;
        return true;
    }

    TrackFrameData OutputThrottle::truncate(const TrackFrameData &frame, size_t max_tracks) const
    {
        TrackFrameData out = frame;
        out.tracks = nullptr;

        if (frame.track_count <= max_tracks)
        {
            if (frame.track_count > 0)
            {
                auto *tracks = new qdgz300::Track[frame.track_count];
                std::memcpy(tracks, frame.tracks, sizeof(qdgz300::Track) * frame.track_count);
                out.tracks = tracks;
            }
            return out;
        }

        std::vector<qdgz300::Track> sorted(frame.tracks, frame.tracks + frame.track_count);
        std::sort(sorted.begin(), sorted.end(), [](const auto &lhs, const auto &rhs)
                  { return lhs.confidence > rhs.confidence; });

        auto *tracks = new qdgz300::Track[max_tracks];
        std::memcpy(tracks, sorted.data(), sizeof(qdgz300::Track) * max_tracks);
        out.tracks = tracks;
        out.track_count = static_cast<uint16_t>(max_tracks);
        out.is_truncated = true;
        out.dropped_count = static_cast<uint16_t>(frame.track_count - max_tracks);
        return out;
    }
}
