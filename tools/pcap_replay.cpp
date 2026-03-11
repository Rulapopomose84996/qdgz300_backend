#include "qdgz300/m01_receiver/protocol/protocol_types.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace
{
    struct Options
    {
        std::string pcap_file;
        std::string target_ip = "127.0.0.1";
        uint16_t target_port = 9999;
        double speed = 1.0;
        bool loop = false;
        std::optional<uint64_t> count;
        std::optional<double> start_time_epoch;
        std::optional<double> end_time_epoch;
    };

    struct PcapFormat
    {
        bool little_endian = true;
        bool timestamp_nanoseconds = false;
        uint32_t snaplen = 0;
        uint32_t network = 0;
    };

    struct Record
    {
        uint32_t ts_sec = 0;
        uint32_t ts_frac = 0;
        std::vector<uint8_t> payload;

        double epoch_seconds(bool timestamp_nanoseconds) const
        {
            const double divisor = timestamp_nanoseconds ? 1e9 : 1e6;
            return static_cast<double>(ts_sec) + static_cast<double>(ts_frac) / divisor;
        }
    };

    struct ReplayStats
    {
        uint64_t sent_packets = 0;
        uint64_t sent_bytes = 0;
        uint64_t file_packets = 0;
        uint64_t filtered_packets = 0;
    };

    void print_usage()
    {
        std::cerr << "Usage: pcap_replay <pcap_file> [options]\n"
                  << "  --target <ip:port>       Target address (default: 127.0.0.1:9999)\n"
                  << "  --target-ip <ip>         Target IP only (port unchanged, default: 127.0.0.1)\n"
                  << "  --speed <factor>         Replay speed factor (default: 1.0, 0=as-fast-as-possible)\n"
                  << "  --rate-multiplier <N>    Alias for --speed\n"
                  << "  --loop                   Loop replay\n"
                  << "  --count <N>              Stop after N iterations\n"
                  << "  --start-time <epoch>     Replay packets after this epoch seconds\n"
                  << "  --end-time <epoch>       Replay packets before this epoch seconds\n";
    }

    bool parse_target(const std::string &text, std::string &ip, uint16_t &port)
    {
        const auto pos = text.rfind(':');
        if (pos == std::string::npos || pos == 0 || pos == (text.size() - 1))
        {
            return false;
        }

        const std::string port_text = text.substr(pos + 1);
        char *end = nullptr;
        const long parsed = std::strtol(port_text.c_str(), &end, 10);
        if (end == nullptr || *end != '\0' || parsed < 1 || parsed > 65535)
        {
            return false;
        }

        ip = text.substr(0, pos);
        port = static_cast<uint16_t>(parsed);
        return true;
    }

    bool parse_double(const std::string &text, double &value)
    {
        char *end = nullptr;
        value = std::strtod(text.c_str(), &end);
        return end != nullptr && *end == '\0';
    }

    bool parse_uint64(const std::string &text, uint64_t &value)
    {
        char *end = nullptr;
        const unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
        if (end == nullptr || *end != '\0')
        {
            return false;
        }
        value = static_cast<uint64_t>(parsed);
        return true;
    }

    bool parse_args(int argc, char **argv, Options &opt)
    {
        if (argc < 2)
        {
            return false;
        }

        opt.pcap_file = argv[1];
        for (int i = 2; i < argc; ++i)
        {
            const std::string arg = argv[i];
            if (arg == "--target")
            {
                if (i + 1 >= argc || !parse_target(argv[++i], opt.target_ip, opt.target_port))
                {
                    return false;
                }
            }
            else if (arg == "--speed" || arg == "--rate-multiplier")
            {
                if (i + 1 >= argc || !parse_double(argv[++i], opt.speed) || opt.speed < 0.0)
                {
                    return false;
                }
            }
            else if (arg == "--target-ip")
            {
                if (i + 1 >= argc)
                {
                    return false;
                }
                opt.target_ip = argv[++i];
            }
            else if (arg == "--loop")
            {
                opt.loop = true;
            }
            else if (arg == "--count")
            {
                uint64_t count = 0;
                if (i + 1 >= argc || !parse_uint64(argv[++i], count) || count == 0)
                {
                    return false;
                }
                opt.count = count;
            }
            else if (arg == "--start-time")
            {
                double epoch = 0.0;
                if (i + 1 >= argc || !parse_double(argv[++i], epoch))
                {
                    return false;
                }
                opt.start_time_epoch = epoch;
            }
            else if (arg == "--end-time")
            {
                double epoch = 0.0;
                if (i + 1 >= argc || !parse_double(argv[++i], epoch))
                {
                    return false;
                }
                opt.end_time_epoch = epoch;
            }
            else
            {
                return false;
            }
        }

        if (opt.start_time_epoch.has_value() && opt.end_time_epoch.has_value() &&
            opt.start_time_epoch.value() > opt.end_time_epoch.value())
        {
            return false;
        }
        return true;
    }

    uint16_t read_u16(const std::array<uint8_t, 24> &bytes, size_t offset, bool little_endian)
    {
        if (little_endian)
        {
            return static_cast<uint16_t>(bytes[offset]) |
                   static_cast<uint16_t>(static_cast<uint16_t>(bytes[offset + 1]) << 8u);
        }
        return static_cast<uint16_t>(static_cast<uint16_t>(bytes[offset]) << 8u) |
               static_cast<uint16_t>(bytes[offset + 1]);
    }

    uint32_t read_u32(const std::array<uint8_t, 24> &bytes, size_t offset, bool little_endian)
    {
        if (little_endian)
        {
            return static_cast<uint32_t>(bytes[offset]) |
                   (static_cast<uint32_t>(bytes[offset + 1]) << 8u) |
                   (static_cast<uint32_t>(bytes[offset + 2]) << 16u) |
                   (static_cast<uint32_t>(bytes[offset + 3]) << 24u);
        }
        return (static_cast<uint32_t>(bytes[offset]) << 24u) |
               (static_cast<uint32_t>(bytes[offset + 1]) << 16u) |
               (static_cast<uint32_t>(bytes[offset + 2]) << 8u) |
               static_cast<uint32_t>(bytes[offset + 3]);
    }

    bool read_global_header(std::ifstream &ifs, PcapFormat &format)
    {
        std::array<uint8_t, 24> header{};
        ifs.read(reinterpret_cast<char *>(header.data()), static_cast<std::streamsize>(header.size()));
        if (ifs.gcount() != static_cast<std::streamsize>(header.size()))
        {
            return false;
        }

        const uint32_t magic_le = read_u32(header, 0, true);
        if (magic_le == 0xA1B2C3D4u)
        {
            format.little_endian = true;
            format.timestamp_nanoseconds = false;
        }
        else if (magic_le == 0xA1B23C4Du)
        {
            format.little_endian = true;
            format.timestamp_nanoseconds = true;
        }
        else
        {
            const uint32_t magic_be = read_u32(header, 0, false);
            if (magic_be == 0xA1B2C3D4u)
            {
                format.little_endian = false;
                format.timestamp_nanoseconds = false;
            }
            else if (magic_be == 0xA1B23C4Du)
            {
                format.little_endian = false;
                format.timestamp_nanoseconds = true;
            }
            else
            {
                return false;
            }
        }

        const uint16_t version_major = read_u16(header, 4, format.little_endian);
        const uint16_t version_minor = read_u16(header, 6, format.little_endian);
        if (version_major != 2 || version_minor != 4)
        {
            return false;
        }

        format.snaplen = read_u32(header, 16, format.little_endian);
        format.network = read_u32(header, 20, format.little_endian);
        return true;
    }

    bool read_record(std::ifstream &ifs, const PcapFormat &format, Record &record)
    {
        std::array<uint8_t, 16> raw{};
        ifs.read(reinterpret_cast<char *>(raw.data()), static_cast<std::streamsize>(raw.size()));
        if (ifs.gcount() == 0)
        {
            return false;
        }
        if (ifs.gcount() != static_cast<std::streamsize>(raw.size()))
        {
            throw std::runtime_error("truncated pcap record header");
        }

        auto read_u32_raw = [&](size_t offset) -> uint32_t
        {
            if (format.little_endian)
            {
                return static_cast<uint32_t>(raw[offset]) |
                       (static_cast<uint32_t>(raw[offset + 1]) << 8u) |
                       (static_cast<uint32_t>(raw[offset + 2]) << 16u) |
                       (static_cast<uint32_t>(raw[offset + 3]) << 24u);
            }
            return (static_cast<uint32_t>(raw[offset]) << 24u) |
                   (static_cast<uint32_t>(raw[offset + 1]) << 16u) |
                   (static_cast<uint32_t>(raw[offset + 2]) << 8u) |
                   static_cast<uint32_t>(raw[offset + 3]);
        };

        record.ts_sec = read_u32_raw(0);
        record.ts_frac = read_u32_raw(4);
        const uint32_t incl_len = read_u32_raw(8);
        const uint32_t orig_len = read_u32_raw(12);
        if (incl_len == 0 || orig_len == 0 || incl_len > format.snaplen || incl_len > orig_len)
        {
            throw std::runtime_error("invalid pcap record lengths");
        }

        record.payload.resize(incl_len);
        ifs.read(reinterpret_cast<char *>(record.payload.data()), static_cast<std::streamsize>(incl_len));
        if (ifs.gcount() != static_cast<std::streamsize>(incl_len))
        {
            throw std::runtime_error("truncated pcap packet payload");
        }
        return true;
    }

    using SocketHandle = int;
    constexpr SocketHandle kInvalidSocket = -1;

    void close_socket(SocketHandle sock)
    {
        close(sock);
    }

    class SocketRuntime
    {
    public:
        bool init()
        {
            return true;
        }

        ~SocketRuntime() = default;
    };

    int send_payload(SocketHandle sock, const sockaddr_in &addr, const std::vector<uint8_t> &payload)
    {
        return static_cast<int>(sendto(sock,
                                       payload.data(),
                                       payload.size(),
                                       0,
                                       reinterpret_cast<const sockaddr *>(&addr),
                                       static_cast<socklen_t>(sizeof(addr))));
    }

    bool should_send(const Options &opt, double packet_epoch)
    {
        if (opt.start_time_epoch.has_value() && packet_epoch < opt.start_time_epoch.value())
        {
            return false;
        }
        if (opt.end_time_epoch.has_value() && packet_epoch > opt.end_time_epoch.value())
        {
            return false;
        }
        return true;
    }

    bool replay_once(const Options &opt, SocketHandle sock, const sockaddr_in &target_addr, ReplayStats &stats)
    {
        std::ifstream ifs(opt.pcap_file, std::ios::binary);
        if (!ifs.is_open())
        {
            std::cerr << "Failed to open PCAP file: " << opt.pcap_file << "\n";
            return false;
        }

        PcapFormat format{};
        if (!read_global_header(ifs, format))
        {
            std::cerr << "Invalid/unsupported PCAP global header in file: " << opt.pcap_file << "\n";
            return false;
        }

        std::optional<double> prev_packet_epoch{};
        const auto wall_start = std::chrono::steady_clock::now();
        auto window_start = wall_start;
        uint64_t window_bytes = 0;

        Record record{};
        while (read_record(ifs, format, record))
        {
            stats.file_packets += 1;
            const double epoch = record.epoch_seconds(format.timestamp_nanoseconds);
            if (!should_send(opt, epoch))
            {
                stats.filtered_packets += 1;
                continue;
            }

            if (opt.speed > 0.0 && prev_packet_epoch.has_value())
            {
                const double delta = epoch - prev_packet_epoch.value();
                if (delta > 0.0)
                {
                    const std::chrono::duration<double> sleep_for(delta / opt.speed);
                    std::this_thread::sleep_for(sleep_for);
                }
            }
            prev_packet_epoch = epoch;

            const int sent = send_payload(sock, target_addr, record.payload);
            if (sent < 0)
            {
                std::cerr << "sendto failed, skipping one packet\n";
                continue;
            }

            stats.sent_packets += 1;
            stats.sent_bytes += static_cast<uint64_t>(record.payload.size());
            window_bytes += static_cast<uint64_t>(record.payload.size());

            const auto now = std::chrono::steady_clock::now();
            const auto window_elapsed = std::chrono::duration<double>(now - window_start).count();
            if (window_elapsed >= 1.0)
            {
                const double rate_bps = static_cast<double>(window_bytes) / window_elapsed;
                std::cout << "[stats] sent_packets=" << stats.sent_packets
                          << " sent_bytes=" << stats.sent_bytes
                          << " current_rate_mbps=" << std::fixed << std::setprecision(3)
                          << (rate_bps * 8.0 / 1000000.0) << "\n";
                window_start = now;
                window_bytes = 0;
            }
        }

        const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - wall_start).count();
        const double avg_mbps = elapsed > 0.0
                                    ? (static_cast<double>(stats.sent_bytes) * 8.0 / elapsed / 1000000.0)
                                    : 0.0;
        std::cout << "[pass] file_packets=" << stats.file_packets
                  << " filtered_packets=" << stats.filtered_packets
                  << " sent_packets=" << stats.sent_packets
                  << " sent_bytes=" << stats.sent_bytes
                  << " avg_rate_mbps=" << std::fixed << std::setprecision(3) << avg_mbps
                  << "\n";
        return true;
    }

} // namespace

