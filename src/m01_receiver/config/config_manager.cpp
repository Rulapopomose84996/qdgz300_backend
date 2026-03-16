#include "qdgz300/m01_receiver/config/config_manager.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace receiver
{
    namespace config
    {
        namespace
        {
            template <typename T>
            void assign_if_present(const YAML::Node &node, const char *key, T &target)
            {
                if (node && node[key])
                {
                    target = node[key].as<T>();
                }
            }

            template <typename T>
            void assign_sequence_if_present(const YAML::Node &node, const char *key, std::vector<T> &target)
            {
                if (!node || !node[key])
                {
                    return;
                }
                const YAML::Node seq = node[key];
                if (!seq.IsSequence())
                {
                    return;
                }
                std::vector<T> parsed;
                parsed.reserve(seq.size());
                for (const auto &value : seq)
                {
                    parsed.push_back(value.as<T>());
                }
                target = std::move(parsed);
            }

            void assign_hex_or_decimal_u8(const YAML::Node &node, const char *key, uint8_t &target)
            {
                if (!node || !node[key])
                {
                    return;
                }

                const std::string text = node[key].as<std::string>();
                const unsigned long parsed = std::stoul(text, nullptr, 0);
                target = static_cast<uint8_t>(parsed);
            }

            void assign_hex_or_decimal_u8_vector(const YAML::Node &node, const char *key, std::vector<uint8_t> &target)
            {
                if (!node || !node[key])
                {
                    return;
                }

                const YAML::Node list = node[key];
                if (!list.IsSequence())
                {
                    return;
                }

                std::vector<uint8_t> parsed_values;
                parsed_values.reserve(list.size());
                for (const auto &value : list)
                {
                    if (value.IsScalar())
                    {
                        const std::string text = value.as<std::string>();
                        const unsigned long parsed = std::stoul(text, nullptr, 0);
                        parsed_values.push_back(static_cast<uint8_t>(parsed));
                    }
                }
                target = std::move(parsed_values);
            }
        } // namespace

        class ConfigManager::Impl
        {
        public:
            ReceiverConfig current_config;
            std::string config_file_path;
            std::vector<std::function<void(const ReceiverConfig &)>> callbacks;
        };

        ConfigManager::ConfigManager()
            : impl_(std::make_unique<Impl>())
        {
        }

        ConfigManager::~ConfigManager() = default;

        ConfigManager &ConfigManager::instance()
        {
            static ConfigManager manager;
            return manager;
        }

        bool ConfigManager::load_from_file(const std::string &config_file)
        {
            ReceiverConfig cfg;

            try
            {
                const YAML::Node root = YAML::LoadFile(config_file);

                const YAML::Node network = root["network"];
                assign_if_present(network, "listen_port", cfg.network.listen_port);
                assign_sequence_if_present(network, "bind_ips", cfg.network.bind_ips);
                assign_if_present(network, "bind_ip", cfg.network.bind_ip);
                assign_if_present(network, "interface", cfg.network.interface);
                assign_hex_or_decimal_u8(network, "local_device_id", cfg.network.local_device_id);
                assign_if_present(network, "recv_threads", cfg.network.recv_threads);
                assign_if_present(network, "recvmmsg_batch_size", cfg.network.recvmmsg_batch_size);
                assign_if_present(network, "socket_rcvbuf_mb", cfg.network.socket_rcvbuf_mb);
                assign_if_present(network, "enable_so_reuseport", cfg.network.enable_so_reuseport);
                assign_if_present(network, "enable_ip_freebind", cfg.network.enable_ip_freebind);
                assign_hex_or_decimal_u8_vector(network, "source_id_map", cfg.network.source_id_map);
                assign_sequence_if_present(network, "cpu_affinity_map", cfg.network.cpu_affinity_map);
                assign_if_present(network, "source_filter_enabled", cfg.network.source_filter_enabled);

                const YAML::Node reassembly = root["reassembly"];
                assign_if_present(reassembly, "timeout_ms", cfg.reassembly.timeout_ms);
                assign_if_present(reassembly, "max_contexts", cfg.reassembly.max_contexts);
                assign_if_present(reassembly, "max_total_frags", cfg.reassembly.max_total_frags);
                assign_if_present(reassembly, "sample_count_fixed", cfg.reassembly.sample_count_fixed);
                assign_if_present(reassembly, "max_reasm_bytes_per_key", cfg.reassembly.max_reasm_bytes_per_key);

                const YAML::Node reorder = root["reorder"];
                assign_if_present(reorder, "window_size", cfg.reorder.window_size);
                assign_if_present(reorder, "timeout_ms", cfg.reorder.timeout_ms);
                assign_if_present(reorder, "enable_zero_fill", cfg.reorder.enable_zero_fill);

                const YAML::Node timestamp = root["timestamp"];
                assign_if_present(timestamp, "apply_window_ms", cfg.timestamp.apply_window_ms);
                assign_if_present(timestamp, "enable_check", cfg.timestamp.enable_check);
                assign_if_present(timestamp, "allow_unlocked_clock", cfg.timestamp.allow_unlocked_clock);

                const YAML::Node control_reliability = root["control_reliability"];
                assign_if_present(control_reliability, "rto_ms", cfg.control_reliability.rto_ms);
                assign_if_present(control_reliability, "max_retry", cfg.control_reliability.max_retry);
                assign_if_present(control_reliability, "dup_cmd_cache_size", cfg.control_reliability.dup_cmd_cache_size);

                const YAML::Node flow = root["flow_control"];
                assign_if_present(flow, "max_buffer_size", cfg.flow_control.max_buffer_size);
                assign_if_present(flow, "high_watermark", cfg.flow_control.high_watermark);
                assign_if_present(flow, "low_watermark", cfg.flow_control.low_watermark);
                assign_if_present(flow, "policy", cfg.flow_control.policy);

                const YAML::Node logging = root["logging"];
                assign_if_present(logging, "level", cfg.logging.level);
                assign_if_present(logging, "log_file", cfg.logging.log_file);

                const YAML::Node monitoring = root["monitoring"];
                assign_if_present(monitoring, "metrics_port", cfg.monitoring.metrics_port);
                assign_if_present(monitoring, "metrics_bind_ip", cfg.monitoring.metrics_bind_ip);

                const YAML::Node performance = root["performance"];
                assign_if_present(performance, "numa_node", cfg.performance.numa_node);
                assign_if_present(performance, "reassembler_cache_align_bytes", cfg.performance.reassembler_cache_align_bytes);
                assign_if_present(performance, "prefetch_hints_enabled", cfg.performance.prefetch_hints_enabled);
                assign_if_present(performance, "qos_enabled", cfg.performance.qos_enabled);
                assign_if_present(performance, "rma_session_timeout_ms", cfg.performance.rma_session_timeout_ms);
                assign_if_present(performance, "heartbeat_max_queue_depth", cfg.performance.heartbeat_max_queue_depth);

                const YAML::Node delivery = root["delivery"];
                assign_if_present(delivery, "method", cfg.delivery.method);
                assign_if_present(delivery, "shm_name", cfg.delivery.shm_name);
                assign_if_present(delivery, "shm_size_mb", cfg.delivery.shm_size_mb);
                assign_if_present(delivery, "socket_path", cfg.delivery.socket_path);
                assign_if_present(delivery, "reconnect_interval_ms", cfg.delivery.reconnect_interval_ms);

                const YAML::Node capture = root["capture"];
                assign_if_present(capture, "enabled", cfg.capture.enabled);
                assign_if_present(capture, "spool_dir", cfg.capture.spool_dir);
                if (cfg.capture.spool_dir == ReceiverConfig::Capture{}.spool_dir)
                {
                    assign_if_present(capture, "output_dir", cfg.capture.spool_dir);
                }
                assign_if_present(capture, "archive_dir", cfg.capture.archive_dir);
                assign_if_present(capture, "archive_max_files", cfg.capture.archive_max_files);
                assign_if_present(capture, "archive_max_age_days", cfg.capture.archive_max_age_days);
                assign_if_present(capture, "max_file_size_mb", cfg.capture.max_file_size_mb);
                assign_if_present(capture, "max_files", cfg.capture.max_files);
                assign_hex_or_decimal_u8_vector(capture, "filter_packet_types", cfg.capture.filter_packet_types);
                assign_hex_or_decimal_u8_vector(capture, "filter_source_ids", cfg.capture.filter_source_ids);

                const YAML::Node queue = root["queue"];
                assign_if_present(queue, "rawcpi_q_capacity", cfg.queue.rawcpi_q_capacity);
                assign_if_present(queue, "rawcpi_q_slot_size_mb", cfg.queue.rawcpi_q_slot_size_mb);

                const YAML::Node consumer = root["consumer"];
                assign_if_present(consumer, "print_summary", cfg.consumer.print_summary);
                assign_if_present(consumer, "write_to_file", cfg.consumer.write_to_file);
                assign_if_present(consumer, "output_dir", cfg.consumer.output_dir);
                assign_if_present(consumer, "stats_interval_ms", cfg.consumer.stats_interval_ms);
            }
            catch (const YAML::Exception &)
            {
                return false;
            }

            if (!validate(cfg))
            {
                return false;
            }

            impl_->current_config = cfg;
            impl_->config_file_path = config_file;
            return true;
        }

        bool ConfigManager::reload()
        {
            if (impl_->config_file_path.empty())
            {
                return false;
            }

            if (!load_from_file(impl_->config_file_path))
            {
                return false;
            }

            for (const auto &cb : impl_->callbacks)
            {
                cb(impl_->current_config);
            }
            return true;
        }

        const ReceiverConfig &ConfigManager::get_config() const
        {
            return impl_->current_config;
        }

        void ConfigManager::register_reload_callback(std::function<void(const ReceiverConfig &)> callback)
        {
            impl_->callbacks.push_back(std::move(callback));
        }

        bool ConfigManager::validate(const ReceiverConfig &config)
        {
            if (config.network.listen_port == 0)
            {
                return false;
            }
            if (config.network.recv_threads <= 0)
            {
                return false;
            }
            // 验证三阵面配置：必须同时有 bind_ips, source_id_map, cpu_affinity_map
            const bool has_any_array_face_field =
                !config.network.bind_ips.empty() ||
                !config.network.source_id_map.empty() ||
                !config.network.cpu_affinity_map.empty();
            if (has_any_array_face_field &&
                (config.network.bind_ips.size() != 3 ||
                 config.network.source_id_map.size() != 3 ||
                 config.network.cpu_affinity_map.size() != 3))
            {
                return false;
            }
            // 验证 IP 地址合法性
            if (!config.network.bind_ips.empty())
            {
                std::unordered_set<std::string> unique_ips(
                    config.network.bind_ips.begin(),
                    config.network.bind_ips.end());
                if (unique_ips.size() != config.network.bind_ips.size())
                {
                    return false; // IP 地址不能重复
                }
                for (const std::string &ip : config.network.bind_ips)
                {
                    if (ip.empty())
                    {
                        return false;
                    }
                }
            }
            if (!config.network.source_id_map.empty())
            {
                std::unordered_set<uint8_t> unique_source_ids(
                    config.network.source_id_map.begin(),
                    config.network.source_id_map.end());
                if (unique_source_ids.size() != config.network.source_id_map.size())
                {
                    return false;
                }
                for (const uint8_t source_id : config.network.source_id_map)
                {
                    if (source_id < 0x11 || source_id > 0x13)
                    {
                        return false;
                    }
                }
            }
            if (!config.network.cpu_affinity_map.empty())
            {
                std::unordered_set<int> unique_cpus(
                    config.network.cpu_affinity_map.begin(),
                    config.network.cpu_affinity_map.end());
                if (unique_cpus.size() != config.network.cpu_affinity_map.size())
                {
                    return false;
                }
                const auto min_cpu = *std::min_element(config.network.cpu_affinity_map.begin(), config.network.cpu_affinity_map.end());
                if (min_cpu < 0)
                {
                    return false;
                }
            }
            if (config.reassembly.max_total_frags == 0 || config.reassembly.max_total_frags > 1024)
            {
                return false;
            }
            if (config.reassembly.max_reasm_bytes_per_key == 0)
            {
                return false;
            }
            if (config.reorder.window_size == 0)
            {
                return false;
            }
            if (config.control_reliability.rto_ms == 0 || config.control_reliability.rto_ms > 60000)
            {
                return false;
            }
            if (config.control_reliability.max_retry == 0 || config.control_reliability.max_retry > 10)
            {
                return false;
            }
            if (config.control_reliability.dup_cmd_cache_size == 0 || config.control_reliability.dup_cmd_cache_size > 100000)
            {
                return false;
            }
            if (config.flow_control.low_watermark > config.flow_control.high_watermark)
            {
                return false;
            }
            if (config.flow_control.high_watermark > config.flow_control.max_buffer_size)
            {
                return false;
            }
            if (config.flow_control.policy != "DROP_OLDEST" && config.flow_control.policy != "REJECT_NEW")
            {
                return false;
            }
            if (config.performance.numa_node != 1)
            {
                return false;
            }
            if (config.performance.reassembler_cache_align_bytes < 64 ||
                (config.performance.reassembler_cache_align_bytes & (config.performance.reassembler_cache_align_bytes - 1)) != 0)
            {
                return false;
            }
            if (config.performance.rma_session_timeout_ms == 0 || config.performance.rma_session_timeout_ms > 300000)
            {
                return false;
            }
            if (config.performance.heartbeat_max_queue_depth == 0 || config.performance.heartbeat_max_queue_depth > 100000)
            {
                return false;
            }
            if (config.delivery.reconnect_interval_ms == 0)
            {
                return false;
            }
            if (config.capture.max_files == 0)
            {
                return false;
            }
            if (config.capture.enabled && config.capture.spool_dir.empty())
            {
                return false;
            }
            if (config.capture.archive_max_files == 0 || config.capture.archive_max_files > 100000)
            {
                return false;
            }
            if (config.capture.archive_max_age_days == 0 || config.capture.archive_max_age_days > 3650)
            {
                return false;
            }
            if (config.queue.rawcpi_q_capacity == 0 || config.queue.rawcpi_q_capacity > 1024)
            {
                return false;
            }
            if (config.queue.rawcpi_q_slot_size_mb == 0 || config.queue.rawcpi_q_slot_size_mb > 16)
            {
                return false;
            }
            if (config.consumer.stats_interval_ms == 0 || config.consumer.stats_interval_ms > 60000)
            {
                return false;
            }
            if (config.consumer.write_to_file && config.consumer.output_dir.empty())
            {
                return false;
            }

            return true;
        }

    } // namespace config
} // namespace receiver
