#pragma once

#include "qdgz300/common/types.h"

#include <vector>

namespace qdgz300::m03
{
    using TrackSnapshot = qdgz300::Track;

    class FusionEngine
    {
    public:
        bool should_fuse(const TrackSnapshot &a, const TrackSnapshot &b) const;
        std::vector<TrackSnapshot> fuse(const std::vector<TrackSnapshot> &tracks) const;
    };
}
