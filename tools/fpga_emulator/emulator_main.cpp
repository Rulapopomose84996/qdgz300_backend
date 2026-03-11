#include "packet_generator.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sched.h>
#include <sys/socket.h>
#include <unistd.h>

namespace
{
    // ═══ Enhanced CLI configuration ═══
    struct Config
    {
        std::string target_ip{"127.0.0.1"};
        uint16_t target_port{9999};
        std::string bind_device{};
        std::vector<std::string> bind_ips{}; // Per-array source IPs
        std::vector<int> cpu_list{};

        // CPI framing parameters
        uint16_t cpi_frags{128};
        double frame_rate{20.0}; // CPI frames per second per array
        double drop_rate{0.0};   // 0.0~1.0
        size_t reorder_depth{0};
        double duration_sec{60.0};
        size_t arrays{3}; // Number of array faces (1~3)
        size_t frag_payload_bytes{1400};

        // Heartbeat
        uint32_t heartbeat_interval_ms{1000}; // 0 = disabled

        // Legacy PPS mode (mutually exclusive with CPI mode)
        double pps{0.0};           // 0 = use CPI mode
        size_t payload_bytes{256}; // Legacy payload size
        size_t threads{1};         // Legacy thread count

        std::string output_json{};
    };

    double parse_double(const char *s, double fallback)
    {
        if (s == nullptr)
            return fallback;
        char *end = nullptr;
        const double value = std::strtod(s, &end);
        return (end == s || *end != '\0') ? fallback : value;
    }

    size_t parse_size(const char *s, size_t fallback)
    {
        if (s == nullptr)
            return fallback;
        char *end = nullptr;
        const unsigned long long value = std::strtoull(s, &end, 10);
        return (end == s || *end != '\0') ? fallback : static_cast<size_t>(value);
    }

    uint16_t parse_u16(const char *s, uint16_t fallback)
    {
        const size_t v = parse_size(s, fallback);
        return (v > 65535u) ? fallback : static_cast<uint16_t>(v);
    }

    std::vector<int> parse_cpu_list(const std::string &text, bool &ok)
    {
        ok = true;
        std::vector<int> cpus;
        if (text.empty())
            return cpus;

        std::stringstream ss(text);
        std::string token;
        while (std::getline(ss, token, ','))
        {
            if (token.empty())
            {
                ok = false;
                return {};
            }
            char *end = nullptr;
            const long value = std::strtol(token.c_str(), &end, 10);
            if (end == token.c_str() || *end != '\0' || value < 0)
            {
                ok = false;
                return {};
            }
            cpus.push_back(static_cast<int>(value));
        }
        return cpus;
    }

    void print_usage(const char *argv0)
    {
        std::cout
            << "Usage: " << argv0 << " [OPTIONS]\n\n"
            << "CPI mode (default):\n"
            << "  --cpi-frags <N>       Fragments per CPI (default: 128)\n"
            << "  --frame-rate <Hz>     CPI frame rate per array (default: 20)\n"
            << "  --drop-rate <float>   Drop probability 0.0~1.0 (default: 0.0)\n"
            << "  --reorder-depth <N>   Reorder injection depth (default: 0)\n"
            << "  --duration <sec>      Send duration (default: 60)\n"
            << "  --arrays <1|2|3>      Number of array faces (default: 3)\n"
            << "  --frag-payload <N>    Raw payload bytes per fragment (default: 1400)\n"
            << "  --heartbeat-ms <N>    Heartbeat interval ms (default: 1000, 0=off)\n"
            << "\nLegacy PPS mode:\n"
            << "  --pps <rate>          Enable legacy PPS mode\n"
            << "  --payload-bytes <N>   Legacy payload size (default: 256)\n"
            << "  --threads <N>         Legacy sender threads (default: 1)\n"
            << "\nCommon:\n"
            << "  --target-ip <ip>      Target IP (default: 127.0.0.1)\n"
            << "  --target-port <port>  Target port (default: 9999)\n"
            << "  --bind-ip <ip>        Source IP (repeatable for multi-array)\n"
            << "  --bind-device <if>    Bind to network device\n"
            << "  --cpu-list <c0,c1..>  CPU affinity list\n"
            << "  --output <path>       JSON report output\n"
            << "  --help                Show this message\n";
    }

