#include "qdgz300/m04_gateway/trackdata_serializer.h"

#include <cstring>
#include <stdexcept>

namespace qdgz300::m04
{
    namespace
    {
        template <typename T>
        void append_bytes(std::vector<uint8_t> &out, const T &value)
        {
            const auto *ptr = reinterpret_cast<const uint8_t *>(&value);
            out.insert(out.end(), ptr, ptr + sizeof(T));
        }

        template <typename T>
        T read_bytes(const uint8_t *data, size_t len, size_t &offset)
        {
            if (offset + sizeof(T) > len)
            {
                throw std::runtime_error("buffer underflow");
            }
            T value{};
            std::memcpy(&value, data + offset, sizeof(T));
            offset += sizeof(T);
            return value;
        }
    }

    std::vector<uint8_t> TrackDataSerializer::serialize(const TrackFrameData &frame) const
    {
        std::vector<uint8_t> out;
        out.reserve(sizeof(TrackFrameData) + static_cast<size_t>(frame.track_count) * sizeof(qdgz300::Track));

        append_bytes(out, frame.frame_seq);
        append_bytes(out, frame.data_timestamp_ns);
        append_bytes(out, frame.backend_instance_id);
        append_bytes(out, frame.clock_domain);
        append_bytes(out, frame.coverage_mask);
        append_bytes(out, frame.track_count);
        append_bytes(out, frame.system_quality_flags);
        append_bytes(out, frame.is_truncated);
        append_bytes(out, frame.dropped_count);

        for (uint16_t i = 0; i < frame.track_count; ++i)
        {
            append_bytes(out, frame.tracks[i]);
        }
        return out;
    }

    TrackFrameData TrackDataSerializer::deserialize(const uint8_t *data, size_t len) const
    {
        TrackFrameData frame{};
        size_t offset = 0;

        frame.frame_seq = read_bytes<uint64_t>(data, len, offset);
        frame.data_timestamp_ns = read_bytes<uint64_t>(data, len, offset);
        frame.backend_instance_id = read_bytes<uint64_t>(data, len, offset);
        frame.clock_domain = read_bytes<uint8_t>(data, len, offset);
        frame.coverage_mask = read_bytes<uint8_t>(data, len, offset);
        frame.track_count = read_bytes<uint16_t>(data, len, offset);
        frame.system_quality_flags = read_bytes<uint32_t>(data, len, offset);
        frame.is_truncated = read_bytes<bool>(data, len, offset);
        frame.dropped_count = read_bytes<uint16_t>(data, len, offset);
        frame.tracks = nullptr;

        if (frame.track_count > 0)
        {
            auto *tracks = new qdgz300::Track[frame.track_count];
            for (uint16_t i = 0; i < frame.track_count; ++i)
            {
                tracks[i] = read_bytes<qdgz300::Track>(data, len, offset);
            }
            frame.tracks = tracks;
        }

        return frame;
    }
}
