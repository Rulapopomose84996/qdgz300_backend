#include "qdgz300/m01_receiver/monitoring/metrics.h"
#include <array>
#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <sstream>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#if defined(RECEIVER_HAS_LIBNUMA)
#include <numa.h>
#include <sched.h>
#endif

namespace receiver
{
    namespace monitoring
    {
        namespace
        {
            using receiver::monitoring::DropReasonIndex;
            using receiver::monitoring::PacketTypeIndex;

            constexpr size_t DATA_DELAY_BUCKET_COUNT = 8;
            constexpr std::array<double, DATA_DELAY_BUCKET_COUNT> DATA_DELAY_BUCKET_BOUNDS = {
                0.001, 0.005, 0.010, 0.050, 0.100, 0.500, 1.000, 5.000};

            constexpr size_t PROCESSING_LATENCY_BUCKET_COUNT = 8;
            constexpr std::array<uint64_t, PROCESSING_LATENCY_BUCKET_COUNT> PROCESSING_LATENCY_BUCKET_BOUNDS = {
                100, 500, 1000, 5000, 10000, 50000, 100000, 500000};

            constexpr size_t PACKET_POOL_ALLOC_LATENCY_BUCKET_COUNT = 8;
            constexpr std::array<uint64_t, PACKET_POOL_ALLOC_LATENCY_BUCKET_COUNT> PACKET_POOL_ALLOC_LATENCY_BUCKET_BOUNDS = {
                100, 500, 1000, 5000, 10000, 50000, 100000, 500000};

            void atomic_add_double(std::atomic<double> &target, double delta)
            {
                double current = target.load(std::memory_order_relaxed);                                                              // numeric accumulator, ordering independent
                while (!target.compare_exchange_weak(current, current + delta, std::memory_order_relaxed, std::memory_order_relaxed)) // numeric accumulator, ordering independent
                {
                }
            }

            constexpr size_t PACKET_TYPE_INDEX_COUNT = static_cast<size_t>(PacketTypeIndex::COUNT);
            constexpr size_t DROP_REASON_INDEX_COUNT = static_cast<size_t>(DropReasonIndex::COUNT);

            constexpr std::array<const char *, PACKET_TYPE_INDEX_COUNT> PACKET_TYPE_LABELS = {
                "data",
                "heartbeat",
                "unknown"};

            constexpr std::array<const char *, DROP_REASON_INDEX_COUNT> DROP_REASON_LABELS = {
                "INVALID_MAGIC",
                "VERSION_MISMATCH",
                "DEST_ID_MISMATCH",
                "LENGTH_MISMATCH",
                "NON_DATA_PACKET",
                "SEQUENCE_DUPLICATE",
                "SEQUENCE_OUT_OF_WINDOW",
                "BUFFER_OVERFLOW",
                "TIMEOUT",
                "CLOCK_UNLOCKED",
                "LATE_FRAGMENT",
                "REASM_DUPLICATE_FRAG",
                "REASM_TIMEOUT",
                "MAX_CONTEXTS_EXCEEDED",
                "REASM_BYTES_OVERFLOW"};

            constexpr size_t SIGNAL_INDEX_COUNT = 3;
            constexpr std::array<const char *, SIGNAL_INDEX_COUNT> SIGNAL_LABELS = {
                "SIGHUP",
                "SIGTERM",
                "SIGINT"};

            constexpr bool is_valid_packet_type_index(PacketTypeIndex idx)
            {
                return static_cast<size_t>(idx) < PACKET_TYPE_INDEX_COUNT;
            }

            constexpr bool is_valid_drop_reason_index(DropReasonIndex idx)
            {
                return static_cast<size_t>(idx) < DROP_REASON_INDEX_COUNT;
            }

            PacketTypeIndex packet_type_index_from_string(const std::string &packet_type)
            {
                return (packet_type == "data")      ? PacketTypeIndex::DATA
                       : (packet_type == "heartbeat")
                           ? PacketTypeIndex::HEARTBEAT
                           : PacketTypeIndex::UNKNOWN;
            }

