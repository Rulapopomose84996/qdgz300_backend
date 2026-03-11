#include "qdgz300/m01_receiver/network/udp_receiver.h"
#include "qdgz300/m01_receiver/protocol/packet_parser.h"
#include "qdgz300/m01_receiver/protocol/validator.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>

namespace
{
    struct Config
    {
        std::string bind_ip{"0.0.0.0"};
        uint16_t port{9999};
        uint8_t local_device_id{0x01};
        double measure_sec{3.0};
        double warmup_sec{0.3};
        double startup_timeout_sec{5.0};
        std::string output_json{"m01_rx_limit_result.json"};
    };

    double parse_double(const char *s, double fallback)
    {
        if (s == nullptr)
        {
            return fallback;
        }
        char *end = nullptr;
        const double value = std::strtod(s, &end);
        if (end == s || *end != '\0')
        {
            return fallback;
        }
        return value;
    }

    size_t parse_size(const char *s, size_t fallback)
    {
        if (s == nullptr)
        {
            return fallback;
        }
        char *end = nullptr;
        const unsigned long long value = std::strtoull(s, &end, 10);
        if (end == s || *end != '\0')
        {
            return fallback;
        }
        return static_cast<size_t>(value);
    }

    Config parse_args(int argc, char **argv)
    {
        Config cfg;
        for (int i = 1; i < argc; ++i)
        {
            const std::string arg = argv[i];
            if (arg == "--bind-ip" && i + 1 < argc)
            {
                cfg.bind_ip = argv[++i];
            }
            else if (arg == "--port" && i + 1 < argc)
            {
                cfg.port = static_cast<uint16_t>(parse_size(argv[++i], cfg.port));
            }
            else if (arg == "--local-device-id" && i + 1 < argc)
            {
                cfg.local_device_id = static_cast<uint8_t>(parse_size(argv[++i], cfg.local_device_id));
            }
            else if (arg == "--measure-sec" && i + 1 < argc)
            {
                cfg.measure_sec = parse_double(argv[++i], cfg.measure_sec);
            }
            else if (arg == "--startup-timeout-sec" && i + 1 < argc)
            {
                cfg.startup_timeout_sec = parse_double(argv[++i], cfg.startup_timeout_sec);
            }
            else if (arg == "--warmup-sec" && i + 1 < argc)
            {
                cfg.warmup_sec = parse_double(argv[++i], cfg.warmup_sec);
            }
            else if (arg == "--output" && i + 1 < argc)
            {
                cfg.output_json = argv[++i];
            }
        }
        return cfg;
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
    using Clock = std::chrono::steady_clock;
    const Config cfg = parse_args(argc, argv);

    std::atomic<uint64_t> recv_total{0};
    std::atomic<uint64_t> recv_valid{0};
    receiver::protocol::PacketParser parser;
    receiver::protocol::Validator validator(
        cfg.local_device_id, receiver::protocol::Validator::Scope::DATA_AND_HEARTBEAT);

    receiver::network::UdpReceiverConfig rx_cfg;
    rx_cfg.bind_ip = cfg.bind_ip;
    rx_cfg.listen_port = cfg.port;
    rx_cfg.recv_batch_size = 64;
    rx_cfg.socket_rcvbuf_mb = 256;
    rx_cfg.array_faces.clear();
    rx_cfg.array_faces.push_back(receiver::network::ArrayFaceBinding{
        1,
        0x11,
        cfg.bind_ip,
        cfg.port,
        0,
        false});

    receiver::network::UdpReceiver receiver(
        rx_cfg,
        [&](receiver::network::ReceivedPacket &&pkt)
        {
            recv_total.fetch_add(1, std::memory_order_relaxed);
            auto parsed = parser.parse(pkt.data.get(), pkt.length);
            if (!parsed.has_value())
            {
                return;
            }
            if (validator.validate(parsed.value()) != receiver::protocol::ValidationResult::OK)
            {
                return;
            }
            recv_valid.fetch_add(1, std::memory_order_relaxed);
        });

    if (!receiver.start())
    {
        std::cerr << "receiver start failed on port " << cfg.port << "\n";
        return 2;
    }

    std::this_thread::sleep_for(std::chrono::duration<double>(cfg.warmup_sec));

    const uint64_t base_total = recv_total.load(std::memory_order_relaxed);
    const uint64_t base_valid = recv_valid.load(std::memory_order_relaxed);
    const auto t0 = Clock::now();
    std::this_thread::sleep_for(std::chrono::duration<double>(cfg.measure_sec));
    const auto t1 = Clock::now();

    const uint64_t total = recv_total.load(std::memory_order_relaxed) - base_total;
    const uint64_t valid = recv_valid.load(std::memory_order_relaxed) - base_valid;
    const double elapsed = std::chrono::duration<double>(t1 - t0).count();
    const double valid_pps = (elapsed > 0.0) ? (static_cast<double>(valid) / elapsed) : 0.0;

    receiver.stop();

    std::ostringstream out;
    out << "{\n";
    out << "  \"tool\": \"m01_rx_limit\",\n";
    out << "  \"bind_ip\": \"" << cfg.bind_ip << "\",\n";
    out << "  \"port\": " << cfg.port << ",\n";
    out << "  \"warmup_sec\": " << cfg.warmup_sec << ",\n";
    out << "  \"measure_sec\": " << cfg.measure_sec << ",\n";
    out << "  \"received_total\": " << total << ",\n";
    out << "  \"received_valid\": " << valid << ",\n";
    out << "  \"valid_pps\": " << std::fixed << std::setprecision(2) << valid_pps << "\n";
    out << "}\n";

    const std::string report = out.str();
    std::cout << report;
    if (!write_text(cfg.output_json, report))
    {
        std::cerr << "failed to write report: " << cfg.output_json << "\n";
        return 4;
    }
    std::cout << "report_path=" << cfg.output_json << "\n";
    return 0;
}
