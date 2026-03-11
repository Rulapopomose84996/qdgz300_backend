#pragma once

#include "qdgz300/common/error_codes.h"
#include "qdgz300/common/types.h"
#include "qdgz300/m03_data_proc/fusion_engine.h"
#include "qdgz300/m03_data_proc/kalman_filter.h"
#include "qdgz300/m03_data_proc/plot_associator.h"

#include <cstdint>
#include <vector>

namespace qdgz300::m03
{
    struct TrackManagerConfig
    {
        uint32_t confirm_hits{3};
        uint32_t coast_after_misses{3};
        uint32_t lost_after_misses{10};
        double association_gate_m{120.0};
        double predict_dt_sec{0.05};
    };

    class TrackManager
    {
    public:
        explicit TrackManager(TrackManagerConfig config = {});

        std::vector<TrackSnapshot> process_plots(const std::vector<PlotBatch *> &batches);
        const std::vector<TrackSnapshot> &tracks() const { return tracks_; }
        size_t active_track_count() const { return tracks_.size(); }
        uint64_t next_track_id() const { return next_track_id_; }

    private:
        struct TrackState
        {
            TrackSnapshot snapshot{};
            KalmanFilter filter{};
            uint32_t hits{0};
            uint32_t misses{0};
        };

        TrackManagerConfig config_;
        uint64_t next_track_id_{1};
        PlotAssociator associator_;
        std::vector<TrackState> states_;
        std::vector<TrackSnapshot> tracks_;

        static Plot normalized_plot(const Plot &plot);
        static void plot_to_cartesian(const Plot &plot, double &x, double &y, double &z);
        void sync_snapshots();
    };
}
