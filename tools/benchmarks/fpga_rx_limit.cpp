#include "qdgz300/m01_receiver/protocol/protocol_types.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
#endif

namespace
{
    using receiver::protocol::CommonHeader;
    using receiver::protocol::DataSpecificHeader;
    using receiver::protocol::DeviceID;
    using receiver::protocol::PacketType;
    using receiver::protocol::PROTOCOL_MAGIC;
    using receiver::protocol::PROTOCOL_VERSION;

    struct BenchConfig
    {
        std::string bind_ip{"127.0.0.1"};
        uint16_t port{9999};
        size_t payload_bytes{256};
        double trial_duration_sec{4.0};
        double warmup_sec{1.0};
        size_t sender_threads{2};
        double loss_threshold{0.01};
        size_t binary_search_rounds{8};
        double start_pps{10000.0};
        double max_pps{2000000.0};
        std::string output_json{};
    };

    struct TrialResult
    {
        double target_pps{0.0};
        double sent_pps{0.0};
        double recv_pps{0.0};
        double throughput_mbps{0.0};
        uint64_t sent_packets{0};
        uint64_t recv_packets{0};
        uint64_t recv_valid_packets{0};
        double loss_rate{0.0};
        bool pass{false};
    };

    struct SearchResult
    {
        double best_target_pps{0.0};
        TrialResult best_trial{};
        std::vector<TrialResult> probes{};
    };

    uint64_t unix_now_ms()
    {
        const auto now = std::chrono::system_clock::now();
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
    }

    std::string default_report_path()
    {
        const auto ts = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
        return "fpga_rx_limit_report_" + std::to_string(ts) + ".json";
    }

    double parse_double(const char *s, double fallback)
    {
        if (s == nullptr)
        {
            return fallback;
        }
        char *end = nullptr;
        const double v = std::strtod(s, &end);
        if (end == s || *end != '\0')
        {
            return fallback;
        }
        return v;
    }

    size_t parse_size(const char *s, size_t fallback)
    {
        if (s == nullptr)
        {
            return fallback;
        }
        char *end = nullptr;
        const unsigned long long v = std::strtoull(s, &end, 10);
        if (end == s || *end != '\0')
        {
            return fallback;
        }
        return static_cast<size_t>(v);
    }

    uint16_t parse_u16(const char *s, uint16_t fallback)
    {
        const size_t v = parse_size(s, fallback);
        if (v > 65535u)
        {
            return fallback;
        }
        return static_cast<uint16_t>(v);
    }

    BenchConfig parse_args(int argc, char **argv)
    {
        BenchConfig cfg;
        for (int i = 1; i < argc; ++i)
        {
            const std::string arg = argv[i];
            if (arg == "--bind-ip" && i + 1 < argc)
            {
                cfg.bind_ip = argv[++i];
            }
            else if (arg == "--port" && i + 1 < argc)
            {
                cfg.port = parse_u16(argv[++i], cfg.port);
            }
            else if (arg == "--payload-bytes" && i + 1 < argc)
            {
                cfg.payload_bytes = parse_size(argv[++i], cfg.payload_bytes);
            }
            else if (arg == "--duration" && i + 1 < argc)
            {
                cfg.trial_duration_sec = parse_double(argv[++i], cfg.trial_duration_sec);
            }
            else if (arg == "--warmup" && i + 1 < argc)
            {
                cfg.warmup_sec = parse_double(argv[++i], cfg.warmup_sec);
            }
            else if (arg == "--sender-threads" && i + 1 < argc)
            {
                cfg.sender_threads = std::max<size_t>(1, parse_size(argv[++i], cfg.sender_threads));
            }
            else if (arg == "--loss-threshold" && i + 1 < argc)
            {
                cfg.loss_threshold = parse_double(argv[++i], cfg.loss_threshold);
            }
            else if (arg == "--start-pps" && i + 1 < argc)
            {
                cfg.start_pps = parse_double(argv[++i], cfg.start_pps);
            }
            else if (arg == "--max-pps" && i + 1 < argc)
            {
                cfg.max_pps = parse_double(argv[++i], cfg.max_pps);
            }
            else if (arg == "--rounds" && i + 1 < argc)
            {
                cfg.binary_search_rounds = std::max<size_t>(1, parse_size(argv[++i], cfg.binary_search_rounds));
            }
            else if (arg == "--output" && i + 1 < argc)
            {
                cfg.output_json = argv[++i];
            }
        }
        if (cfg.output_json.empty())
        {
            cfg.output_json = default_report_path();
        }
        return cfg;
    }

    bool platform_init()
    {
#if defined(_WIN32)
        WSADATA wsa_data{};
        return WSAStartup(MAKEWORD(2, 2), &wsa_data) == 0;
#else
        return true;
#endif
    }