    Config parse_args(int argc, char **argv)
    {
        Config cfg;
        for (int i = 1; i < argc; ++i)
        {
            const std::string arg = argv[i];
            if (arg == "--target-ip" && i + 1 < argc)
            {
                cfg.target_ip = argv[++i];
            }
            else if (arg == "--target-port" && i + 1 < argc)
            {
                cfg.target_port = parse_u16(argv[++i], cfg.target_port);
            }
            else if (arg == "--bind-ip" && i + 1 < argc)
            {
                cfg.bind_ips.push_back(argv[++i]);
            }
            else if (arg == "--bind-device" && i + 1 < argc)
            {
                cfg.bind_device = argv[++i];
            }
            else if (arg == "--cpu-list" && i + 1 < argc)
            {
                bool ok = false;
                cfg.cpu_list = parse_cpu_list(argv[++i], ok);
                if (!ok)
                {
                    std::cerr << "invalid --cpu-list\n";
                    print_usage(argv[0]);
                    std::exit(2);
                }
            }
            else if (arg == "--cpi-frags" && i + 1 < argc)
            {
                cfg.cpi_frags = parse_u16(argv[++i], cfg.cpi_frags);
            }
            else if (arg == "--frame-rate" && i + 1 < argc)
            {
                cfg.frame_rate = parse_double(argv[++i], cfg.frame_rate);
            }
            else if (arg == "--drop-rate" && i + 1 < argc)
            {
                cfg.drop_rate = parse_double(argv[++i], cfg.drop_rate);
            }
            else if (arg == "--reorder-depth" && i + 1 < argc)
            {
                cfg.reorder_depth = parse_size(argv[++i], cfg.reorder_depth);
            }
            else if (arg == "--duration" && i + 1 < argc)
            {
                cfg.duration_sec = parse_double(argv[++i], cfg.duration_sec);
            }
            else if (arg == "--arrays" && i + 1 < argc)
            {
                cfg.arrays = std::min<size_t>(3, std::max<size_t>(1, parse_size(argv[++i], cfg.arrays)));
            }
            else if (arg == "--frag-payload" && i + 1 < argc)
            {
                cfg.frag_payload_bytes = parse_size(argv[++i], cfg.frag_payload_bytes);
            }
            else if (arg == "--heartbeat-ms" && i + 1 < argc)
            {
                cfg.heartbeat_interval_ms = static_cast<uint32_t>(parse_size(argv[++i], cfg.heartbeat_interval_ms));
            }
            else if (arg == "--pps" && i + 1 < argc)
            {
                cfg.pps = parse_double(argv[++i], cfg.pps);
            }
            else if (arg == "--payload-bytes" && i + 1 < argc)
            {
                cfg.payload_bytes = parse_size(argv[++i], cfg.payload_bytes);
            }
            else if (arg == "--threads" && i + 1 < argc)
            {
                cfg.threads = std::max<size_t>(1, parse_size(argv[++i], cfg.threads));
            }
            else if (arg == "--output" && i + 1 < argc)
            {
                cfg.output_json = argv[++i];
            }
            else if (arg == "--help")
            {
                print_usage(argv[0]);
                std::exit(0);
            }
            else
            {
                std::cerr << "unknown argument: " << arg << "\n";
                print_usage(argv[0]);
                std::exit(2);
            }
        }

        if (cfg.output_json.empty())
            cfg.output_json = "fpga_emulator_report.json";
        return cfg;
    }

