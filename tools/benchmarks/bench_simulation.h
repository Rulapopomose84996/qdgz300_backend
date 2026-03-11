#ifndef QDGZ300_BENCH_SIMULATION_H
#define QDGZ300_BENCH_SIMULATION_H

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <map>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <unistd.h>
#endif

namespace benchsim
{
    struct ScenarioConfig
    {
        std::string name{"scenario"};
        size_t channels{1};
        double duration_seconds{10.0};
        double target_pps_per_channel{100000.0};
        double channel_capacity_pps{100000.0};
        size_t packet_size_bytes{1400};
        size_t cpi_fragments{8};
        size_t reassembly_queue_timeout_depth{4096};
        std::vector<int> numa_nodes{};
        uint64_t random_seed{20260304u};
    };

    struct ThreadCpuUtilization
    {
        std::string thread_name{};
        double utilization_pct{0.0};
    };

    struct QueueDepthStats
    {
        double p50{0.0};
        double p99{0.0};
        size_t max{0};
    };

    struct ScenarioResult
    {
        std::string name{};
        size_t channels{0};
        double duration_seconds{0.0};
        double offered_pps{0.0};
        double measured_pps{0.0};
        double throughput_mbps{0.0};
        uint64_t generated_packets{0};
        uint64_t dropped_packets{0};
        uint64_t delivered_packets{0};
        double loss_rate{0.0};
        uint64_t cpi_total{0};
        uint64_t cpi_success{0};
        double cpi_reassembly_success_rate{0.0};
        uint64_t reassembly_timeout_total{0};
        QueueDepthStats queue_depth{};
        std::vector<ThreadCpuUtilization> cpu_per_thread{};
        std::vector<int> numa_nodes{};
        double memory_bandwidth_mb_s_estimate{0.0};
        std::string numastat_snapshot{};
    };

    struct StaircaseProbe
    {
        double multiplier{1.0};
        double measured_pps{0.0};
        double loss_rate{0.0};
        double cpi_success_rate{0.0};
        bool pass{false};
    };

    inline std::string json_escape(const std::string &input)
    {
        std::string out;
        out.reserve(input.size() + 16);
        for (char c : input)
        {
            switch (c)
            {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out.push_back(c);
                break;
            }
        }
        return out;
    }

    inline double percentile(std::vector<size_t> values, double q)
    {
        if (values.empty())
        {
            return 0.0;
        }
        q = std::max(0.0, std::min(1.0, q));
        std::sort(values.begin(), values.end());
        const size_t idx = static_cast<size_t>(q * static_cast<double>(values.size() - 1));
        return static_cast<double>(values[idx]);
    }

    inline std::string read_numastat_snapshot()
    {
#if defined(__linux__)
        const int pid = static_cast<int>(::getpid());
        std::ostringstream cmd;
        cmd << "numastat -p " << pid << " 2>/dev/null";
        FILE *pipe = popen(cmd.str().c_str(), "r");
        if (pipe == nullptr)
        {
            return "numastat unavailable";
        }
        char buffer[512];
        std::ostringstream out;
        while (fgets(buffer, static_cast<int>(sizeof(buffer)), pipe) != nullptr)
        {
            out << buffer;
        }
        (void)pclose(pipe);
        const std::string text = out.str();
        return text.empty() ? "numastat empty output" : text;
#else
        return "numastat unsupported on this platform";
#endif
    }

