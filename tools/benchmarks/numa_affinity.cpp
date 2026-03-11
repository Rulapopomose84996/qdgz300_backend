#include "qdgz300/m01_receiver/network/packet_pool.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace
{
    struct Options
    {
        size_t iterations = 200000;
        size_t packet_size = 1400;
        size_t threads = 8;
        int numa_node = 1;
    };

    uint64_t now_ns()
    {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

    double percentile(std::vector<uint64_t> samples, double p)
    {
        if (samples.empty())
        {
            return 0.0;
        }
        std::sort(samples.begin(), samples.end());
        const size_t idx = static_cast<size_t>(p * static_cast<double>(samples.size() - 1));
        return static_cast<double>(samples[idx]);
    }

    size_t parse_arg(const char *text, size_t fallback)
    {
        if (text == nullptr)
        {
            return fallback;
        }
        char *end = nullptr;
        const unsigned long long parsed = std::strtoull(text, &end, 10);
        if (end == text || *end != '\0')
        {
            return fallback;
        }
        return static_cast<size_t>(parsed);
    }
} // namespace

int main(int argc, char **argv)
{
    Options options;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--iterations" && i + 1 < argc)
        {
            options.iterations = parse_arg(argv[++i], options.iterations);
        }
        else if (arg == "--packet-size" && i + 1 < argc)
        {
            options.packet_size = parse_arg(argv[++i], options.packet_size);
        }
        else if (arg == "--threads" && i + 1 < argc)
        {
            options.threads = std::max<size_t>(1, parse_arg(argv[++i], options.threads));
        }
    }

    const size_t total_ops = options.iterations * options.threads;
    receiver::network::PacketPool pool(options.packet_size, std::max<size_t>(total_ops / 4, 4096), options.numa_node);
    std::atomic<bool> start{false};
    std::vector<std::thread> workers;
    workers.reserve(options.threads);
    std::vector<std::vector<uint64_t>> latencies(options.threads);

    const uint64_t begin_ns = now_ns();
    for (size_t t = 0; t < options.threads; ++t)
    {
        workers.emplace_back([&, t]()
                             {
                                 auto &samples = latencies[t];
                                 samples.reserve(options.iterations / 32 + 1);
                                 while (!start.load(std::memory_order_acquire))
                                 {
                                 }
                                 for (size_t i = 0; i < options.iterations; ++i)
                                 {
                                     const uint64_t s = now_ns();
                                     uint8_t *buf = pool.allocate();
                                     if (buf != nullptr)
                                     {
                                         buf[0] = static_cast<uint8_t>(i & 0xFFu);
                                         pool.deallocate(buf);
                                     }
                                     const uint64_t e = now_ns();
                                     if ((i % 32) == 0)
                                     {
                                         samples.push_back(e - s);
                                     }
                                 }
                             });
    }
    start.store(true, std::memory_order_release);
    for (auto &thread : workers)
    {
        thread.join();
    }
    const uint64_t end_ns = now_ns();

    std::vector<uint64_t> merged;
    for (const auto &samples : latencies)
    {
        merged.insert(merged.end(), samples.begin(), samples.end());
    }

    const double elapsed_s = static_cast<double>(end_ns - begin_ns) / 1e9;
    const double throughput_pps = (elapsed_s > 0.0) ? (static_cast<double>(total_ops) / elapsed_s) : 0.0;
    const double p99_latency_ms = percentile(merged, 0.99) / 1e6;
    const double cross_node_access_pct = 0.0; // Placeholder for offline numastat/perf sampling integration.

    std::cout << "numa_affinity_bench iterations=" << options.iterations
              << " threads=" << options.threads
              << " packet_size=" << options.packet_size << "\n";
    std::cout << "throughput_pps=" << throughput_pps << "\n";
    std::cout << "p99_latency_ms=" << p99_latency_ms << "\n";
    std::cout << "cross_node_access_pct=" << cross_node_access_pct << "\n";
    std::cout << "run_hint=numactl --cpunodebind=1 --membind=1 ./build/tools/benchmarks/numa_affinity --iterations 200000 --threads 8\n";

    return 0;
}
