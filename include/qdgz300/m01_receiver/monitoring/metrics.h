#ifndef RECEIVER_MONITORING_METRICS_H
#define RECEIVER_MONITORING_METRICS_H

#include "qdgz300/m01_receiver/protocol/protocol_types.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace receiver
{
    namespace monitoring
    {
        enum class PacketTypeIndex : size_t
        {
            DATA = 0,
            HEARTBEAT = 1,
            UNKNOWN = 2,
            COUNT = 3
        };

        enum class DropReasonIndex : size_t
        {
            INVALID_MAGIC = 0,
            VERSION_MISMATCH = 1,
            DEST_ID_MISMATCH = 2,
            LENGTH_MISMATCH = 3,
            NON_DATA_PACKET = 4,
            SEQUENCE_DUPLICATE = 5,
            SEQUENCE_OUT_OF_WINDOW = 6,
            BUFFER_OVERFLOW = 7,
            TIMEOUT = 8,
            CLOCK_UNLOCKED = 9,
            LATE_FRAGMENT = 10,
            REASM_DUPLICATE_FRAG = 11,
            REASM_TIMEOUT = 12,
            MAX_CONTEXTS_EXCEEDED = 13,
            REASM_BYTES_OVERFLOW = 14,
            COUNT = 15
        };


        /**
         * @brief Prometheus指标收集器
         *
         * 职责：
         * - 采集各模块的性能指标
         * - 暴露Prometheus格式的/metrics HTTP端点
         * - 支持Counter、Gauge、Histogram等指标类型
         */
        class MetricsCollector
        {
        public:
            /**
             * @brief 获取全局MetricsCollector单例
             */
            static MetricsCollector &instance();

            /**
             * @brief 初始化指标收集器
             * @param port HTTP端口（默认8080）
             * @param bind_ip 绑定IP地址
             */
            void initialize(uint16_t port = 8080, const std::string &bind_ip = "0.0.0.0");

            /**
             * @brief 启动HTTP metrics端点
             */
            bool start();

            /**
             * @brief 停止HTTP metrics端点
             */
            void stop();

            // ==================== Counter指标 ====================

            /**
             * @brief 接收到的数据包总数
             */
            void increment_packets_received(uint64_t count = 1);
            void increment_packets_received_by_type(PacketTypeIndex idx, uint64_t count = 1);
            [[deprecated("use increment_packets_received_by_type(PacketTypeIndex, ...)")]]
            void increment_packets_received(const std::string &packet_type, uint64_t count = 1);

            /**
             * @brief 丢弃的数据包总数（按原因分类）
             * @param reason 丢弃原因标签
             */
            void increment_packets_dropped_by_reason(DropReasonIndex idx, uint64_t count = 1);
            [[deprecated("use increment_packets_dropped_by_reason(DropReasonIndex, ...)")]]
            void increment_packets_dropped(const std::string &reason, uint64_t count = 1);

            /**
             * @brief 接收的字节数
             */
            void increment_bytes_received(uint64_t bytes);

            /**
             * @brief APPLIED_LATE计数
             */
            void increment_applied_late(uint64_t count = 1);

            /**
             * @brief 时钟未锁定计数
             */
            void increment_clock_unlocked(uint64_t count = 1);
            void increment_config_reloads(uint64_t count = 1);
            void increment_signal_received(const std::string &signal_name, uint64_t count = 1);
            void increment_heartbeat_packets_processed(uint64_t count = 1);
            void increment_heartbeat_sent(uint64_t count = 1);
            void increment_socket_packets_received(uint64_t count = 1);
            void increment_socket_bytes_received(uint64_t bytes);
            void increment_socket_receive_batches(uint64_t count = 1);
            void increment_socket_source_filtered(uint64_t count = 1);
            void increment_socket_receive_errors(uint64_t count = 1);
            void increment_pipeline_packets_entered(uint64_t count = 1);
            void increment_pipeline_parse_ok(uint64_t count = 1);
            void increment_pipeline_validate_ok(uint64_t count = 1);

            // ==================== Gauge指标 ====================

            /**
             * @brief 当前活跃重组上下文数
             */
            void set_active_reorder_contexts(size_t count);
            void set_missing_fragments_total(uint64_t count);

            /**
             * @brief 当前缓冲区使用率
             */
            void set_buffer_usage(size_t current, size_t max);
            void set_uptime_seconds(double uptime_seconds);
            void set_memory_rss_bytes(size_t rss_bytes);
            void set_numa_local_memory_pct(double pct);
            void set_heartbeat_queue_depth(size_t depth);
            void set_heartbeat_state(int state);
            void set_face_rx_queue_depth(uint8_t array_id, size_t depth);
            void set_face_rx_queue_high_watermark(uint8_t array_id, size_t depth);
            void set_face_rx_queue_drops(uint8_t array_id, uint64_t drops);
            void set_face_packet_pool_stats(uint8_t array_id, size_t total, size_t available, uint64_t fallback_alloc);

            // ==================== Histogram指标 ====================

            /**
             * @brief 记录数据延迟（秒）
             */
            void observe_data_delay(double delay_seconds);

            /**
             * @brief 记录处理延迟（微秒）
             */
            void observe_processing_latency(uint64_t latency_us);
            void observe_packet_pool_allocation_latency_ns(uint64_t latency_ns);
            void collect_system_metrics();

        private:
            MetricsCollector();
            ~MetricsCollector();

            // 禁用拷贝和移动
            MetricsCollector(const MetricsCollector &) = delete;
            MetricsCollector &operator=(const MetricsCollector &) = delete;

            class Impl;
            std::unique_ptr<Impl> impl_;
        };

// 便捷宏
#define METRICS_INC_PACKETS_RECEIVED(count) \
    receiver::monitoring::MetricsCollector::instance().increment_packets_received(count)

#define METRICS_INC_PACKETS_DROPPED(reason, count) \
    receiver::monitoring::MetricsCollector::instance().increment_packets_dropped(reason, count)

#define METRICS_OBSERVE_DELAY(delay_sec) \
    receiver::monitoring::MetricsCollector::instance().observe_data_delay(delay_sec)

    } // namespace monitoring
} // namespace receiver

#endif // RECEIVER_MONITORING_METRICS_H