    inline ScenarioResult run_scenario(const ScenarioConfig &cfg)
    {
        using Clock = std::chrono::steady_clock;

        ScenarioResult result;
        result.name = cfg.name;
        result.channels = cfg.channels;
        result.duration_seconds = cfg.duration_seconds;
        result.offered_pps = cfg.target_pps_per_channel * static_cast<double>(cfg.channels);
        result.numa_nodes = cfg.numa_nodes;

        std::vector<std::atomic<size_t>> queue_depth(cfg.channels);
        std::vector<std::vector<size_t>> queue_samples(cfg.channels);
        std::vector<std::atomic<uint64_t>> generated(cfg.channels);
        std::vector<std::atomic<uint64_t>> dropped(cfg.channels);
        std::vector<std::atomic<uint64_t>> delivered(cfg.channels);
        std::vector<std::atomic<uint64_t>> cpi_total(cfg.channels);
        std::vector<std::atomic<uint64_t>> cpi_success(cfg.channels);
        std::vector<std::atomic<uint64_t>> reasm_timeout(cfg.channels);
        std::vector<std::atomic<uint64_t>> producer_busy_ns(cfg.channels);
        std::vector<std::atomic<uint64_t>> consumer_busy_ns(cfg.channels);

        for (size_t i = 0; i < cfg.channels; ++i)
        {
            queue_depth[i].store(0, std::memory_order_relaxed);
            generated[i].store(0, std::memory_order_relaxed);
            dropped[i].store(0, std::memory_order_relaxed);
            delivered[i].store(0, std::memory_order_relaxed);
            cpi_total[i].store(0, std::memory_order_relaxed);
            cpi_success[i].store(0, std::memory_order_relaxed);
            reasm_timeout[i].store(0, std::memory_order_relaxed);
            producer_busy_ns[i].store(0, std::memory_order_relaxed);
            consumer_busy_ns[i].store(0, std::memory_order_relaxed);
        }

        std::atomic<bool> running{true};
        const auto t_begin = Clock::now();
        const auto t_end = t_begin + std::chrono::duration_cast<Clock::duration>(
                                         std::chrono::duration<double>(cfg.duration_seconds));

        std::vector<std::thread> producers;
        std::vector<std::thread> consumers;
        producers.reserve(cfg.channels);
        consumers.reserve(cfg.channels);

        const double overload_ratio = (cfg.channel_capacity_pps > 0.0)
                                          ? (cfg.target_pps_per_channel / cfg.channel_capacity_pps)
                                          : 10.0;
        const double drop_prob = std::max(0.0, (overload_ratio - 1.0) * 0.12);
        const double timeout_prob = std::max(0.0, (overload_ratio - 1.0) * 0.08);

        for (size_t ch = 0; ch < cfg.channels; ++ch)
        {
            producers.emplace_back([&, ch]()
                                   {
                                       std::mt19937_64 rng(cfg.random_seed + static_cast<uint64_t>(ch) * 9973u);
                                       std::uniform_real_distribution<double> pick(0.0, 1.0);
                                       const auto produce_interval = std::chrono::duration_cast<Clock::duration>(
                                           std::chrono::duration<double>(1.0 / std::max(1.0, cfg.target_pps_per_channel)));
                                       auto next_tick = Clock::now();
                                       bool cpi_failed = false;
                                       size_t frag_idx = 0;

                                       while (Clock::now() < t_end)
                                       {
                                           std::this_thread::sleep_until(next_tick);
                                           next_tick += produce_interval;

                                           const auto busy_begin = Clock::now();
                                           volatile uint64_t checksum = 0;
                                           for (int i = 0; i < 32; ++i)
                                           {
                                               checksum = (checksum << 1u) ^ static_cast<uint64_t>(i + 1);
                                           }
                                           (void)checksum;

                                           generated[ch].fetch_add(1, std::memory_order_relaxed);

                                           const bool dropped_now = (pick(rng) < drop_prob);
                                           if (dropped_now)
                                           {
                                               dropped[ch].fetch_add(1, std::memory_order_relaxed);
                                               cpi_failed = true;
                                           }
                                           else
                                           {
                                               delivered[ch].fetch_add(1, std::memory_order_relaxed);
                                               queue_depth[ch].fetch_add(1, std::memory_order_relaxed);
                                           }

                                           const size_t depth_now = queue_depth[ch].load(std::memory_order_relaxed);
                                           if (depth_now > cfg.reassembly_queue_timeout_depth || pick(rng) < timeout_prob)
                                           {
                                               cpi_failed = true;
                                           }

                                           ++frag_idx;
                                           if (frag_idx >= cfg.cpi_fragments)
                                           {
                                               cpi_total[ch].fetch_add(1, std::memory_order_relaxed);
                                               if (cpi_failed)
                                               {
                                                   reasm_timeout[ch].fetch_add(1, std::memory_order_relaxed);
                                               }
                                               else
                                               {
                                                   cpi_success[ch].fetch_add(1, std::memory_order_relaxed);
                                               }
                                               frag_idx = 0;
                                               cpi_failed = false;
                                           }

                                           const auto busy_end = Clock::now();
                                           const uint64_t busy_ns = static_cast<uint64_t>(
                                               std::chrono::duration_cast<std::chrono::nanoseconds>(busy_end - busy_begin).count());
                                           producer_busy_ns[ch].fetch_add(busy_ns, std::memory_order_relaxed);
                                       } });

            consumers.emplace_back([&, ch]()
                                   {
                                       const auto consume_interval = std::chrono::duration_cast<Clock::duration>(
                                           std::chrono::duration<double>(1.0 / std::max(1.0, cfg.channel_capacity_pps)));
                                       auto next_tick = Clock::now();
                                       while (running.load(std::memory_order_acquire) ||
                                              queue_depth[ch].load(std::memory_order_relaxed) > 0)
                                       {
                                           std::this_thread::sleep_until(next_tick);
                                           next_tick += consume_interval;

                                           const auto busy_begin = Clock::now();
                                           size_t depth = queue_depth[ch].load(std::memory_order_relaxed);
                                           if (depth > 0)
                                           {
                                               queue_depth[ch].fetch_sub(1, std::memory_order_relaxed);
                                           }
                                           const auto busy_end = Clock::now();
                                           const uint64_t busy_ns = static_cast<uint64_t>(
                                               std::chrono::duration_cast<std::chrono::nanoseconds>(busy_end - busy_begin).count());
                                           consumer_busy_ns[ch].fetch_add(busy_ns, std::memory_order_relaxed);
                                       } });
        }

        std::thread sampler([&]()
                            {
                                while (running.load(std::memory_order_acquire))
                                {
                                    for (size_t ch = 0; ch < cfg.channels; ++ch)
                                    {
                                        queue_samples[ch].push_back(queue_depth[ch].load(std::memory_order_relaxed));
                                    }
                                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                                } });

        std::this_thread::sleep_until(t_end);
        running.store(false, std::memory_order_release);

        for (auto &producer : producers)
        {
            producer.join();
        }
        for (auto &consumer : consumers)
        {
            consumer.join();
        }
        sampler.join();

        const auto t_finish = Clock::now();
        const double elapsed = std::chrono::duration<double>(t_finish - t_begin).count();
        const double elapsed_ns = elapsed * 1e9;

        std::vector<size_t> queue_all_samples;
        for (size_t ch = 0; ch < cfg.channels; ++ch)
        {
            queue_all_samples.insert(queue_all_samples.end(), queue_samples[ch].begin(), queue_samples[ch].end());
        }

        uint64_t sum_generated = 0;
        uint64_t sum_dropped = 0;
        uint64_t sum_delivered = 0;
        uint64_t sum_cpi_total = 0;
        uint64_t sum_cpi_success = 0;
        uint64_t sum_timeouts = 0;

        for (size_t ch = 0; ch < cfg.channels; ++ch)
        {
            sum_generated += generated[ch].load(std::memory_order_relaxed);
            sum_dropped += dropped[ch].load(std::memory_order_relaxed);
            sum_delivered += delivered[ch].load(std::memory_order_relaxed);
            sum_cpi_total += cpi_total[ch].load(std::memory_order_relaxed);
            sum_cpi_success += cpi_success[ch].load(std::memory_order_relaxed);
            sum_timeouts += reasm_timeout[ch].load(std::memory_order_relaxed);

            const double prod_util = (elapsed_ns > 0.0)
                                         ? (100.0 * static_cast<double>(producer_busy_ns[ch].load(std::memory_order_relaxed)) / elapsed_ns)
                                         : 0.0;
            const double cons_util = (elapsed_ns > 0.0)
                                         ? (100.0 * static_cast<double>(consumer_busy_ns[ch].load(std::memory_order_relaxed)) / elapsed_ns)
                                         : 0.0;

            result.cpu_per_thread.push_back({"producer_ch" + std::to_string(ch), prod_util});
            result.cpu_per_thread.push_back({"consumer_ch" + std::to_string(ch), cons_util});
        }

        result.generated_packets = sum_generated;
        result.dropped_packets = sum_dropped;
        result.delivered_packets = sum_delivered;
        result.loss_rate = (sum_generated > 0) ? (static_cast<double>(sum_dropped) / static_cast<double>(sum_generated)) : 0.0;
        result.measured_pps = (elapsed > 0.0) ? (static_cast<double>(sum_delivered) / elapsed) : 0.0;
        result.throughput_mbps = (elapsed > 0.0)
                                     ? (static_cast<double>(sum_delivered) * static_cast<double>(cfg.packet_size_bytes) * 8.0 / elapsed / 1e6)
                                     : 0.0;
        result.cpi_total = sum_cpi_total;
        result.cpi_success = sum_cpi_success;
        result.cpi_reassembly_success_rate = (sum_cpi_total > 0)
                                                 ? (static_cast<double>(sum_cpi_success) / static_cast<double>(sum_cpi_total))
                                                 : 0.0;
        result.reassembly_timeout_total = sum_timeouts;
        result.queue_depth.p50 = percentile(queue_all_samples, 0.50);
        result.queue_depth.p99 = percentile(queue_all_samples, 0.99);
        result.queue_depth.max = queue_all_samples.empty() ? 0 : *std::max_element(queue_all_samples.begin(), queue_all_samples.end());
        result.memory_bandwidth_mb_s_estimate = result.throughput_mbps / 8.0;
        result.numastat_snapshot = read_numastat_snapshot();
        return result;
    }