            DropReasonIndex drop_reason_index_from_string(const std::string &reason)
            {
                return (reason == "INVALID_MAGIC")        ? DropReasonIndex::INVALID_MAGIC
                       : (reason == "VERSION_MISMATCH")   ? DropReasonIndex::VERSION_MISMATCH
                       : (reason == "DEST_ID_MISMATCH")   ? DropReasonIndex::DEST_ID_MISMATCH
                       : (reason == "LENGTH_MISMATCH")    ? DropReasonIndex::LENGTH_MISMATCH
                       : (reason == "NON_DATA_PACKET")    ? DropReasonIndex::NON_DATA_PACKET
                       : (reason == "SEQUENCE_DUPLICATE") ? DropReasonIndex::SEQUENCE_DUPLICATE
                       : (reason == "SEQUENCE_OUT_OF_WINDOW")
                           ? DropReasonIndex::SEQUENCE_OUT_OF_WINDOW
                       : (reason == "BUFFER_OVERFLOW")      ? DropReasonIndex::BUFFER_OVERFLOW
                       : (reason == "TIMEOUT")              ? DropReasonIndex::TIMEOUT
                       : (reason == "CLOCK_UNLOCKED")       ? DropReasonIndex::CLOCK_UNLOCKED
                       : (reason == "LATE_FRAGMENT")        ? DropReasonIndex::LATE_FRAGMENT
                       : (reason == "REASM_DUPLICATE_FRAG") ? DropReasonIndex::REASM_DUPLICATE_FRAG
                       : (reason == "REASM_TIMEOUT")        ? DropReasonIndex::REASM_TIMEOUT
                       : (reason == "MAX_CONTEXTS_EXCEEDED")
                           ? DropReasonIndex::MAX_CONTEXTS_EXCEEDED
                       : (reason == "REASM_BYTES_OVERFLOW")
                           ? DropReasonIndex::REASM_BYTES_OVERFLOW
                       : (reason == "parse_failed") ? DropReasonIndex::LENGTH_MISMATCH
                       : (reason == "validation_failed")
                           ? DropReasonIndex::LENGTH_MISMATCH
                           : DropReasonIndex::LENGTH_MISMATCH;
            }

            std::optional<size_t> signal_index_from_string(const std::string &signal_name)
            {
                for (size_t i = 0; i < SIGNAL_LABELS.size(); ++i)
                {
                    if (signal_name == SIGNAL_LABELS[i])
                    {
                        return i;
                    }
                }
                return std::nullopt;
            }

            size_t read_process_rss_bytes()
            {
                std::ifstream status("/proc/self/status");
                if (!status.is_open())
                {
                    return 0;
                }

                std::string line;
                while (std::getline(status, line))
                {
                    if (line.rfind("VmRSS:", 0) == 0)
                    {
                        std::istringstream iss(line.substr(6));
                        size_t kb = 0;
                        iss >> kb;
                        return kb * 1024u;
                    }
                }
                return 0;
            }

            std::string external_metrics_file_path()
            {
                if (const char *override_path = std::getenv("QDGZ300_EXTERNAL_METRICS_FILE"))
                {
                    if (*override_path != '\0')
                    {
                        return override_path;
                    }
                }
                return "/opt/qdgz300_backend/data/metrics/qdgz300_spool_mover.prom";
            }

            std::string read_external_metrics_payload()
            {
                const std::string path = external_metrics_file_path();
                std::ifstream input(path);
                if (!input.is_open())
                {
                    return {};
                }

                std::ostringstream buffer;
                buffer << input.rdbuf();
                std::string payload = buffer.str();
                if (!payload.empty() && payload.back() != '\n')
                {
                    payload.push_back('\n');
                }
                return payload;
            }
        }

        class MetricsCollector::Impl
        {
        public:
            std::atomic<bool> started{false};
            uint16_t port{8080};
            std::string bind_ip{"0.0.0.0"};
            std::mutex mutex;
            std::thread server_thread;

            std::atomic<uint64_t> packets_received{0};
            std::array<std::atomic<uint64_t>, PACKET_TYPE_INDEX_COUNT> packets_by_type_atomic{};
            std::array<std::atomic<uint64_t>, DROP_REASON_INDEX_COUNT> packets_by_reason_atomic{};
            std::atomic<uint64_t> bytes_received{0};
            std::atomic<uint64_t> applied_late{0};
            std::atomic<uint64_t> clock_unlocked{0};
            std::atomic<uint64_t> missing_fragments_total{0};
            std::atomic<uint64_t> config_reload_total{0};
            std::array<std::atomic<uint64_t>, SIGNAL_INDEX_COUNT> signal_received_total{};
            std::atomic<uint64_t> heartbeat_packets_processed{0};
            std::atomic<uint64_t> heartbeat_sent_total{0};
            std::atomic<uint64_t> socket_packets_received_total{0};
            std::atomic<uint64_t> socket_bytes_received_total{0};
            std::atomic<uint64_t> socket_receive_batches_total{0};
            std::atomic<uint64_t> socket_source_filtered_total{0};
            std::atomic<uint64_t> socket_receive_errors_total{0};
            std::atomic<uint64_t> pipeline_packets_entered_total{0};
            std::atomic<uint64_t> pipeline_parse_ok_total{0};
            std::atomic<uint64_t> pipeline_validate_ok_total{0};

            std::atomic<size_t> active_contexts{0};
            std::atomic<size_t> buffer_current{0};
            std::atomic<size_t> buffer_max{0};
            std::atomic<double> uptime_seconds{0.0};
            std::atomic<size_t> memory_rss_bytes{0};
            std::atomic<double> numa_local_memory_pct{0.0};
            std::atomic<size_t> heartbeat_queue_depth{0};
            std::atomic<int> heartbeat_state{0};