    bool write_text(const std::string &path, const std::string &text)
    {
        std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open())
            return false;
        ofs << text;
        return ofs.good();
    }

    bool apply_thread_affinity(int cpu)
    {
#if defined(__linux__)
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(cpu, &cpuset);
        return pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) == 0;
#else
        (void)cpu;
        return true;
#endif
    }

    int create_udp_socket(const std::string &bind_device, const std::string &bind_ip, std::string &error)
    {
        const int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
        if (sockfd < 0)
        {
            error = "socket() failed errno=" + std::to_string(errno);
            return -1;
        }

#if defined(__linux__)
        if (!bind_device.empty())
        {
            if (setsockopt(sockfd, SOL_SOCKET, SO_BINDTODEVICE,
                           bind_device.c_str(),
                           static_cast<socklen_t>(bind_device.size() + 1)) != 0)
            {
                error = "SO_BINDTODEVICE(" + bind_device + ") failed errno=" + std::to_string(errno);
                close(sockfd);
                return -1;
            }
        }
#else
        (void)bind_device;
#endif

        if (!bind_ip.empty())
        {
            sockaddr_in src{};
            src.sin_family = AF_INET;
            src.sin_port = htons(0);
            if (inet_pton(AF_INET, bind_ip.c_str(), &src.sin_addr) != 1)
            {
                error = "invalid bind-ip: " + bind_ip;
                close(sockfd);
                return -1;
            }
            if (bind(sockfd, reinterpret_cast<const sockaddr *>(&src), sizeof(src)) != 0)
            {
                error = "bind(" + bind_ip + ") errno=" + std::to_string(errno);
                close(sockfd);
                return -1;
            }
        }

        return sockfd;
    }

    // ═══ Array sender statistics ═══
    struct ArrayStats
    {
        std::atomic<uint64_t> packets_sent{0};
        std::atomic<uint64_t> bytes_sent{0};
        std::atomic<uint64_t> cpis_sent{0};
        std::atomic<uint64_t> heartbeats_sent{0};
        std::atomic<uint64_t> send_errors{0};
    };

    // ═══ CPI-mode array sender ═══
    void run_array_sender(const Config &cfg,
                          size_t array_index,
                          const std::chrono::steady_clock::time_point &end_at,
                          ArrayStats &stats)
    {
        // CPU affinity
        if (!cfg.cpu_list.empty())
        {
            const int cpu = cfg.cpu_list[array_index % cfg.cpu_list.size()];
            if (!apply_thread_affinity(cpu))
            {
                std::cerr << "array " << (array_index + 1) << " failed to bind cpu " << cpu << "\n";
            }
        }

        // Source IP for this array
        const std::string bind_ip = (array_index < cfg.bind_ips.size())
                                        ? cfg.bind_ips[array_index]
                                        : std::string{};

        std::string error;
        const int sockfd = create_udp_socket(cfg.bind_device, bind_ip, error);
        if (sockfd < 0)
        {
            std::cerr << "array " << (array_index + 1) << " socket error: " << error << "\n";
            return;
        }

        sockaddr_in dst{};
        dst.sin_family = AF_INET;
        dst.sin_port = htons(cfg.target_port);
        if (inet_pton(AF_INET, cfg.target_ip.c_str(), &dst.sin_addr) != 1)
        {
            std::cerr << "invalid --target-ip: " << cfg.target_ip << "\n";
            close(sockfd);
            return;
        }

        // DACS device IDs: 0x11(array1), 0x12(array2), 0x13(array3)
        fpga_emulator::GeneratorConfig gen_cfg;
        gen_cfg.source_id = static_cast<uint8_t>(0x11 + array_index);
        gen_cfg.dest_id = 0x01; // SPS
        gen_cfg.cpi_frags = cfg.cpi_frags;
        gen_cfg.frag_payload_bytes = cfg.frag_payload_bytes;
        gen_cfg.drop_rate = cfg.drop_rate;
        gen_cfg.reorder_depth = cfg.reorder_depth;

        fpga_emulator::PacketGenerator generator(gen_cfg);

        const auto cpi_interval = std::chrono::duration<double>(1.0 / std::max(0.1, cfg.frame_rate));
        const auto heartbeat_interval = std::chrono::milliseconds(cfg.heartbeat_interval_ms);
        auto next_cpi_tick = std::chrono::steady_clock::now();
        auto next_heartbeat = std::chrono::steady_clock::now() + heartbeat_interval;

        while (std::chrono::steady_clock::now() < end_at)
        {
            // Wait for next CPI frame time
            std::this_thread::sleep_until(next_cpi_tick);
            next_cpi_tick += std::chrono::duration_cast<std::chrono::steady_clock::duration>(cpi_interval);

            // Generate CPI fragments
            auto cpi_packets = generator.make_cpi_packets();
            cpi_packets = generator.apply_impairments(std::move(cpi_packets));

            // Send all fragments
            for (const auto &pkt : cpi_packets)
            {
                const ssize_t n = sendto(sockfd, pkt.data(), pkt.size(), 0,
                                         reinterpret_cast<const sockaddr *>(&dst), sizeof(dst));
                if (n == static_cast<ssize_t>(pkt.size()))
                {
                    stats.packets_sent.fetch_add(1, std::memory_order_relaxed);
                    stats.bytes_sent.fetch_add(static_cast<uint64_t>(pkt.size()), std::memory_order_relaxed);
                }
                else
                {
                    stats.send_errors.fetch_add(1, std::memory_order_relaxed);
                }
            }
            stats.cpis_sent.fetch_add(1, std::memory_order_relaxed);

            // Heartbeat injection
            if (cfg.heartbeat_interval_ms > 0 &&
                std::chrono::steady_clock::now() >= next_heartbeat)
            {
                auto hb = generator.make_heartbeat_packet();
                const ssize_t n = sendto(sockfd, hb.data(), hb.size(), 0,
                                         reinterpret_cast<const sockaddr *>(&dst), sizeof(dst));
                if (n == static_cast<ssize_t>(hb.size()))
                {
                    stats.heartbeats_sent.fetch_add(1, std::memory_order_relaxed);
                }
                next_heartbeat += heartbeat_interval;
            }
        }

        close(sockfd);
    }

    // ═══ Legacy PPS-mode sender ═══
    void run_pps_sender(const Config &cfg,
                        size_t thread_idx,
                        const std::chrono::steady_clock::time_point &end_at,
                        std::atomic<uint64_t> &sent_ok,
                        std::atomic<uint64_t> &send_fail)
    {
        if (!cfg.cpu_list.empty())
        {
            const int cpu = cfg.cpu_list[thread_idx % cfg.cpu_list.size()];
            if (!apply_thread_affinity(cpu))
            {
                std::cerr << "thread " << thread_idx << " failed to bind cpu " << cpu << "\n";
                return;
            }
        }

        const std::string bind_ip = (thread_idx < cfg.bind_ips.size())
                                        ? cfg.bind_ips[thread_idx]
                                        : std::string{};

        std::string error;
        const int sockfd = create_udp_socket(cfg.bind_device, bind_ip, error);
        if (sockfd < 0)
        {
            std::cerr << "thread " << thread_idx << " socket error: " << error << "\n";
            return;
        }

        sockaddr_in dst{};
        dst.sin_family = AF_INET;
        dst.sin_port = htons(cfg.target_port);
        if (inet_pton(AF_INET, cfg.target_ip.c_str(), &dst.sin_addr) != 1)
        {
            close(sockfd);
            return;
        }

        fpga_emulator::PacketGenerator generator(cfg.payload_bytes);
        const double pps_per_thread = cfg.pps / static_cast<double>(cfg.threads);
        const auto interval = std::chrono::duration<double>(1.0 / std::max(1.0, pps_per_thread));
        auto next_tick = std::chrono::steady_clock::now();

        while (std::chrono::steady_clock::now() < end_at)
        {
            std::this_thread::sleep_until(next_tick);
            next_tick += std::chrono::duration_cast<std::chrono::steady_clock::duration>(interval);

            auto packet = generator.make_data_packet();
            const ssize_t n = sendto(sockfd, packet.data(), packet.size(), 0,
                                     reinterpret_cast<const sockaddr *>(&dst), sizeof(dst));
            if (n == static_cast<ssize_t>(packet.size()))
                sent_ok.fetch_add(1, std::memory_order_relaxed);
            else
                send_fail.fetch_add(1, std::memory_order_relaxed);
        }

        close(sockfd);
    }
} // namespace

