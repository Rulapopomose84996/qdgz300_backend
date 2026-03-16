#ifndef RECEIVER_CONFIG_CONFIG_MANAGER_H
#define RECEIVER_CONFIG_CONFIG_MANAGER_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace receiver
{
    namespace config
    {

        struct ReceiverConfig
        {
            struct Network
            {
                uint16_t listen_port = 9999;         // 统一监听端口（三阵面相同）
                std::vector<std::string> bind_ips{}; // 三阵面绑定IP地址列表（对应网卡三网口）
                std::string bind_ip = "0.0.0.0";     // 遗留单IP模式（向后兼容）
                std::string interface = "";
                uint8_t local_device_id = 0x01;
                int recv_threads = 8;
                size_t recvmmsg_batch_size = 64;
                size_t socket_rcvbuf_mb = 256;
                bool enable_so_reuseport = true;
                bool enable_ip_freebind = false;
                std::vector<uint8_t> source_id_map{};
                std::vector<int> cpu_affinity_map{};
                bool source_filter_enabled = true;
            } network;

            struct Reassembly
            {
                uint32_t timeout_ms = 100;
                size_t max_contexts = 1024;
                uint16_t max_total_frags = 1024;
                size_t sample_count_fixed = 4096;
                size_t max_reasm_bytes_per_key = 16u * 1024u * 1024u;
            } reassembly;

            struct Reorder
            {
                size_t window_size = 512;
                uint32_t timeout_ms = 50;
                bool enable_zero_fill = true;
            } reorder;

            struct Timestamp
            {
                uint32_t apply_window_ms = 5;
                bool enable_check = true;
                bool allow_unlocked_clock = true;
            } timestamp;

            struct ControlReliability
            {
                uint32_t rto_ms = 2500;
                uint32_t max_retry = 3;
                size_t dup_cmd_cache_size = 1000;
            } control_reliability;

            struct FlowControl
            {
                size_t max_buffer_size = 1024;
                size_t high_watermark = 800;
                size_t low_watermark = 200;
                std::string policy = "DROP_OLDEST";
            } flow_control;

            struct Logging
            {
                std::string level = "INFO";
                std::string log_file = "";
            } logging;

            struct Monitoring
            {
                uint16_t metrics_port = 8080;
                std::string metrics_bind_ip = "0.0.0.0";
            } monitoring;

            struct Performance
            {
                int numa_node = 1;
                size_t reassembler_cache_align_bytes = 64;
                bool prefetch_hints_enabled = true;
                bool qos_enabled = true;
                uint32_t rma_session_timeout_ms = 30000;
                size_t heartbeat_max_queue_depth = 1000;
                size_t packet_pool_mb_per_face = 64;
                size_t recv_drain_rounds = 4;
                std::vector<int> processing_cpu_affinity_map{};
            } performance;

            struct Delivery
            {
                std::string method = "callback";
                std::string shm_name = "/receiver_shm";
                size_t shm_size_mb = 64;
                std::string socket_path = "/tmp/tower_receiver.sock";
                uint32_t reconnect_interval_ms = 100;
            } delivery;

            struct Capture
            {
                bool enabled = false;
                std::string spool_dir = "/var/spool/qdgz300/receiver";
                std::string archive_dir = "/data/qdgz300/receiver";
                uint32_t spool_low_watermark_pct = 10;
                uint32_t archive_low_watermark_pct = 10;
                size_t archive_max_files = 256;
                uint32_t archive_max_age_days = 7;
                size_t max_file_size_mb = 1024;
                size_t max_files = 10;
                std::vector<uint8_t> filter_packet_types;
                std::vector<uint8_t> filter_source_ids;
            } capture;

            struct Queue
            {
                size_t rawcpi_q_capacity = 64;
                size_t rawcpi_q_slot_size_mb = 2;
            } queue;

            struct Consumer
            {
                bool print_summary = true;
                bool write_to_file = false;
                std::string output_dir = "/tmp/receiver_rawblocks";
                uint32_t stats_interval_ms = 1000;
            } consumer;
        };

        class ConfigManager
        {
        public:
            static ConfigManager &instance();

            bool load_from_file(const std::string &config_file);
            bool reload();
            const ReceiverConfig &get_config() const;
            void register_reload_callback(std::function<void(const ReceiverConfig &)> callback);
            static bool validate(const ReceiverConfig &config);

        private:
            ConfigManager();
            ~ConfigManager();

            ConfigManager(const ConfigManager &) = delete;
            ConfigManager &operator=(const ConfigManager &) = delete;

            class Impl;
            std::unique_ptr<Impl> impl_;
        };

    } // namespace config
} // namespace receiver

#endif // RECEIVER_CONFIG_CONFIG_MANAGER_H