int main(int argc, char **argv)
{
    Options opt{};
    if (!parse_args(argc, argv, opt))
    {
        print_usage();
        return 1;
    }

    SocketRuntime socket_runtime;
    if (!socket_runtime.init())
    {
        std::cerr << "Failed to initialize socket runtime\n";
        return 1;
    }

    const SocketHandle sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == kInvalidSocket)
    {
        std::cerr << "Failed to create UDP socket\n";
        return 1;
    }

    sockaddr_in target_addr{};
    target_addr.sin_family = AF_INET;
    target_addr.sin_port = htons(opt.target_port);
    if (inet_pton(AF_INET, opt.target_ip.c_str(), &target_addr.sin_addr) != 1)
    {
        std::cerr << "Invalid target IP: " << opt.target_ip << "\n";
        close_socket(sock);
        return 1;
    }

    const uint64_t iterations_target = opt.count.value_or(opt.loop ? 0ull : 1ull);
    uint64_t iteration = 0;
    ReplayStats total{};
    const auto replay_wall_start = std::chrono::steady_clock::now();
    while (iterations_target == 0 || iteration < iterations_target)
    {
        ReplayStats per_pass{};
        if (!replay_once(opt, sock, target_addr, per_pass))
        {
            close_socket(sock);
            return 1;
        }
        total.sent_packets += per_pass.sent_packets;
        total.sent_bytes += per_pass.sent_bytes;
        total.file_packets += per_pass.file_packets;
        total.filtered_packets += per_pass.filtered_packets;
        iteration += 1;

        if (!opt.loop && !opt.count.has_value())
        {
            break;
        }
    }

    close_socket(sock);

    const auto total_elapsed = std::chrono::duration<double>(
                                   std::chrono::steady_clock::now() - replay_wall_start)
                                   .count();
    const double total_avg_pps = total_elapsed > 0.0
                                     ? static_cast<double>(total.sent_packets) / total_elapsed
                                     : 0.0;
    const double total_avg_mbps = total_elapsed > 0.0
                                      ? (static_cast<double>(total.sent_bytes) * 8.0 / total_elapsed / 1000000.0)
                                      : 0.0;

    std::cout << "[done] iterations=" << iteration
              << " total_file_packets=" << total.file_packets
              << " total_filtered_packets=" << total.filtered_packets
              << " total_sent_packets=" << total.sent_packets
              << " total_sent_bytes=" << total.sent_bytes
              << " elapsed_sec=" << std::fixed << std::setprecision(3) << total_elapsed
              << " avg_pps=" << std::fixed << std::setprecision(1) << total_avg_pps
              << " avg_mbps=" << std::fixed << std::setprecision(3) << total_avg_mbps
              << "\n";
    return 0;
}
