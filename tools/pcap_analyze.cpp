#include "qdgz300/m01_receiver/protocol/packet_parser.h"
#include "qdgz300/m01_receiver/protocol/protocol_types.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    struct Options
    {
        std::string pcap_file;
        std::optional<std::string> output_json;
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

    struct DelayStats
    {
        double min = 0.0;
        double max = 0.0;
        double mean = 0.0;
        double p50 = 0.0;
        double p99 = 0.0;
    };

    struct TypeCounter
    {
        uint64_t packets = 0;
        uint64_t bytes = 0;
    };

    struct Report
    {
        std::string file;
        uint64_t total_packets = 0;
        uint64_t total_bytes = 0;
        double duration_seconds = 0.0;
        std::map<std::string, uint64_t> packet_types;
        std::map<std::string, uint64_t> packet_type_bytes;
        std::map<std::string, uint64_t> source_ids;
        uint64_t sequence_gaps = 0;
        uint64_t sequence_wraparounds = 0;
        uint64_t invalid_packets = 0;
        DelayStats inter_packet_delay_ms{};
    };

    void print_usage()
    {
        std::cerr << "Usage: pcap_analyze <pcap_file> [--output <json_file>]\n";
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
            if (arg == "--output")
            {
                if (i + 1 >= argc)
                {
                    return false;
                }
                opt.output_json = std::string(argv[++i]);
            }
            else
            {
                return false;
            }
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

    std::string packet_type_label(uint8_t packet_type)
    {
        using receiver::protocol::PacketType;
        const std::ostringstream hex_name = [&]() -> std::ostringstream
        {
            std::ostringstream os;
            os << "UNKNOWN(0x" << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
               << static_cast<unsigned>(packet_type) << ")";
            return os;
        }();

        switch (static_cast<PacketType>(packet_type))
        {
        case PacketType::CONTROL:
            return "CONTROL(0x01)";
        case PacketType::ACK:
            return "ACK(0x02)";
        case PacketType::DATA:
            return "DATA(0x03)";
        case PacketType::HEARTBEAT:
            return "HEARTBEAT(0x04)";
        case PacketType::RMA:
            return "RMA(0xFF)";
        default:
            return hex_name.str();
        }
    }

    std::string source_id_label(uint8_t source_id)
    {
        std::ostringstream os;
        os << "0x" << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
           << static_cast<unsigned>(source_id);
        return os.str();
    }

    double percentile_sorted(const std::vector<double> &sorted, double p)
    {
        if (sorted.empty())
        {
            return 0.0;
        }
        const double pos = p * static_cast<double>(sorted.size() - 1);
        const size_t idx = static_cast<size_t>(pos);
        const size_t next = std::min(idx + 1, sorted.size() - 1);
        const double frac = pos - static_cast<double>(idx);
        return sorted[idx] * (1.0 - frac) + sorted[next] * frac;
    }

    DelayStats compute_delay_stats(std::vector<double> delays_ms)
    {
        DelayStats stats{};
        if (delays_ms.empty())
        {
            return stats;
        }
        std::sort(delays_ms.begin(), delays_ms.end());
        stats.min = delays_ms.front();
        stats.max = delays_ms.back();

        double sum = 0.0;
        for (const double v : delays_ms)
        {
            sum += v;
        }
        stats.mean = sum / static_cast<double>(delays_ms.size());
        stats.p50 = percentile_sorted(delays_ms, 0.50);
        stats.p99 = percentile_sorted(delays_ms, 0.99);
        return stats;
    }

    std::string escape_json(const std::string &s)
    {
        std::ostringstream os;
        for (const char ch : s)
        {
            switch (ch)
            {
            case '\\':
                os << "\\\\";
                break;
            case '"':
                os << "\\\"";
                break;
            case '\n':
                os << "\\n";
                break;
            case '\r':
                os << "\\r";
                break;
            case '\t':
                os << "\\t";
                break;
            default:
                os << ch;
                break;
            }
        }
        return os.str();
    }

    template <typename MapT>
    void append_json_map(std::ostringstream &os, const MapT &m, int indent_spaces)
    {
        os << "{";
        if (!m.empty())
        {
            os << "\n";
        }

        size_t idx = 0;
        for (const auto &[key, value] : m)
        {
            os << std::string(static_cast<size_t>(indent_spaces), ' ')
               << "\"" << escape_json(key) << "\": " << value;
            idx += 1;
            if (idx != m.size())
            {
                os << ",";
            }
            os << "\n";
        }
        if (!m.empty())
        {
            os << std::string(static_cast<size_t>(indent_spaces - 2), ' ');
        }
        os << "}";
    }

    std::string build_json(const Report &report)
    {
        std::ostringstream os;
        os << std::fixed << std::setprecision(6);
        os << "{\n";
        os << "  \"file\": \"" << escape_json(report.file) << "\",\n";
        os << "  \"total_packets\": " << report.total_packets << ",\n";
        os << "  \"total_bytes\": " << report.total_bytes << ",\n";
        os << "  \"duration_seconds\": " << report.duration_seconds << ",\n";

        os << "  \"packet_types\": ";
        append_json_map(os, report.packet_types, 4);
        os << ",\n";

        os << "  \"packet_type_bytes\": ";
        append_json_map(os, report.packet_type_bytes, 4);
        os << ",\n";

        os << "  \"source_ids\": ";
        append_json_map(os, report.source_ids, 4);
        os << ",\n";

        os << "  \"sequence_gaps\": " << report.sequence_gaps << ",\n";
        os << "  \"sequence_wraparounds\": " << report.sequence_wraparounds << ",\n";
        os << "  \"invalid_packets\": " << report.invalid_packets << ",\n";
        os << "  \"inter_packet_delay_ms\": {\n";
        os << "    \"min\": " << report.inter_packet_delay_ms.min << ",\n";
        os << "    \"max\": " << report.inter_packet_delay_ms.max << ",\n";
        os << "    \"mean\": " << report.inter_packet_delay_ms.mean << ",\n";
        os << "    \"p50\": " << report.inter_packet_delay_ms.p50 << ",\n";
        os << "    \"p99\": " << report.inter_packet_delay_ms.p99 << "\n";
        os << "  }\n";
        os << "}\n";
        return os.str();
    }

    bool analyze_file(const Options &opt, Report &report)
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

        receiver::protocol::PacketParser parser;
        std::optional<double> first_ts{};
        std::optional<double> last_ts{};
        std::optional<double> prev_ts{};
        std::optional<uint32_t> prev_seq{};
        std::map<std::string, TypeCounter> packet_type_counters;
        std::vector<double> delays_ms;

        Record record{};
        while (read_record(ifs, format, record))
        {
            const double ts = record.epoch_seconds(format.timestamp_nanoseconds);
            if (!first_ts.has_value())
            {
                first_ts = ts;
            }
            if (prev_ts.has_value())
            {
                delays_ms.push_back((ts - prev_ts.value()) * 1000.0);
            }
            prev_ts = ts;
            last_ts = ts;

            report.total_packets += 1;
            report.total_bytes += static_cast<uint64_t>(record.payload.size());

            auto parsed = parser.parse(record.payload.data(), record.payload.size());
            if (!parsed.has_value())
            {
                report.invalid_packets += 1;
                continue;
            }

            const auto &header = parsed->header;
            const std::string type_key = packet_type_label(header.packet_type);
            auto &type_counter = packet_type_counters[type_key];
            type_counter.packets += 1;
            type_counter.bytes += static_cast<uint64_t>(record.payload.size());

            const std::string src_key = source_id_label(header.source_id);
            report.source_ids[src_key] += 1;

            if (prev_seq.has_value())
            {
                if (prev_seq.value() > header.sequence_number)
                {
                    const uint32_t back_delta = prev_seq.value() - header.sequence_number;
                    if (back_delta > 0x80000000u)
                    {
                        report.sequence_wraparounds += 1;
                    }
                }
                else
                {
                    const uint32_t forward_delta = header.sequence_number - prev_seq.value();
                    if (forward_delta > 1u)
                    {
                        report.sequence_gaps += 1;
                    }
                }
            }
            prev_seq = header.sequence_number;
        }

        for (const auto &[k, v] : packet_type_counters)
        {
            report.packet_types[k] = v.packets;
            report.packet_type_bytes[k] = v.bytes;
        }

        if (first_ts.has_value() && last_ts.has_value() && last_ts.value() >= first_ts.value())
        {
            report.duration_seconds = last_ts.value() - first_ts.value();
        }
        report.inter_packet_delay_ms = compute_delay_stats(std::move(delays_ms));
        report.file = opt.pcap_file;
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

    Report report{};
    try
    {
        if (!analyze_file(opt, report))
        {
            return 1;
        }
    }
    catch (const std::exception &ex)
    {
        std::cerr << "Analyze failed: " << ex.what() << "\n";
        return 1;
    }

    const std::string json = build_json(report);
    if (opt.output_json.has_value())
    {
        std::ofstream ofs(opt.output_json.value(), std::ios::binary | std::ios::trunc);
        if (!ofs.is_open())
        {
            std::cerr << "Failed to write output file: " << opt.output_json.value() << "\n";
            return 1;
        }
        ofs << json;
    }
    else
    {
        std::cout << json;
    }

    return 0;
}