            std::array<std::atomic<uint64_t>, DATA_DELAY_BUCKET_COUNT + 1> data_delay_bucket_counts{};
            std::atomic<uint64_t> data_delay_count{0};
            std::atomic<double> data_delay_sum{0.0};

            std::array<std::atomic<uint64_t>, PROCESSING_LATENCY_BUCKET_COUNT + 1> processing_latency_bucket_counts{};
            std::atomic<uint64_t> processing_latency_count{0};
            std::atomic<uint64_t> processing_latency_sum_us{0};

            std::array<std::atomic<uint64_t>, PACKET_POOL_ALLOC_LATENCY_BUCKET_COUNT + 1> packet_pool_alloc_latency_bucket_counts{};
            std::atomic<uint64_t> packet_pool_alloc_latency_count{0};
            std::atomic<uint64_t> packet_pool_alloc_latency_sum_ns{0};

            std::chrono::steady_clock::time_point startup_time = std::chrono::steady_clock::now();

            int listen_fd{-1};
        };

        MetricsCollector::MetricsCollector()
            : impl_(std::make_unique<Impl>())
        {
        }

        MetricsCollector::~MetricsCollector()
        {
            stop();
        }

        MetricsCollector &MetricsCollector::instance()
        {
            static MetricsCollector collector;
            return collector;
        }

        void MetricsCollector::initialize(uint16_t port, const std::string &bind_ip)
        {
            impl_->port = port;
            impl_->bind_ip = bind_ip;
            impl_->startup_time = std::chrono::steady_clock::now();
        }

        static void bind_to_management_numa_node()
        {
#if defined(__linux__)
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);
            for (int cpu = 0; cpu <= 15; ++cpu)
            {
                CPU_SET(cpu, &cpuset);
            }
            (void)sched_setaffinity(0, sizeof(cpuset), &cpuset);
#endif
#if defined(RECEIVER_HAS_LIBNUMA)
            numa_set_preferred(0);
#endif
        }

        static void append_counter(std::ostringstream &out,
                                   const std::string &name,
                                   const std::string &help,
                                   uint64_t value)
        {
            out << "# HELP " << name << ' ' << help << "\n";
            out << "# TYPE " << name << " counter\n";
            out << name << ' ' << value << "\n";
        }

        static void append_gauge(std::ostringstream &out,
                                 const std::string &name,
                                 const std::string &help,
                                 double value)
        {
            out << "# HELP " << name << ' ' << help << "\n";
            out << "# TYPE " << name << " gauge\n";
            out << name << ' ' << value << "\n";
        }

        static void append_histogram_double(std::ostringstream &out,
                                            const std::string &name,
                                            const std::string &help,
                                            const std::array<double, DATA_DELAY_BUCKET_COUNT> &bounds,
                                            const std::array<std::atomic<uint64_t>, DATA_DELAY_BUCKET_COUNT + 1> &bucket_counts,
                                            uint64_t count,
                                            double sum)
        {
            out << "# HELP " << name << ' ' << help << "\n";
            out << "# TYPE " << name << " histogram\n";

            uint64_t cumulative = 0;
            for (size_t i = 0; i < bounds.size(); ++i)
            {
                cumulative += bucket_counts[i].load(std::memory_order_relaxed); // histogram counters are independent statistics
                out << name << "_bucket{le=\"" << bounds[i] << "\"} " << cumulative << "\n";
            }
            cumulative += bucket_counts[bounds.size()].load(std::memory_order_relaxed); // histogram counters are independent statistics
            out << name << "_bucket{le=\"+Inf\"} " << cumulative << "\n";
            out << name << "_sum " << sum << "\n";
            out << name << "_count " << count << "\n";
        }

        static void append_histogram_uint64(std::ostringstream &out,
                                            const std::string &name,
                                            const std::string &help,
                                            const std::array<uint64_t, PROCESSING_LATENCY_BUCKET_COUNT> &bounds,
                                            const std::array<std::atomic<uint64_t>, PROCESSING_LATENCY_BUCKET_COUNT + 1> &bucket_counts,
                                            uint64_t count,
                                            uint64_t sum)
        {
            out << "# HELP " << name << ' ' << help << "\n";
            out << "# TYPE " << name << " histogram\n";

            uint64_t cumulative = 0;
            for (size_t i = 0; i < bounds.size(); ++i)
            {
                cumulative += bucket_counts[i].load(std::memory_order_relaxed); // histogram counters are independent statistics
                out << name << "_bucket{le=\"" << bounds[i] << "\"} " << cumulative << "\n";
            }
            cumulative += bucket_counts[bounds.size()].load(std::memory_order_relaxed); // histogram counters are independent statistics
            out << name << "_bucket{le=\"+Inf\"} " << cumulative << "\n";
            out << name << "_sum " << sum << "\n";
            out << name << "_count " << count << "\n";
        }

