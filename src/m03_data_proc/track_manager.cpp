#include "qdgz300/m03_data_proc/track_manager.h"

#include <algorithm>
#include <cmath>

namespace qdgz300::m03
{
    namespace
    {
        constexpr double kPi = 3.14159265358979323846;
    }

    TrackManager::TrackManager(TrackManagerConfig config)
        : config_(config)
    {
    }

    Plot TrackManager::normalized_plot(const Plot &plot)
    {
        return plot;
    }

    void TrackManager::plot_to_cartesian(const Plot &plot, double &x, double &y, double &z)
    {
        const double az = plot.azimuth_deg * kPi / 180.0;
        const double el = plot.elevation_deg * kPi / 180.0;
        const double cos_el = std::cos(el);
        x = plot.range_m * cos_el * std::cos(az);
        y = plot.range_m * cos_el * std::sin(az);
        z = plot.range_m * std::sin(el);
    }

    std::vector<TrackSnapshot> TrackManager::process_plots(const std::vector<PlotBatch *> &batches)
    {
        std::vector<Plot> plots;
        uint64_t latest_ts = 0;

        for (const PlotBatch *batch : batches)
        {
            if (batch == nullptr || batch->plots == nullptr)
            {
                continue;
            }
            latest_ts = std::max(latest_ts, batch->data_ts);
            for (uint16_t i = 0; i < batch->plot_count; ++i)
            {
                plots.push_back(normalized_plot(batch->plots[i]));
            }
        }

        std::vector<TrackPrediction> predictions;
        predictions.reserve(states_.size());
        for (auto &state : states_)
        {
            state.filter.predict(config_.predict_dt_sec);
            predictions.push_back({state.snapshot.track_id, state.filter.x(), state.filter.y(), state.filter.z()});
        }

        const auto associations = associator_.associate(predictions, plots, config_.association_gate_m);
        std::vector<bool> matched(states_.size(), false);

        for (const auto &association : associations)
        {
            auto it = std::find_if(states_.begin(), states_.end(), [&](const TrackState &state)
                                   { return state.snapshot.track_id == association.track_id; });
            if (it == states_.end())
            {
                continue;
            }

            const size_t state_index = static_cast<size_t>(std::distance(states_.begin(), it));
            matched[state_index] = true;

            double x = 0.0;
            double y = 0.0;
            double z = 0.0;
            plot_to_cartesian(plots[association.plot_index], x, y, z);
            it->filter.update(x, y, z);
            it->hits++;
            it->misses = 0;
            it->snapshot.pos_x = it->filter.x();
            it->snapshot.pos_y = it->filter.y();
            it->snapshot.pos_z = it->filter.z();
            it->snapshot.vel_x = it->filter.vx();
            it->snapshot.vel_y = it->filter.vy();
            it->snapshot.vel_z = it->filter.vz();
            it->snapshot.heading_deg = it->filter.heading_deg();
            it->snapshot.confidence = std::min(1.0f, 0.2f * static_cast<float>(it->hits));
            it->snapshot.source_array_id = plots[association.plot_index].array_id;
            it->snapshot.update_ts = latest_ts;

            if (it->snapshot.lifecycle_state == static_cast<uint8_t>(TrackLifecycle::TENTATIVE) &&
                it->hits >= config_.confirm_hits)
            {
                it->snapshot.lifecycle_state = static_cast<uint8_t>(TrackLifecycle::CONFIRMED);
            }
            else if (it->snapshot.lifecycle_state == static_cast<uint8_t>(TrackLifecycle::COASTING))
            {
                it->snapshot.lifecycle_state = static_cast<uint8_t>(TrackLifecycle::CONFIRMED);
            }
        }

        for (size_t i = 0; i < states_.size(); ++i)
        {
            if (matched[i])
            {
                continue;
            }

            auto &state = states_[i];
            state.misses++;
            state.snapshot.coast_frames = state.misses;
            if (state.misses >= config_.lost_after_misses)
            {
                state.snapshot.lifecycle_state = static_cast<uint8_t>(TrackLifecycle::LOST);
            }
            else if (state.misses >= config_.coast_after_misses)
            {
                state.snapshot.lifecycle_state = static_cast<uint8_t>(TrackLifecycle::COASTING);
            }
            state.snapshot.update_ts = latest_ts;
        }

        for (size_t plot_index : associator_.unassociated_plots())
        {
            TrackState state;
            double x = 0.0;
            double y = 0.0;
            double z = 0.0;
            plot_to_cartesian(plots[plot_index], x, y, z);
            state.filter.init(x, y, z);
            state.snapshot.track_id = next_track_id_++;
            state.snapshot.pos_x = x;
            state.snapshot.pos_y = y;
            state.snapshot.pos_z = z;
            state.snapshot.heading_deg = 0.0;
            state.snapshot.confidence = 0.2f;
            state.snapshot.lifecycle_state = static_cast<uint8_t>(TrackLifecycle::TENTATIVE);
            state.snapshot.source_array_id = plots[plot_index].array_id;
            state.snapshot.handover_flag = false;
            state.snapshot.handover_from = 0;
            state.snapshot.coast_frames = 0;
            state.snapshot.create_ts = latest_ts;
            state.snapshot.update_ts = latest_ts;
            state.hits = 1;
            states_.push_back(state);
        }

        sync_snapshots();
        return tracks_;
    }

    void TrackManager::sync_snapshots()
    {
        tracks_.clear();
        tracks_.reserve(states_.size());
        for (const auto &state : states_)
        {
            if (state.snapshot.lifecycle_state == static_cast<uint8_t>(TrackLifecycle::LOST))
            {
                continue;
            }
            tracks_.push_back(state.snapshot);
        }
    }
}