int main(int argc, char **argv)
{
    const Config cfg = parse_args(argc, argv);
    const auto started = std::chrono::steady_clock::now();
    const auto end_at = started + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                      std::chrono::duration<double>(cfg.duration_sec));

    // ═══ CPI mode (default) ═══
    if (cfg.pps <= 0.0)
    {
        std::cout << "[fpga_emulator] CPI mode: arrays=" << cfg.arrays
                  << " cpi_frags=" << cfg.cpi_frags
                  << " frame_rate=" << cfg.frame_rate << "Hz"
                  << " duration=" << cfg.duration_sec << "s"
                  << " drop_rate=" << cfg.drop_rate
                  << " reorder_depth=" << cfg.reorder_depth
                  << " heartbeat_ms=" << cfg.heartbeat_interval_ms << "\n";

        std::vector<ArrayStats> array_stats(cfg.arrays);
        std::vector<std::thread> workers;
        workers.reserve(cfg.arrays);

        for (size_t a = 0; a < cfg.arrays; ++a)
        {
            workers.emplace_back(run_array_sender,
                                 std::cref(cfg), a, std::cref(end_at),
                                 std::ref(array_stats[a]));
        }

        for (auto &w : workers)
            w.join();

        const auto finished = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(finished - started).count();

        // Aggregate stats
        uint64_t total_packets = 0, total_bytes = 0, total_cpis = 0;
        uint64_t total_heartbeats = 0, total_errors = 0;
        for (size_t a = 0; a < cfg.arrays; ++a)
        {
            total_packets += array_stats[a].packets_sent.load(std::memory_order_relaxed);
            total_bytes += array_stats[a].bytes_sent.load(std::memory_order_relaxed);
            total_cpis += array_stats[a].cpis_sent.load(std::memory_order_relaxed);
            total_heartbeats += array_stats[a].heartbeats_sent.load(std::memory_order_relaxed);
            total_errors += array_stats[a].send_errors.load(std::memory_order_relaxed);
        }

        const double pps = (elapsed > 0.0) ? (static_cast<double>(total_packets) / elapsed) : 0.0;
        const double mbps = (elapsed > 0.0) ? (static_cast<double>(total_bytes) * 8.0 / elapsed / 1e6) : 0.0;

        // Build JSON report
        std::ostringstream report;
        report << std::fixed;
        report << "{\n";
        report << "  \"tool\": \"fpga_emulator\",\n";
        report << "  \"mode\": \"cpi\",\n";
        report << "  \"target_ip\": \"" << cfg.target_ip << "\",\n";
        report << "  \"target_port\": " << cfg.target_port << ",\n";
        report << "  \"arrays\": " << cfg.arrays << ",\n";
        report << "  \"cpi_frags\": " << cfg.cpi_frags << ",\n";
        report << "  \"frame_rate_hz\": " << std::setprecision(2) << cfg.frame_rate << ",\n";
        report << "  \"drop_rate\": " << std::setprecision(4) << cfg.drop_rate << ",\n";
        report << "  \"reorder_depth\": " << cfg.reorder_depth << ",\n";
        report << "  \"duration_sec\": " << std::setprecision(2) << elapsed << ",\n";
        report << "  \"total_packets\": " << total_packets << ",\n";
        report << "  \"total_bytes\": " << total_bytes << ",\n";
        report << "  \"total_cpis\": " << total_cpis << ",\n";
        report << "  \"total_heartbeats\": " << total_heartbeats << ",\n";
        report << "  \"send_errors\": " << total_errors << ",\n";
        report << "  \"actual_pps\": " << std::setprecision(2) << pps << ",\n";
        report << "  \"throughput_mbps\": " << std::setprecision(2) << mbps << ",\n";
        report << "  \"per_array\": [\n";
        for (size_t a = 0; a < cfg.arrays; ++a)
        {
            const auto &s = array_stats[a];
            report << "    {\"array\": " << (a + 1)
                   << ", \"packets\": " << s.packets_sent.load(std::memory_order_relaxed)
                   << ", \"bytes\": " << s.bytes_sent.load(std::memory_order_relaxed)
                   << ", \"cpis\": " << s.cpis_sent.load(std::memory_order_relaxed)
                   << ", \"heartbeats\": " << s.heartbeats_sent.load(std::memory_order_relaxed)
                   << ", \"errors\": " << s.send_errors.load(std::memory_order_relaxed)
                   << "}";
            if (a + 1 < cfg.arrays)
                report << ",";
            report << "\n";
        }
        report << "  ]\n";
        report << "}\n";

        const std::string text = report.str();
        std::cout << text;
        if (!write_text(cfg.output_json, text))
        {
            std::cerr << "failed to write report: " << cfg.output_json << "\n";
            return 1;
        }
        std::cout << "report_path=" << cfg.output_json << "\n";
        return total_errors > 0 ? 1 : 0;
    }

    // ═══ Legacy PPS mode ═══
    std::cout << "[fpga_emulator] Legacy PPS mode: pps=" << cfg.pps
              << " threads=" << cfg.threads
              << " duration=" << cfg.duration_sec << "s\n";

    std::atomic<uint64_t> sent_ok{0};
    std::atomic<uint64_t> send_fail{0};
    std::vector<std::thread> workers;
    workers.reserve(cfg.threads);

    for (size_t t = 0; t < cfg.threads; ++t)
    {
        workers.emplace_back(run_pps_sender,
                             std::cref(cfg), t, std::cref(end_at),
                             std::ref(sent_ok), std::ref(send_fail));
    }

    for (auto &w : workers)
        w.join();

    const auto finished = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(finished - started).count();
    const uint64_t sent = sent_ok.load(std::memory_order_relaxed);
    const uint64_t failed = send_fail.load(std::memory_order_relaxed);
    const double actual_pps = (elapsed > 0.0) ? (static_cast<double>(sent) / elapsed) : 0.0;

    std::ostringstream report;
    report << "{\n";
    report << "  \"tool\": \"fpga_emulator\",\n";
    report << "  \"mode\": \"pps\",\n";
    report << "  \"target_ip\": \"" << cfg.target_ip << "\",\n";
    report << "  \"target_port\": " << cfg.target_port << ",\n";
    report << "  \"target_pps\": " << cfg.pps << ",\n";
    report << "  \"actual_pps\": " << std::fixed << std::setprecision(2) << actual_pps << ",\n";
    report << "  \"duration_sec\": " << cfg.duration_sec << ",\n";
    report << "  \"payload_bytes\": " << cfg.payload_bytes << ",\n";
    report << "  \"threads\": " << cfg.threads << ",\n";
    report << "  \"sent_packets\": " << sent << ",\n";
    report << "  \"failed_packets\": " << failed << "\n";
    report << "}\n";

    const std::string text = report.str();
    std::cout << text;
    if (!write_text(cfg.output_json, text))
    {
        std::cerr << "failed to write report: " << cfg.output_json << "\n";
        return 1;
    }
    std::cout << "report_path=" << cfg.output_json << "\n";
    return (failed > 0) ? 1 : 0;
}