        static double histogram_quantile_from_cumulative(const std::vector<std::pair<double, uint64_t>> &buckets,
                                                         uint64_t total_count,
                                                         double q)
        {
            if (total_count == 0 || buckets.empty())
            {
                return 0.0;
            }

            const double rank = q * static_cast<double>(total_count);
            uint64_t prev_cum = 0;
            double prev_bound = 0.0;
            for (const auto &entry : buckets)
            {
                const double bound = entry.first;
                const uint64_t cum = entry.second;
                if (static_cast<double>(cum) >= rank)
                {
                    const uint64_t bucket_count = (cum > prev_cum) ? (cum - prev_cum) : 0;
                    if (bucket_count == 0)
                    {
                        return bound;
                    }
                    const double pos_in_bucket = (rank - static_cast<double>(prev_cum)) / static_cast<double>(bucket_count);
                    return prev_bound + (bound - prev_bound) * pos_in_bucket;
                }
                prev_cum = cum;
                prev_bound = bound;
            }
            return buckets.back().first;
        }

        template <typename ImplType>
        static std::string build_metrics_payload(ImplType *impl)
        {
            std::lock_guard<std::mutex> lock(impl->mutex);
            std::ostringstream out;

            append_counter(out,
                           "receiver_packets_received_total",
                           "Total received packets",
                           impl->packets_received.load(std::memory_order_relaxed)); // counter snapshot only
            append_counter(out,
                           "receiver_bytes_received_total",
                           "Total received bytes",
                           impl->bytes_received.load(std::memory_order_relaxed)); // counter snapshot only
            append_counter(out,
                           "receiver_applied_late_total",
                           "Total applied-late events",
                           impl->applied_late.load(std::memory_order_relaxed)); // counter snapshot only
            append_counter(out,
                           "receiver_clock_unlocked_total",
                           "Total clock unlocked events",
                           impl->clock_unlocked.load(std::memory_order_relaxed)); // counter snapshot only
            append_counter(out,
                           "receiver_config_reload_total",
                           "Total successful config reloads",
                           impl->config_reload_total.load(std::memory_order_relaxed));
            append_counter(out,
                           "heartbeat_packets_processed",
                           "Total heartbeat packets processed",
                           impl->heartbeat_packets_processed.load(std::memory_order_relaxed));
            append_counter(out,
                           "receiver_heartbeat_sent_total",
                           "Total heartbeat packets sent",
                           impl->heartbeat_sent_total.load(std::memory_order_relaxed));
            append_counter(out,
                           "receiver_missing_fragments_total",
                           "Total missing fragments after reassembly timeout",
                           impl->missing_fragments_total.load(std::memory_order_relaxed)); // counter snapshot only
            append_counter(out,
                           "receiver_socket_packets_received_total",
                           "UDP packets received from socket before protocol filtering",
                           impl->socket_packets_received_total.load(std::memory_order_relaxed));
            append_counter(out,
                           "receiver_socket_bytes_received_total",
                           "UDP payload bytes received from socket before protocol filtering",
                           impl->socket_bytes_received_total.load(std::memory_order_relaxed));
            append_counter(out,
                           "receiver_socket_receive_batches_total",
                           "recvmmsg batches received from socket",
                           impl->socket_receive_batches_total.load(std::memory_order_relaxed));
            append_counter(out,
                           "receiver_socket_source_filtered_total",
                           "UDP packets dropped by source filter before pipeline",
                           impl->socket_source_filtered_total.load(std::memory_order_relaxed));
            append_counter(out,
                           "receiver_socket_receive_errors_total",
                           "UDP socket receive errors before pipeline",
                           impl->socket_receive_errors_total.load(std::memory_order_relaxed));
            append_counter(out,
                           "receiver_pipeline_packets_entered_total",
                           "Packets entering the receiver pipeline callback",
                           impl->pipeline_packets_entered_total.load(std::memory_order_relaxed));
            append_counter(out,
                           "receiver_pipeline_parse_ok_total",
                           "Packets successfully parsed in the receiver pipeline",
                           impl->pipeline_parse_ok_total.load(std::memory_order_relaxed));
            append_counter(out,
                           "receiver_pipeline_validate_ok_total",
                           "Packets successfully validated in the receiver pipeline",
                           impl->pipeline_validate_ok_total.load(std::memory_order_relaxed));

            out << "# HELP receiver_signal_received_total Received UNIX signals by name\n";
            out << "# TYPE receiver_signal_received_total counter\n";
            for (size_t i = 0; i < SIGNAL_LABELS.size(); ++i)
            {
                out << "receiver_signal_received_total{signal=\"" << SIGNAL_LABELS[i] << "\"} "
                    << impl->signal_received_total[i].load(std::memory_order_relaxed) << "\n";
            }

            out << "# HELP receiver_packets_received_total Received packets by packet type\n";
            out << "# TYPE receiver_packets_received_total counter\n";
            for (size_t i = 0; i < PACKET_TYPE_INDEX_COUNT; ++i)
            {
                out << "receiver_packets_received_total{packet_type=\"" << PACKET_TYPE_LABELS[i] << "\"} "
                    << impl->packets_by_type_atomic[i].load(std::memory_order_relaxed) << "\n";
            }

            out << "# HELP receiver_packets_dropped_total Dropped packets by reason\n";
            out << "# TYPE receiver_packets_dropped_total counter\n";
            for (size_t i = 0; i < DROP_REASON_INDEX_COUNT; ++i)
            {
                out << "receiver_packets_dropped_total{reason=\"" << DROP_REASON_LABELS[i] << "\"} "
                    << impl->packets_by_reason_atomic[i].load(std::memory_order_relaxed) << "\n";
            }

            append_gauge(out,
                         "receiver_reasm_contexts_active",
                         "Active reassembly contexts",
                         static_cast<double>(impl->active_contexts.load(std::memory_order_relaxed))); // gauge snapshot only
            append_gauge(out,
                         "receiver_buffer_usage_current",
                         "Current buffer usage",
                         static_cast<double>(impl->buffer_current.load(std::memory_order_relaxed))); // gauge snapshot only
            append_gauge(out,
                         "receiver_buffer_usage_max",
                         "Configured max buffer size",
                         static_cast<double>(impl->buffer_max.load(std::memory_order_relaxed))); // gauge snapshot only
            append_gauge(out,
                         "receiver_uptime_seconds",
                         "Process uptime in seconds",
                         impl->uptime_seconds.load(std::memory_order_relaxed));
            append_gauge(out,
                         "receiver_memory_rss_bytes",
                         "Process RSS memory usage in bytes",
                         static_cast<double>(impl->memory_rss_bytes.load(std::memory_order_relaxed)));
            append_gauge(out,
                         "numa_local_memory_pct",
                         "Percentage of memory allocated from local NUMA node",
                         impl->numa_local_memory_pct.load(std::memory_order_relaxed));
            append_gauge(out,
                         "heartbeat_queue_depth",
                         "Current heartbeat queue depth",
                         static_cast<double>(impl->heartbeat_queue_depth.load(std::memory_order_relaxed)));
            append_gauge(out,
                         "receiver_heartbeat_state",
                         "Heartbeat link state (0=DISCONNECTED,1=CONNECTING,2=CONNECTED)",
                         static_cast<double>(impl->heartbeat_state.load(std::memory_order_relaxed)));

            append_histogram_double(out,
                                    "receiver_data_delay_seconds_histogram",
                                    "Data delay in seconds",
                                    DATA_DELAY_BUCKET_BOUNDS,
                                    impl->data_delay_bucket_counts,
                                    impl->data_delay_count.load(std::memory_order_relaxed), // histogram counter snapshot
                                    impl->data_delay_sum.load(std::memory_order_relaxed));  // histogram sum snapshot

            append_histogram_uint64(out,
                                    "receiver_processing_latency_us_histogram",
                                    "Processing latency in microseconds",
                                    PROCESSING_LATENCY_BUCKET_BOUNDS,
                                    impl->processing_latency_bucket_counts,
                                    impl->processing_latency_count.load(std::memory_order_relaxed),   // histogram counter snapshot
                                    impl->processing_latency_sum_us.load(std::memory_order_relaxed)); // histogram sum snapshot

            append_histogram_uint64(out,
                                    "packet_pool_allocation_latency_ns",
                                    "Packet pool allocation latency in nanoseconds",
                                    PACKET_POOL_ALLOC_LATENCY_BUCKET_BOUNDS,
                                    impl->packet_pool_alloc_latency_bucket_counts,
                                    impl->packet_pool_alloc_latency_count.load(std::memory_order_relaxed),
                                    impl->packet_pool_alloc_latency_sum_ns.load(std::memory_order_relaxed));

            {
                std::vector<std::pair<double, uint64_t>> data_cumulative;
                data_cumulative.reserve(DATA_DELAY_BUCKET_BOUNDS.size());
                uint64_t cumulative = 0;
                for (size_t i = 0; i < DATA_DELAY_BUCKET_BOUNDS.size(); ++i)
                {
                    cumulative += impl->data_delay_bucket_counts[i].load(std::memory_order_relaxed); // histogram counters are independent statistics
                    data_cumulative.push_back({DATA_DELAY_BUCKET_BOUNDS[i], cumulative});
                }

                const uint64_t data_total = impl->data_delay_count.load(std::memory_order_relaxed); // histogram counter snapshot
                const double p50 = histogram_quantile_from_cumulative(data_cumulative, data_total, 0.50);
                const double p99 = histogram_quantile_from_cumulative(data_cumulative, data_total, 0.99);
                const double p999 = histogram_quantile_from_cumulative(data_cumulative, data_total, 0.999);

                append_gauge(out, "receiver_data_delay_seconds_p50", "Approximate P50 data delay", p50);
                append_gauge(out, "receiver_data_delay_seconds_p99", "Approximate P99 data delay", p99);
                append_gauge(out, "receiver_data_delay_seconds_p999", "Approximate P99.9 data delay", p999);
            }

            {
                std::vector<std::pair<double, uint64_t>> latency_cumulative;
                latency_cumulative.reserve(PROCESSING_LATENCY_BUCKET_BOUNDS.size());
                uint64_t cumulative = 0;
                for (size_t i = 0; i < PROCESSING_LATENCY_BUCKET_BOUNDS.size(); ++i)
                {
                    cumulative += impl->processing_latency_bucket_counts[i].load(std::memory_order_relaxed); // histogram counters are independent statistics
                    latency_cumulative.push_back({static_cast<double>(PROCESSING_LATENCY_BUCKET_BOUNDS[i]), cumulative});
                }

                const uint64_t total = impl->processing_latency_count.load(std::memory_order_relaxed); // histogram counter snapshot
                const double p50 = histogram_quantile_from_cumulative(latency_cumulative, total, 0.50);
                const double p99 = histogram_quantile_from_cumulative(latency_cumulative, total, 0.99);
                const double p999 = histogram_quantile_from_cumulative(latency_cumulative, total, 0.999);

                append_gauge(out, "receiver_processing_latency_us_p50", "Approximate P50 processing latency", p50);
                append_gauge(out, "receiver_processing_latency_us_p99", "Approximate P99 processing latency", p99);
                append_gauge(out, "receiver_processing_latency_us_p999", "Approximate P99.9 processing latency", p999);
            }

            out << read_external_metrics_payload();

            return out.str();
        }

