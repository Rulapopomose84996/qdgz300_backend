#include "qdgz300/m03_data_proc/fusion_engine.h"

#include "qdgz300/common/constants.h"

#include <algorithm>
#include <cmath>

namespace qdgz300::m03
{
    namespace
    {
        double normalize_heading_delta(double a, double b)
        {
            double delta = std::fabs(a - b);
            while (delta > 180.0)
            {
                delta -= 360.0;
            }
            return std::fabs(delta);
        }
    }

    bool FusionEngine::should_fuse(const TrackSnapshot &a, const TrackSnapshot &b) const
    {
        const double dx = a.pos_x - b.pos_x;
        const double dy = a.pos_y - b.pos_y;
        const double dz = a.pos_z - b.pos_z;
        const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);

        const double dvx = a.vel_x - b.vel_x;
        const double dvy = a.vel_y - b.vel_y;
        const double dvz = a.vel_z - b.vel_z;
        const double velocity_delta = std::sqrt(dvx * dvx + dvy * dvy + dvz * dvz);

        return distance <= D_HANDOVER_M &&
               velocity_delta <= V_HANDOVER_MS &&
               normalize_heading_delta(a.heading_deg, b.heading_deg) <= THETA_HANDOVER_DEG;
    }

    std::vector<TrackSnapshot> FusionEngine::fuse(const std::vector<TrackSnapshot> &tracks) const
    {
        std::vector<TrackSnapshot> result;
        std::vector<bool> merged(tracks.size(), false);

        for (size_t i = 0; i < tracks.size(); ++i)
        {
            if (merged[i])
            {
                continue;
            }

            TrackSnapshot survivor = tracks[i];
            for (size_t j = i + 1; j < tracks.size(); ++j)
            {
                if (merged[j] || !should_fuse(survivor, tracks[j]))
                {
                    continue;
                }

                if (tracks[j].confidence > survivor.confidence)
                {
                    survivor = tracks[j];
                }
                survivor.handover_flag = true;
                merged[j] = true;
            }
            result.push_back(survivor);
        }

        return result;
    }
}