    void platform_cleanup()
    {
#if defined(_WIN32)
        WSACleanup();
#endif
    }

    void close_socket(SocketHandle fd)
    {
#if defined(_WIN32)
        if (fd != kInvalidSocket)
        {
            closesocket(fd);
        }
#else
        if (fd >= 0)
        {
            close(fd);
        }
#endif
    }

    bool set_recv_timeout_ms(SocketHandle fd, int timeout_ms)
    {
#if defined(_WIN32)
        const DWORD tv = static_cast<DWORD>(timeout_ms);
        return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&tv), sizeof(tv)) == 0;
#else
        timeval tv{};
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
#endif
    }

    bool is_data_packet_valid(const uint8_t *data, size_t len)
    {
        if (data == nullptr || len < sizeof(CommonHeader) + sizeof(DataSpecificHeader))
        {
            return false;
        }
        CommonHeader h{};
        std::memcpy(&h, data, sizeof(h));
        if (h.magic != PROTOCOL_MAGIC)
        {
            return false;
        }
        if (h.protocol_version != PROTOCOL_VERSION)
        {
            return false;
        }
        if (h.packet_type != static_cast<uint8_t>(PacketType::DATA))
        {
            return false;
        }
        if (len != static_cast<size_t>(sizeof(CommonHeader) + h.payload_len))
        {
            return false;
        }
        DataSpecificHeader s{};
        std::memcpy(&s, data + sizeof(CommonHeader), sizeof(s));
        if (s.total_frags == 0 || s.frag_index >= s.total_frags)
        {
            return false;
        }
        return true;
    }

    std::vector<uint8_t> make_packet_template(size_t payload_bytes)
    {
        if (payload_bytes < sizeof(DataSpecificHeader))
        {
            payload_bytes = sizeof(DataSpecificHeader);
        }
        std::vector<uint8_t> pkt(sizeof(CommonHeader) + payload_bytes, 0);

        CommonHeader h{};
        h.magic = PROTOCOL_MAGIC;
        h.sequence_number = 1;
        h.timestamp = unix_now_ms();
        h.payload_len = static_cast<uint16_t>(payload_bytes);
        h.packet_type = static_cast<uint8_t>(PacketType::DATA);
        h.protocol_version = PROTOCOL_VERSION;
        h.source_id = static_cast<uint8_t>(DeviceID::DACS_01);
        h.dest_id = static_cast<uint8_t>(DeviceID::SPS);
        h.control_epoch = 1;
        std::memcpy(pkt.data(), &h, sizeof(h));

        DataSpecificHeader s{};
        s.frame_counter = 1;
        s.cpi_count = 1;
        s.pulse_index = 0;
        s.sample_offset = 0;
        s.sample_count = 256;
        s.data_timestamp = unix_now_ms();
        s.health_summary = 0;
        s.set_channel_mask_data_type_compat(0x0001u, 0x00u);
        s.beam_id = 1;
        s.frag_index = 0;
        s.total_frags = 1;
        s.tail_frag_payload_bytes = static_cast<uint16_t>(payload_bytes - sizeof(DataSpecificHeader));
        std::memcpy(pkt.data() + sizeof(CommonHeader), &s, sizeof(s));

        for (size_t i = sizeof(CommonHeader) + sizeof(DataSpecificHeader); i < pkt.size(); ++i)
        {
            pkt[i] = static_cast<uint8_t>(i & 0xFFu);
        }
        return pkt;
    }

    TrialResult run_trial(const BenchConfig &cfg, double target_pps)
    {
        TrialResult out;
        out.target_pps = target_pps;

        SocketHandle rx_fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (rx_fd == kInvalidSocket)
        {
            return out;
        }

        sockaddr_in rx_addr{};
        rx_addr.sin_family = AF_INET;
        rx_addr.sin_port = htons(cfg.port);
        rx_addr.sin_addr.s_addr = inet_addr(cfg.bind_ip.c_str());
        if (bind(rx_fd, reinterpret_cast<sockaddr *>(&rx_addr), sizeof(rx_addr)) != 0)
        {
            close_socket(rx_fd);
            return out;
        }
        (void)set_recv_timeout_ms(rx_fd, 50);

        std::atomic<bool> running{true};
        std::atomic<uint64_t> recv_packets{0};
        std::atomic<uint64_t> recv_valid_packets{0};

        std::thread receiver([&]()
                             {
                                 std::vector<uint8_t> buf(65536);
                                 while (running.load(std::memory_order_acquire))
                                 {
                                     const int n = recv(rx_fd, reinterpret_cast<char *>(buf.data()), static_cast<int>(buf.size()), 0);
                                     if (n <= 0)
                                     {
                                         continue;
                                     }
                                     recv_packets.fetch_add(1, std::memory_order_relaxed);
                                     if (is_data_packet_valid(buf.data(), static_cast<size_t>(n)))
                                     {
                                         recv_valid_packets.fetch_add(1, std::memory_order_relaxed);
                                     }
                                 } });

        std::atomic<uint64_t> global_seq{1};
        std::atomic<uint64_t> global_frame{1};
        std::atomic<uint64_t> sent_packets{0};
        std::vector<std::thread> senders;
        senders.reserve(cfg.sender_threads);

        const double pps_per_thread = target_pps / static_cast<double>(cfg.sender_threads);
        const auto interval = std::chrono::duration<double>(1.0 / std::max(1.0, pps_per_thread));

        for (size_t t = 0; t < cfg.sender_threads; ++t)
        {
            senders.emplace_back([&]()
                                 {
                                     SocketHandle tx_fd = socket(AF_INET, SOCK_DGRAM, 0);
                                     if (tx_fd == kInvalidSocket)
                                     {
                                         return;
                                     }
                                     sockaddr_in dst{};
                                     dst.sin_family = AF_INET;
                                     dst.sin_port = htons(cfg.port);
                                     dst.sin_addr.s_addr = inet_addr(cfg.bind_ip.c_str());

                                     std::vector<uint8_t> pkt = make_packet_template(cfg.payload_bytes);
                                     auto next_tick = std::chrono::steady_clock::now();
                                     const auto end_at = std::chrono::steady_clock::now() +
                                                         std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                                             std::chrono::duration<double>(cfg.trial_duration_sec + cfg.warmup_sec));

                                     while (std::chrono::steady_clock::now() < end_at)
                                     {
                                         std::this_thread::sleep_until(next_tick);
                                         next_tick += std::chrono::duration_cast<std::chrono::steady_clock::duration>(interval);

                                         CommonHeader h{};
                                         std::memcpy(&h, pkt.data(), sizeof(h));
                                         h.sequence_number = static_cast<uint32_t>(global_seq.fetch_add(1, std::memory_order_relaxed));
                                         h.timestamp = unix_now_ms();
                                         std::memcpy(pkt.data(), &h, sizeof(h));

                                         DataSpecificHeader s{};
                                         std::memcpy(&s, pkt.data() + sizeof(CommonHeader), sizeof(s));
                                         s.frame_counter = static_cast<uint32_t>(global_frame.fetch_add(1, std::memory_order_relaxed));
                                         s.cpi_count = s.frame_counter;
                                         s.data_timestamp = unix_now_ms();
                                         std::memcpy(pkt.data() + sizeof(CommonHeader), &s, sizeof(s));

                                         const int n = sendto(tx_fd,
                                                              reinterpret_cast<const char *>(pkt.data()),
                                                              static_cast<int>(pkt.size()),
                                                              0,
                                                              reinterpret_cast<sockaddr *>(&dst),
                                                              static_cast<int>(sizeof(dst)));
                                         if (n > 0)
                                         {
                                             sent_packets.fetch_add(1, std::memory_order_relaxed);
                                         }
                                     }
                                     close_socket(tx_fd); });
        }

        std::this_thread::sleep_for(std::chrono::duration<double>(cfg.warmup_sec));
        const auto measured_begin = std::chrono::steady_clock::now();
        const uint64_t sent_begin = sent_packets.load(std::memory_order_relaxed);
        const uint64_t recv_begin = recv_packets.load(std::memory_order_relaxed);
        const uint64_t recv_valid_begin = recv_valid_packets.load(std::memory_order_relaxed);

        std::this_thread::sleep_for(std::chrono::duration<double>(cfg.trial_duration_sec));
        const auto measured_end = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(measured_end - measured_begin).count();

        const uint64_t sent_end = sent_packets.load(std::memory_order_relaxed);
        const uint64_t recv_end = recv_packets.load(std::memory_order_relaxed);
        const uint64_t recv_valid_end = recv_valid_packets.load(std::memory_order_relaxed);

        out.sent_packets = (sent_end >= sent_begin) ? (sent_end - sent_begin) : 0;
        out.recv_packets = (recv_end >= recv_begin) ? (recv_end - recv_begin) : 0;
        out.recv_valid_packets = (recv_valid_end >= recv_valid_begin) ? (recv_valid_end - recv_valid_begin) : 0;
        out.sent_pps = (elapsed > 0.0) ? (static_cast<double>(out.sent_packets) / elapsed) : 0.0;
        out.recv_pps = (elapsed > 0.0) ? (static_cast<double>(out.recv_valid_packets) / elapsed) : 0.0;
        out.throughput_mbps = out.recv_pps * static_cast<double>(sizeof(CommonHeader) + cfg.payload_bytes) * 8.0 / 1e6;
        out.loss_rate = (out.sent_packets > 0)
                            ? 1.0 - (static_cast<double>(out.recv_valid_packets) / static_cast<double>(out.sent_packets))
                            : 1.0;

        for (auto &th : senders)
        {
            th.join();
        }
        running.store(false, std::memory_order_release);
        receiver.join();
        close_socket(rx_fd);
        return out;
    }

    SearchResult find_limit(const BenchConfig &cfg)
    {
        SearchResult result;

        double low = cfg.start_pps;
        double high = std::min(cfg.max_pps, cfg.start_pps);

        TrialResult low_trial = run_trial(cfg, low);
        low_trial.pass = (low_trial.loss_rate <= cfg.loss_threshold);
        result.probes.push_back(low_trial);
        if (low_trial.pass)
        {
            result.best_target_pps = low;
            result.best_trial = low_trial;
        }

        while (high < cfg.max_pps)
        {
            const double next = std::min(cfg.max_pps, high * 2.0);
            TrialResult t = run_trial(cfg, next);
            t.pass = (t.loss_rate <= cfg.loss_threshold);
            result.probes.push_back(t);
            if (t.pass)
            {
                low = next;
                result.best_target_pps = next;
                result.best_trial = t;
                high = next;
                continue;
            }
            high = next;
            break;
        }

        if (result.probes.back().pass && high >= cfg.max_pps)
        {
            return result;
        }

        double l = std::max(cfg.start_pps, low);
        double r = high;
        for (size_t i = 0; i < cfg.binary_search_rounds; ++i)
        {
            const double mid = (l + r) * 0.5;
            TrialResult t = run_trial(cfg, mid);
            t.pass = (t.loss_rate <= cfg.loss_threshold);
            result.probes.push_back(t);
            if (t.pass)
            {
                l = mid;
                result.best_target_pps = mid;
                result.best_trial = t;
            }
            else
            {
                r = mid;
            }
        }
        return result;
    }

    std::string build_report_json(const BenchConfig &cfg, const SearchResult &sr)
    {
        std::ostringstream out;
        out << "{\n";
        out << "  \"tool\": \"fpga_rx_limit\",\n";
        out << "  \"config\": {\n";
        out << "    \"bind_ip\": \"" << cfg.bind_ip << "\",\n";
        out << "    \"port\": " << cfg.port << ",\n";
        out << "    \"payload_bytes\": " << cfg.payload_bytes << ",\n";
        out << "    \"sender_threads\": " << cfg.sender_threads << ",\n";
        out << "    \"trial_duration_sec\": " << cfg.trial_duration_sec << ",\n";
        out << "    \"loss_threshold\": " << cfg.loss_threshold << "\n";
        out << "  },\n";
        out << "  \"limit_result\": {\n";
        out << "    \"best_target_pps\": " << std::fixed << std::setprecision(2) << sr.best_target_pps << ",\n";
        out << "    \"best_sent_pps\": " << sr.best_trial.sent_pps << ",\n";
        out << "    \"best_recv_pps\": " << sr.best_trial.recv_pps << ",\n";
        out << "    \"best_throughput_mbps\": " << sr.best_trial.throughput_mbps << ",\n";
        out << "    \"best_loss_rate\": " << sr.best_trial.loss_rate << ",\n";
        out << "    \"best_sent_packets\": " << sr.best_trial.sent_packets << ",\n";
        out << "    \"best_recv_valid_packets\": " << sr.best_trial.recv_valid_packets << "\n";
        out << "  },\n";
        out << "  \"probes\": [\n";
        for (size_t i = 0; i < sr.probes.size(); ++i)
        {
            const auto &p = sr.probes[i];
            out << "    {\"target_pps\": " << p.target_pps
                << ", \"sent_pps\": " << p.sent_pps
                << ", \"recv_pps\": " << p.recv_pps
                << ", \"throughput_mbps\": " << p.throughput_mbps
                << ", \"loss_rate\": " << p.loss_rate
                << ", \"pass\": " << (p.pass ? "true" : "false") << "}";
            out << ((i + 1 < sr.probes.size()) ? ",\n" : "\n");
        }
        out << "  ]\n";
        out << "}\n";
        return out.str();
    }

    bool write_text(const std::string &path, const std::string &text)
    {
        std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open())
        {
            return false;
        }
        ofs << text;
        return ofs.good();
    }
} // namespace

int main(int argc, char **argv)
{
    const BenchConfig cfg = parse_args(argc, argv);
    if (!platform_init())
    {
        std::cerr << "socket platform init failed\n";
        return 1;
    }

    const SearchResult sr = find_limit(cfg);
    const std::string report = build_report_json(cfg, sr);

    std::cout << report;
    if (!write_text(cfg.output_json, report))
    {
        std::cerr << "failed to write report: " << cfg.output_json << "\n";
        platform_cleanup();
        return 2;
    }
    std::cout << "report_path=" << cfg.output_json << "\n";

    platform_cleanup();
    return 0;
}
