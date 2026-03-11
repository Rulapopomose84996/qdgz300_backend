#pragma once

#include "qdgz300/common/types.h"

#include <cstddef>
#include <vector>

namespace qdgz300::m03
{
    struct TrackPrediction
    {
        uint64_t track_id{0};
        double x{0.0};
        double y{0.0};
        double z{0.0};
    };

    class PlotAssociator
    {
    public:
        struct Association
        {
            uint64_t track_id{0};
            size_t plot_index{0};
            double distance{0.0};
        };

        std::vector<Association> associate(const std::vector<TrackPrediction> &predictions,
                                           const std::vector<Plot> &plots,
                                           double gate_threshold_m);
        std::vector<size_t> unassociated_plots() const { return unassociated_plots_; }

    private:
        std::vector<size_t> unassociated_plots_;
    };
}
