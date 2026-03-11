// src/logging/metrics_exporter.cpp
// Prometheus text format 导出器实现
#include "qdgz300/logging/metrics_exporter.h"

#include <sstream>
#include <iomanip>

namespace qdgz300
{

    MetricsExporter &MetricsExporter::instance() noexcept
    {
        static MetricsExporter inst;
        return inst;
    }

    void MetricsExporter::register_metric(MetricDescriptor descriptor)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        metrics_.push_back(std::move(descriptor));
    }

    void MetricsExporter::register_metrics(std::vector<MetricDescriptor> descriptors)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        metrics_.reserve(metrics_.size() + descriptors.size());
        for (auto &d : descriptors)
        {
            metrics_.push_back(std::move(d));
        }
    }

    std::string MetricsExporter::export_text() const
    {
        std::lock_guard<std::mutex> lock(mutex_);

        std::ostringstream oss;
        oss << std::fixed << std::setprecision(6);

        for (const auto &m : metrics_)
        {
            // HELP line
            oss << "# HELP " << m.name << " " << m.help << "\n";

            // TYPE line
            oss << "# TYPE " << m.name << " ";
            switch (m.type)
            {
            case MetricType::COUNTER:
                oss << "counter";
                break;
            case MetricType::GAUGE:
                oss << "gauge";
                break;
            }
            oss << "\n";

            // Metric value
            double value = 0.0;
            if (m.read)
            {
                value = m.read();
            }
            oss << m.name << " " << value << "\n";
        }

        return oss.str();
    }

    size_t MetricsExporter::metric_count() const noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return metrics_.size();
    }

    void MetricsExporter::clear() noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        metrics_.clear();
    }

} // namespace qdgz300
