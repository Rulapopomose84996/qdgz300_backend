// include/qdgz300/logging/metrics_exporter.h
// Prometheus text format 指标导出器
// 周期性采集所有模块的 AtomicCounter / AtomicGauge，
// 以 Prometheus exposition format 输出
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <functional>
#include <mutex>
#include <atomic>

namespace qdgz300
{

    /// 指标类型（Prometheus 规范）
    enum class MetricType : uint8_t
    {
        COUNTER = 0,
        GAUGE = 1,
    };

    /// 单条指标描述
    struct MetricDescriptor
    {
        std::string name;             // e.g. "qdgz300_m01_packets_received_total"
        std::string help;             // 说明文本
        MetricType type;              // COUNTER or GAUGE
        std::function<double()> read; // 读取当前值的回调
    };

    /// Prometheus text format 导出器
    /// - 各模块在启动时注册指标描述
    /// - HTTP handler / 定时器 调用 export_text() 获取文本
    class MetricsExporter
    {
    public:
        /// 获取全局单例
        static MetricsExporter &instance() noexcept;

        /// 注册一条指标
        void register_metric(MetricDescriptor descriptor);

        /// 批量注册
        void register_metrics(std::vector<MetricDescriptor> descriptors);

        /// 导出 Prometheus text format
        /// @return 完整的 metrics 文本（可直接作为 HTTP response body）
        std::string export_text() const;

        /// 注册的指标总数
        size_t metric_count() const noexcept;

        /// 清除所有注册（仅用于测试）
        void clear() noexcept;

    private:
        MetricsExporter() = default;

        mutable std::mutex mutex_;
        std::vector<MetricDescriptor> metrics_;
    };

} // namespace qdgz300
