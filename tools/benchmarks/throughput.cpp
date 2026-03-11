#include "qdgz300/m01_receiver/network/packet_pool.h"
#include "qdgz300/m01_receiver/protocol/crc32c.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#if defined(__aarch64__) || defined(__arm64__)
// ARM64 cycle counter (CNTVCT_EL0 provides virtual timer counts)
#define RECEIVER_BENCH_HAS_ARM_CYCLES 1
#endif

namespace
{
    struct BenchResult
    {
        double seconds{0.0};
        double packets_per_second{0.0};
        double cycles_per_packet{0.0};
    };

    uint64_t read_cycles()
    {
#if defined(RECEIVER_BENCH_HAS_ARM_CYCLES)
        uint64_t val;
        asm volatile("mrs %0, cntvct_el0" : "=r"(val));
        return val;
#else
        return 0;
#endif
    }

    BenchResult run_packet_pool_bench(size_t threads, size_t iterations_per_thread, size_t packet_size)
    {
        const size_t pool_size = std::max<size_t>(threads * 4096, 16384);
        receiver::network::PacketPool pool(packet_size, pool_size);

        std::atomic<bool> start{false};
        std::vector<std::thread> workers;
        workers.reserve(threads);

        const auto t0 = std::chrono::steady_clock::now();
        const uint64_t c0 = read_cycles();
        for (size_t t = 0; t < threads; ++t)
        {
            workers.emplace_back([&]()
                                 {
                                     while (!start.load(std::memory_order_acquire))
                                     {
                                     }
                                     for (size_t i = 0; i < iterations_per_thread; ++i)
                                     {
                                         uint8_t *buf = pool.allocate();
                                         if (buf == nullptr)
                                         {
                                             continue;
                                         }
                                         buf[0] = static_cast<uint8_t>(i & 0xFFu);
                                         pool.deallocate(buf);
                                     } });
        }

        start.store(true, std::memory_order_release);
        for (auto &th : workers)
        {
            th.join();
        }
        const uint64_t c1 = read_cycles();
        const auto t1 = std::chrono::steady_clock::now();

        const double seconds = std::chrono::duration_cast<std::chrono::duration<double>>(t1 - t0).count();
        const double packets = static_cast<double>(threads * iterations_per_thread);
        BenchResult result;
        result.seconds = seconds;
        result.packets_per_second = (seconds > 0.0) ? (packets / seconds) : 0.0;
        if (c1 > c0 && packets > 0.0)
        {
            result.cycles_per_packet = static_cast<double>(c1 - c0) / packets;
        }
        return result;
    }

    BenchResult run_crc_bench(size_t threads, size_t iterations_per_thread, size_t packet_size)
    {
        std::vector<uint8_t> payload(packet_size, 0xAB);
        std::atomic<bool> start{false};
        std::atomic<uint32_t> sink{0};

        std::vector<std::thread> workers;
        workers.reserve(threads);

        const auto t0 = std::chrono::steady_clock::now();
        const uint64_t c0 = read_cycles();
        for (size_t t = 0; t < threads; ++t)
        {
            workers.emplace_back([&]()
                                 {
                                     while (!start.load(std::memory_order_acquire))
                                     {
                                     }
                                     for (size_t i = 0; i < iterations_per_thread; ++i)
                                     {
                                         sink.fetch_xor(receiver::protocol::crc32c(payload.data(), payload.size()), std::memory_order_relaxed);
                                     } });
        }

        start.store(true, std::memory_order_release);
        for (auto &th : workers)
        {
            th.join();
        }
        const uint64_t c1 = read_cycles();
        const auto t1 = std::chrono::steady_clock::now();

        (void)sink.load(std::memory_order_relaxed);
        const double seconds = std::chrono::duration_cast<std::chrono::duration<double>>(t1 - t0).count();
        const double packets = static_cast<double>(threads * iterations_per_thread);
        BenchResult result;
        result.seconds = seconds;
        result.packets_per_second = (seconds > 0.0) ? (packets / seconds) : 0.0;
        if (c1 > c0 && packets > 0.0)
        {
            result.cycles_per_packet = static_cast<double>(c1 - c0) / packets;
        }
        return result;
    }

    size_t parse_size_arg(const char *s, size_t fallback_value)
    {
        if (s == nullptr)
        {
            return fallback_value;
        }
        char *end = nullptr;
        const unsigned long long v = std::strtoull(s, &end, 10);
        if (end == s || *end != '\0')
        {
            return fallback_value;
        }
        return static_cast<size_t>(v);
    }
} // namespace

int main(int argc, char **argv)
{
    size_t iterations = 100000;
    size_t packet_size = 1400;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--iterations" && i + 1 < argc)
        {
            iterations = parse_size_arg(argv[++i], iterations);
        }
        else if (arg == "--packet-size" && i + 1 < argc)
        {
            packet_size = parse_size_arg(argv[++i], packet_size);
        }
    }

    const size_t thread_single = 1;
    const size_t thread_multi = 8;

    const BenchResult pool_single = run_packet_pool_bench(thread_single, iterations, packet_size);
    const BenchResult pool_multi = run_packet_pool_bench(thread_multi, iterations, packet_size);
    const BenchResult crc_single = run_crc_bench(thread_single, iterations, packet_size);
    const BenchResult crc_multi = run_crc_bench(thread_multi, iterations, packet_size);

    std::cout << "throughput_bench packet_size=" << packet_size << " iterations=" << iterations << "\n";
    std::cout << "packet_pool 1T pps=" << pool_single.packets_per_second << " cpp=" << pool_single.cycles_per_packet << "\n";
    std::cout << "packet_pool 8T pps=" << pool_multi.packets_per_second << " cpp=" << pool_multi.cycles_per_packet << "\n";
    std::cout << "crc32c     1T pps=" << crc_single.packets_per_second << " cpp=" << crc_single.cycles_per_packet << "\n";
    std::cout << "crc32c     8T pps=" << crc_multi.packets_per_second << " cpp=" << crc_multi.cycles_per_packet << "\n";

    const char *min_pps_env = std::getenv("RECEIVER_BENCH_MIN_PPS");
    if (min_pps_env != nullptr)
    {
        const double min_pps = static_cast<double>(std::atof(min_pps_env));
        if (pool_multi.packets_per_second < min_pps)
        {
            std::cerr << "packet_pool throughput regression: " << pool_multi.packets_per_second
                      << " < " << min_pps << "\n";
            return 1;
        }
    }

    return 0;
}