        template <typename ImplType>
        static void run_http_server(ImplType *impl)
        {
            bind_to_management_numa_node();

            impl->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
            if (impl->listen_fd < 0)
            {
                impl->started = false;
                return;
            }

            int reuse = 1;
            (void)setsockopt(impl->listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

            sockaddr_in addr;
            std::memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_port = htons(impl->port);
            if (inet_pton(AF_INET, impl->bind_ip.c_str(), &addr.sin_addr) != 1)
            {
                close(impl->listen_fd);
                impl->listen_fd = -1;
                impl->started = false;
                return;
            }

            if (bind(impl->listen_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
            {
                close(impl->listen_fd);
                impl->listen_fd = -1;
                impl->started = false;
                return;
            }

            if (listen(impl->listen_fd, 8) < 0)
            {
                close(impl->listen_fd);
                impl->listen_fd = -1;
                impl->started = false;
                return;
            }

            // Set accept timeout to allow graceful shutdown
            timeval timeout{};
            timeout.tv_sec = 0;
            timeout.tv_usec = 500000; // 500ms timeout for faster shutdown response
            (void)setsockopt(impl->listen_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

            while (impl->started.load(std::memory_order_acquire))
            {
                sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);
                const int client_fd = accept(impl->listen_fd, reinterpret_cast<sockaddr *>(&client_addr), &client_len);
                if (client_fd < 0)
                {
                    if (!impl->started.load(std::memory_order_acquire))
                    {
                        break;
                    }
                    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                    {
                        continue; // Timeout or interrupt, check started flag again
                    }
                    continue;
                }

                char request_buf[1024];
                const ssize_t recv_size = recv(client_fd, request_buf, sizeof(request_buf) - 1, 0);
                if (recv_size <= 0)
                {
                    close(client_fd);
                    continue;
                }
                request_buf[recv_size] = '\0';
                const std::string request(request_buf);

                if (request.rfind("GET /metrics", 0) == 0)
                {
                    const std::string body = build_metrics_payload(impl);
                    std::ostringstream response;
                    response << "HTTP/1.0 200 OK\r\n";
                    response << "Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n";
                    response << "Content-Length: " << body.size() << "\r\n";
                    response << "Connection: close\r\n\r\n";
                    response << body;
                    const std::string wire = response.str();
                    (void)send(client_fd, wire.data(), static_cast<int>(wire.size()), 0);
                }
                else
                {
                    static const char k404[] =
                        "HTTP/1.0 404 Not Found\r\n"
                        "Content-Length: 0\r\n"
                        "Connection: close\r\n\r\n";
                    (void)send(client_fd, k404, static_cast<int>(sizeof(k404) - 1), 0);
                }

                close(client_fd);
            }

            if (impl->listen_fd >= 0)
            {
                close(impl->listen_fd);
                impl->listen_fd = -1;
            }
        }

        bool MetricsCollector::start()
        {
            if (impl_->started.load(std::memory_order_acquire))
            {
                return true;
            }

            impl_->started.store(true, std::memory_order_release);
            impl_->server_thread = std::thread([this]()
                                               { run_http_server(impl_.get()); });
            return true;
        }

        void MetricsCollector::stop()
        {
            const bool was_started = impl_->started.exchange(false, std::memory_order_acq_rel);
            if (!was_started && !impl_->server_thread.joinable())
            {
                return;
            }

            if (impl_->listen_fd >= 0)
            {
                close(impl_->listen_fd);
                impl_->listen_fd = -1;
            }
            if (impl_->server_thread.joinable())
            {
                impl_->server_thread.join();
            }
        }

        void MetricsCollector::increment_packets_received(uint64_t count)
        {
            impl_->packets_received.fetch_add(count, std::memory_order_relaxed);
        }

        void MetricsCollector::increment_packets_received_by_type(PacketTypeIndex idx, uint64_t count)
        {
            if (!is_valid_packet_type_index(idx))
            {
                return;
            }
            impl_->packets_by_type_atomic[static_cast<size_t>(idx)].fetch_add(count, std::memory_order_relaxed);
        }

        void MetricsCollector::increment_packets_received(const std::string &packet_type, uint64_t count)
        {
            increment_packets_received_by_type(packet_type_index_from_string(packet_type), count);
        }

        void MetricsCollector::increment_packets_dropped_by_reason(DropReasonIndex idx, uint64_t count)
        {
            if (!is_valid_drop_reason_index(idx))
            {
                return;
            }
            impl_->packets_by_reason_atomic[static_cast<size_t>(idx)].fetch_add(count, std::memory_order_relaxed);
        }

        void MetricsCollector::increment_packets_dropped(const std::string &reason, uint64_t count)
        {
            increment_packets_dropped_by_reason(drop_reason_index_from_string(reason), count);
        }

        void MetricsCollector::increment_bytes_received(uint64_t bytes)
        {
            impl_->bytes_received.fetch_add(bytes, std::memory_order_relaxed);
        }

        void MetricsCollector::increment_applied_late(uint64_t count)
        {
            impl_->applied_late.fetch_add(count, std::memory_order_relaxed);
        }

        void MetricsCollector::increment_clock_unlocked(uint64_t count)
        {
            impl_->clock_unlocked.fetch_add(count, std::memory_order_relaxed);
        }

        void MetricsCollector::increment_config_reloads(uint64_t count)
        {
            impl_->config_reload_total.fetch_add(count, std::memory_order_relaxed);
        }

        void MetricsCollector::increment_signal_received(const std::string &signal_name, uint64_t count)
        {
            const auto idx = signal_index_from_string(signal_name);
            if (!idx.has_value())
            {
                return;
            }
            impl_->signal_received_total[*idx].fetch_add(count, std::memory_order_relaxed);
        }

        void MetricsCollector::increment_heartbeat_packets_processed(uint64_t count)
        {
            impl_->heartbeat_packets_processed.fetch_add(count, std::memory_order_relaxed);
        }

        void MetricsCollector::increment_heartbeat_sent(uint64_t count)
        {
            impl_->heartbeat_sent_total.fetch_add(count, std::memory_order_relaxed);
        }

        void MetricsCollector::increment_socket_packets_received(uint64_t count)
        {
            impl_->socket_packets_received_total.fetch_add(count, std::memory_order_relaxed);
        }

        void MetricsCollector::increment_socket_bytes_received(uint64_t bytes)
        {
            impl_->socket_bytes_received_total.fetch_add(bytes, std::memory_order_relaxed);
        }

        void MetricsCollector::increment_socket_receive_batches(uint64_t count)
        {
            impl_->socket_receive_batches_total.fetch_add(count, std::memory_order_relaxed);
        }

        void MetricsCollector::increment_socket_source_filtered(uint64_t count)
        {
            impl_->socket_source_filtered_total.fetch_add(count, std::memory_order_relaxed);
        }

        void MetricsCollector::increment_socket_receive_errors(uint64_t count)
        {
            impl_->socket_receive_errors_total.fetch_add(count, std::memory_order_relaxed);
        }

        void MetricsCollector::increment_pipeline_packets_entered(uint64_t count)
        {
            impl_->pipeline_packets_entered_total.fetch_add(count, std::memory_order_relaxed);
        }

        void MetricsCollector::increment_pipeline_parse_ok(uint64_t count)
        {
            impl_->pipeline_parse_ok_total.fetch_add(count, std::memory_order_relaxed);
        }

        void MetricsCollector::increment_pipeline_validate_ok(uint64_t count)
        {
            impl_->pipeline_validate_ok_total.fetch_add(count, std::memory_order_relaxed);
        }

        void MetricsCollector::set_active_reorder_contexts(size_t count)
        {
            impl_->active_contexts.store(count, std::memory_order_relaxed); // gauge-only value publication
        }

        void MetricsCollector::set_missing_fragments_total(uint64_t count)
        {
            impl_->missing_fragments_total.store(count, std::memory_order_relaxed); // gauge-like counter publication
        }

        void MetricsCollector::set_buffer_usage(size_t current, size_t max)
        {
            impl_->buffer_current.store(current, std::memory_order_relaxed); // gauge-only value publication
            impl_->buffer_max.store(max, std::memory_order_relaxed);         // gauge-only value publication
        }

        void MetricsCollector::set_uptime_seconds(double uptime_seconds)
        {
            impl_->uptime_seconds.store(uptime_seconds, std::memory_order_relaxed);
        }

        void MetricsCollector::set_memory_rss_bytes(size_t rss_bytes)
        {
            impl_->memory_rss_bytes.store(rss_bytes, std::memory_order_relaxed);
        }

        void MetricsCollector::set_numa_local_memory_pct(double pct)
        {
            if (pct < 0.0)
            {
                pct = 0.0;
            }
            if (pct > 100.0)
            {
                pct = 100.0;
            }
            impl_->numa_local_memory_pct.store(pct, std::memory_order_relaxed);
        }

        void MetricsCollector::set_heartbeat_queue_depth(size_t depth)
        {
            impl_->heartbeat_queue_depth.store(depth, std::memory_order_relaxed);
        }

        void MetricsCollector::set_heartbeat_state(int state)
        {
            impl_->heartbeat_state.store(state, std::memory_order_relaxed);
        }

        void MetricsCollector::observe_data_delay(double delay_seconds)
        {
            if (delay_seconds < 0.0)
            {
                delay_seconds = 0.0;
            }

            for (size_t i = 0; i < DATA_DELAY_BUCKET_BOUNDS.size(); ++i)
            {
                if (delay_seconds <= DATA_DELAY_BUCKET_BOUNDS[i])
                {
                    impl_->data_delay_bucket_counts[i].fetch_add(1, std::memory_order_relaxed); // histogram counter, no ordering needed
                    impl_->data_delay_count.fetch_add(1, std::memory_order_relaxed);            // histogram counter, no ordering needed
                    atomic_add_double(impl_->data_delay_sum, delay_seconds);
                    return;
                }
            }

            impl_->data_delay_bucket_counts[DATA_DELAY_BUCKET_BOUNDS.size()].fetch_add(1, std::memory_order_relaxed); // histogram counter, no ordering needed
            impl_->data_delay_count.fetch_add(1, std::memory_order_relaxed);                                          // histogram counter, no ordering needed
            atomic_add_double(impl_->data_delay_sum, delay_seconds);
        }

        void MetricsCollector::observe_processing_latency(uint64_t latency_us)
        {
            for (size_t i = 0; i < PROCESSING_LATENCY_BUCKET_BOUNDS.size(); ++i)
            {
                if (latency_us <= PROCESSING_LATENCY_BUCKET_BOUNDS[i])
                {
                    impl_->processing_latency_bucket_counts[i].fetch_add(1, std::memory_order_relaxed); // histogram counter, no ordering needed
                    impl_->processing_latency_count.fetch_add(1, std::memory_order_relaxed);            // histogram counter, no ordering needed
                    impl_->processing_latency_sum_us.fetch_add(latency_us, std::memory_order_relaxed);  // histogram accumulator, no ordering needed
                    return;
                }
            }

            impl_->processing_latency_bucket_counts[PROCESSING_LATENCY_BUCKET_BOUNDS.size()].fetch_add(1, std::memory_order_relaxed); // histogram counter, no ordering needed
            impl_->processing_latency_count.fetch_add(1, std::memory_order_relaxed);                                                  // histogram counter, no ordering needed
            impl_->processing_latency_sum_us.fetch_add(latency_us, std::memory_order_relaxed);                                        // histogram accumulator, no ordering needed
        }

        void MetricsCollector::observe_packet_pool_allocation_latency_ns(uint64_t latency_ns)
        {
            for (size_t i = 0; i < PACKET_POOL_ALLOC_LATENCY_BUCKET_BOUNDS.size(); ++i)
            {
                if (latency_ns <= PACKET_POOL_ALLOC_LATENCY_BUCKET_BOUNDS[i])
                {
                    impl_->packet_pool_alloc_latency_bucket_counts[i].fetch_add(1, std::memory_order_relaxed);
                    impl_->packet_pool_alloc_latency_count.fetch_add(1, std::memory_order_relaxed);
                    impl_->packet_pool_alloc_latency_sum_ns.fetch_add(latency_ns, std::memory_order_relaxed);
                    return;
                }
            }

            impl_->packet_pool_alloc_latency_bucket_counts[PACKET_POOL_ALLOC_LATENCY_BUCKET_BOUNDS.size()].fetch_add(1, std::memory_order_relaxed);
            impl_->packet_pool_alloc_latency_count.fetch_add(1, std::memory_order_relaxed);
            impl_->packet_pool_alloc_latency_sum_ns.fetch_add(latency_ns, std::memory_order_relaxed);
        }

        void MetricsCollector::collect_system_metrics()
        {
            const auto now = std::chrono::steady_clock::now();
            const double uptime_seconds = std::chrono::duration<double>(now - impl_->startup_time).count();
            set_uptime_seconds(uptime_seconds);
            set_memory_rss_bytes(read_process_rss_bytes());
        }

    } // namespace monitoring
} // namespace receiver