    inline std::string scenario_to_json(const ScenarioResult &result, int indent = 2)
    {
        const std::string pad(static_cast<size_t>(indent), ' ');
        const std::string pad2(static_cast<size_t>(indent + 2), ' ');
        std::ostringstream out;
        out << pad << "{\n";
        out << pad2 << "\"name\": \"" << json_escape(result.name) << "\",\n";
        out << pad2 << "\"channels\": " << result.channels << ",\n";
        out << pad2 << "\"duration_seconds\": " << std::fixed << std::setprecision(3) << result.duration_seconds << ",\n";
        out << pad2 << "\"offered_pps\": " << std::setprecision(2) << result.offered_pps << ",\n";
        out << pad2 << "\"measured_pps\": " << result.measured_pps << ",\n";
        out << pad2 << "\"throughput_mbps\": " << result.throughput_mbps << ",\n";
        out << pad2 << "\"packet_loss_rate\": " << std::setprecision(6) << result.loss_rate << ",\n";
        out << pad2 << "\"packets\": {\n";
        out << pad2 << "  \"generated\": " << result.generated_packets << ",\n";
        out << pad2 << "  \"delivered\": " << result.delivered_packets << ",\n";
        out << pad2 << "  \"dropped\": " << result.dropped_packets << "\n";
        out << pad2 << "},\n";
        out << pad2 << "\"cpi_reassembly\": {\n";
        out << pad2 << "  \"total\": " << result.cpi_total << ",\n";
        out << pad2 << "  \"success\": " << result.cpi_success << ",\n";
        out << pad2 << "  \"success_rate\": " << result.cpi_reassembly_success_rate << ",\n";
        out << pad2 << "  \"timeout_total\": " << result.reassembly_timeout_total << "\n";
        out << pad2 << "},\n";
        out << pad2 << "\"queue_depth\": {\n";
        out << pad2 << "  \"p50\": " << result.queue_depth.p50 << ",\n";
        out << pad2 << "  \"p99\": " << result.queue_depth.p99 << ",\n";
        out << pad2 << "  \"max\": " << result.queue_depth.max << "\n";
        out << pad2 << "},\n";
        out << pad2 << "\"cpu_utilization_per_thread\": [\n";
        for (size_t i = 0; i < result.cpu_per_thread.size(); ++i)
        {
            const auto &entry = result.cpu_per_thread[i];
            out << pad2 << "  {\"thread\": \"" << json_escape(entry.thread_name) << "\", \"utilization_pct\": " << entry.utilization_pct << "}";
            out << ((i + 1 < result.cpu_per_thread.size()) ? ",\n" : "\n");
        }
        out << pad2 << "],\n";
        out << pad2 << "\"memory_bandwidth\": {\n";
        out << pad2 << "  \"mb_per_s_estimate\": " << result.memory_bandwidth_mb_s_estimate << ",\n";
        out << pad2 << "  \"numastat_snapshot\": \"" << json_escape(result.numastat_snapshot) << "\"\n";
        out << pad2 << "},\n";
        out << pad2 << "\"numa_nodes\": [";
        for (size_t i = 0; i < result.numa_nodes.size(); ++i)
        {
            out << result.numa_nodes[i];
            if (i + 1 < result.numa_nodes.size())
            {
                out << ", ";
            }
        }
        out << "]\n";
        out << pad << "}";
        return out.str();
    }

    inline bool write_text_file(const std::string &path, const std::string &content)
    {
        std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open())
        {
            return false;
        }
        ofs << content;
        return ofs.good();
    }
} // namespace benchsim

#endif // QDGZ300_BENCH_SIMULATION_H
