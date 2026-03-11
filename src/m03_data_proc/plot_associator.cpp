#include "qdgz300/m03_data_proc/plot_associator.h"

#include <cmath>
#include <limits>
#include <unordered_set>

namespace qdgz300::m03
{
    namespace
    {
        void plot_to_cartesian(const Plot &plot, double &x, double &y, double &z)
        {
            const double az = plot.azimuth_deg * 3.14159265358979323846 / 180.0;
            const double el = plot.elevation_deg * 3.14159265358979323846 / 180.0;
            const double cos_el = std::cos(el);
            x = plot.range_m * cos_el * std::cos(az);
            y = plot.range_m * cos_el * std::sin(az);
            z = plot.range_m * std::sin(el);
        }
    }

    std::vector<PlotAssociator::Association> PlotAssociator::associate(const std::vector<TrackPrediction> &predictions,
                                                                       const std::vector<Plot> &plots,
                                                                       double gate_threshold_m)
    {
        std::vector<Association> associations;
        std::unordered_set<size_t> used_plots;

        for (const auto &prediction : predictions)
        {
            double best_distance = std::numeric_limits<double>::max();
            size_t best_plot = plots.size();

            for (size_t i = 0; i < plots.size(); ++i)
            {
                if (used_plots.find(i) != used_plots.end())
                {
                    continue;
                }

                double x = 0.0;
                double y = 0.0;
                double z = 0.0;
                plot_to_cartesian(plots[i], x, y, z);
                const double dx = x - prediction.x;
                const double dy = y - prediction.y;
                const double dz = z - prediction.z;
                const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (distance < best_distance)
                {
                    best_distance = distance;
                    best_plot = i;
                }
            }

            if (best_plot < plots.size() && best_distance <= gate_threshold_m)
            {
                used_plots.insert(best_plot);
                associations.push_back({prediction.track_id, best_plot, best_distance});
            }
        }

        unassociated_plots_.clear();
        for (size_t i = 0; i < plots.size(); ++i)
        {
            if (used_plots.find(i) == used_plots.end())
            {
                unassociated_plots_.push_back(i);
            }
        }

        return associations;
    }
}
