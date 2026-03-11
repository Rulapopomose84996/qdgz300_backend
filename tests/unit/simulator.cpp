#include "qdgz300/m01_receiver/protocol/heartbeat_builder.h"
#include "qdgz300/m01_receiver/protocol/payload_codec.h"
#include "qdgz300/m01_receiver/protocol/protocol_types.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>
#include <limits>
#include <memory>
#include <queue>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

using receiver::protocol::CommonHeader;
using receiver::protocol::ControlBeamItem;
using receiver::protocol::ControlPayload;
using receiver::protocol::DataSpecificHeader;
using receiver::protocol::ExecutionSnapshot;
using receiver::protocol::HeartbeatBuilder;
using receiver::protocol::HeartbeatInput;
using receiver::protocol::PacketType;
using receiver::protocol::PayloadCodec;
using receiver::protocol::PROTOCOL_MAGIC;
using receiver::protocol::PROTOCOL_VERSION;
using receiver::protocol::RmaPayload;
using receiver::protocol::ScheduleApplyAckPayload;

namespace
{
    struct SimulatorOptions
    {
        std::string mode{"data-only"};
        double pps{10000.0};
        double duration_seconds{10.0};
        std::string target_ip{"127.0.0.1"};
        uint16_t target_port{9999};
        double loss_rate{0.05};
        double reorder_rate{0.1};

        uint16_t min_total_frags{1};
        uint16_t max_total_frags{16};
        uint32_t frame_counter_start{0};
        uint16_t control_beams_per_frame{4};
        uint32_t heartbeat_interval_ms{1000};
    };

    struct RunStatistics
    {
        uint64_t generated{0};
        uint64_t sent{0};
        uint64_t dropped_by_loss{0};
        uint64_t delayed_for_reorder{0};
        uint64_t send_errors{0};

        uint64_t data_packets{0};
        uint64_t control_packets{0};
        uint64_t ack_packets{0};
        uint64_t heartbeat_packets{0};
        uint64_t rma_packets{0};
    };

    struct DelayedPacket
    {
        std::chrono::steady_clock::time_point due;
        std::vector<uint8_t> bytes;
    };

    struct DelayedPacketLater
    {
        bool operator()(const DelayedPacket &lhs, const DelayedPacket &rhs) const
        {
            return lhs.due > rhs.due;
        }
    };

    void print_usage(const char *prog)
    {
        std::cout << "Usage: " << prog << " [options]\n";
        std::cout << "  --mode <data-only|mixed|burst|lossy|reorder>\n";
        std::cout << "  --pps <packets_per_second> (default: 10000)\n";
        std::cout << "  --duration <seconds> (default: 10)\n";
        std::cout << "  --target <ip:port> (default: 127.0.0.1:9999)\n";
        std::cout << "  --loss-rate <0..1> (lossy mode, default: 0.05)\n";
        std::cout << "  --reorder-rate <0..1> (reorder mode, default: 0.1)\n";
        std::cout << "\nExamples:\n";
        std::cout << "  " << prog << " --mode data-only --pps 20000 --duration 10\n";
        std::cout << "  " << prog << " --mode mixed --target 127.0.0.1:9999 --duration 30\n";
        std::cout << "  " << prog << " --mode lossy --loss-rate 0.05\n";
        std::cout << "  " << prog << " --mode reorder --reorder-rate 0.10\n";
    }

    bool parse_target(const std::string &target, std::string &ip, uint16_t &port)
    {
        const size_t sep = target.rfind(':');
        if (sep == std::string::npos)
        {
            return false;
        }

        const std::string ip_part = target.substr(0, sep);
        const std::string port_part = target.substr(sep + 1);
        if (ip_part.empty() || port_part.empty())
        {
            return false;
        }

        const unsigned long parsed_port = std::stoul(port_part);
        if (parsed_port > std::numeric_limits<uint16_t>::max())
        {
            return false;
        }

        ip = ip_part;
        port = static_cast<uint16_t>(parsed_port);
        return true;
    }

    bool parse_mode(const std::string &mode)
    {
        return mode == "data-only" ||
               mode == "mixed" ||
               mode == "burst" ||
               mode == "lossy" ||
               mode == "reorder";
    }

    bool parse_args(int argc, char **argv, SimulatorOptions &opts)
    {
        for (int i = 1; i < argc; ++i)
        {
            const std::string arg = argv[i];
            if (arg == "--help" || arg == "-h")
            {
                print_usage(argv[0]);
                return false;
            }

            auto require_value = [&](std::string_view flag) -> const char *
            {
                if (i + 1 >= argc)
                {
                    throw std::runtime_error("Missing value for flag " + std::string(flag));
                }
                return argv[++i];
            };

            if (arg == "--mode")
            {
                opts.mode = require_value("--mode");
                if (!parse_mode(opts.mode))
                {
                    throw std::runtime_error("Unsupported mode: " + opts.mode);
                }
                continue;
            }

            if (arg == "--pps")
            {
                opts.pps = std::stod(require_value("--pps"));
                if (opts.pps <= 0.0)
                {
                    throw std::runtime_error("--pps must be > 0");
                }
                continue;
            }

            if (arg == "--duration")
            {
                opts.duration_seconds = std::stod(require_value("--duration"));
                if (opts.duration_seconds <= 0.0)
                {
                    throw std::runtime_error("--duration must be > 0");
                }
                continue;
            }

            if (arg == "--target")
            {
                const std::string target = require_value("--target");
                if (!parse_target(target, opts.target_ip, opts.target_port))
                {
                    throw std::runtime_error("Invalid --target format, expected ip:port");
                }
                continue;
            }

            if (arg == "--loss-rate")
            {
                opts.loss_rate = std::stod(require_value("--loss-rate"));
                if (opts.loss_rate < 0.0 || opts.loss_rate > 1.0)
                {
                    throw std::runtime_error("--loss-rate must be in [0,1]");
                }
                continue;
            }

            if (arg == "--reorder-rate")
            {
                opts.reorder_rate = std::stod(require_value("--reorder-rate"));
                if (opts.reorder_rate < 0.0 || opts.reorder_rate > 1.0)
                {
                    throw std::runtime_error("--reorder-rate must be in [0,1]");
                }
                continue;
            }

            throw std::runtime_error("Unknown argument: " + arg);
        }

        return true;
    }

    uint64_t now_ms()
    {
        const auto now = std::chrono::system_clock::now();
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
    }

    class UdpSimulator
    {
    public:
        explicit UdpSimulator(const SimulatorOptions &options)
            : options_(options),
              rng_(20260302u),
              unit_dist_(0.0, 1.0),
              non_tail_payload_dist_(128, 1024),
              tail_payload_dist_(64, 512),
              total_frags_dist_(options_.min_total_frags, options_.max_total_frags),
              reorder_delay_ms_dist_(2, 30),
              frame_counter_(options_.frame_counter_start)
        {
            sockfd_ = socket(AF_INET, SOCK_DGRAM, 0);
            if (sockfd_ < 0)
            {
                throw std::runtime_error("Failed to create UDP socket");
            }

            std::memset(&target_addr_, 0, sizeof(target_addr_));
            target_addr_.sin_family = AF_INET;
            target_addr_.sin_port = htons(options_.target_port);
            if (inet_pton(AF_INET, options_.target_ip.c_str(), &target_addr_.sin_addr) != 1)
            {
                throw std::runtime_error("Invalid target IP: " + options_.target_ip);
            }

            select_new_frame_shape();
        }

        ~UdpSimulator()
        {
            if (sockfd_ >= 0)
            {
                close(sockfd_);
            }
        }

        RunStatistics run()
        {
            const auto start = std::chrono::steady_clock::now();
            const auto end = start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                           std::chrono::duration<double>(options_.duration_seconds));
            const auto interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(1.0 / options_.pps));
            auto next_tick = start;
            auto last_heartbeat = start;

            while (std::chrono::steady_clock::now() < end)
            {
                std::this_thread::sleep_until(next_tick);
                const auto now = std::chrono::steady_clock::now();
                flush_due_reordered(now, false);

                const int emit_count = burst_emit_count(now, start);
                for (int i = 0; i < emit_count; ++i)
                {
                    auto packet = next_packet(now, last_heartbeat);
                    if (packet.empty())
                    {
                        continue;
                    }
                    process_packet(std::move(packet), now);
                }

                next_tick += interval;
            }

            flush_due_reordered(std::chrono::steady_clock::now(), true);
            return stats_;
        }

    private:
        int burst_emit_count(std::chrono::steady_clock::time_point now,
                             std::chrono::steady_clock::time_point start) const
        {
            if (options_.mode != "burst")
            {
                return 1;
            }

            const std::chrono::duration<double> elapsed = now - start;
            const double phase = std::fmod(elapsed.count(), 1.0);
            if (phase < 0.5)
            {
                return 2;
            }
            return 0;
        }

        std::vector<uint8_t> next_packet(std::chrono::steady_clock::time_point now,
                                         std::chrono::steady_clock::time_point &last_heartbeat)
        {
            if (!rma_queue_.empty())
            {
                auto packet = std::move(rma_queue_.front());
                rma_queue_.pop_front();
                return packet;
            }

            if (!ack_queue_.empty())
            {
                auto packet = std::move(ack_queue_.front());
                ack_queue_.pop_front();
                return packet;
            }

            if (options_.mode == "mixed")
            {
                const double pick = unit_dist_(rng_);
                if (pick < 0.95)
                {
                    return build_data_packet();
                }
                if (pick < 0.98)
                {
                    const auto since_last = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_heartbeat).count();
                    if (since_last >= static_cast<int64_t>(options_.heartbeat_interval_ms))
                    {
                        last_heartbeat = now;
                        return build_heartbeat_packet();
                    }
                    return build_data_packet();
                }
                if (pick < 0.995)
                {
                    auto control = build_control_packet();
                    ack_queue_.push_back(build_ack_packet());
                    return control;
                }

                queue_rma_session();
                if (!rma_queue_.empty())
                {
                    auto first = std::move(rma_queue_.front());
                    rma_queue_.pop_front();
                    return first;
                }
                return build_data_packet();
            }

            return build_data_packet();
        }

        void process_packet(std::vector<uint8_t> packet, std::chrono::steady_clock::time_point now)
        {
            ++stats_.generated;

            if (options_.mode == "lossy" && should_drop_loss())
            {
                ++stats_.dropped_by_loss;
                return;
            }

            if (options_.mode == "reorder" && should_reorder())
            {
                DelayedPacket delayed;
                delayed.due = now + std::chrono::milliseconds(reorder_delay_ms_dist_(rng_));
                delayed.bytes = std::move(packet);
                reorder_heap_.push(std::move(delayed));
                ++stats_.delayed_for_reorder;
                return;
            }

            send_packet(packet);
        }

        bool should_drop_loss()
        {
            return unit_dist_(rng_) < options_.loss_rate;
        }

        bool should_reorder()
        {
            return unit_dist_(rng_) < options_.reorder_rate;
        }

        void flush_due_reordered(std::chrono::steady_clock::time_point now, bool force)
        {
            while (!reorder_heap_.empty())
            {
                if (!force && reorder_heap_.top().due > now)
                {
                    break;
                }

                std::vector<uint8_t> packet = reorder_heap_.top().bytes;
                reorder_heap_.pop();
                send_packet(packet);
            }
        }

        uint32_t allocate_sequence()
        {
            return next_sequence_++;
        }

        void fill_common_header(CommonHeader &header, PacketType type, uint32_t seq, uint16_t payload_len)
        {
            header.magic = PROTOCOL_MAGIC;
            header.sequence_number = seq;
            header.timestamp = now_ms();
            header.payload_len = payload_len;
            header.packet_type = static_cast<uint8_t>(type);
            header.protocol_version = PROTOCOL_VERSION;
            header.source_id = static_cast<uint8_t>(receiver::protocol::DeviceID::DACS_01);
            header.dest_id = static_cast<uint8_t>(receiver::protocol::DeviceID::SPS);
            header.control_epoch = control_epoch_;
            header.ext_flags = 0;
            header.reserved1 = 0;
            header.reserved2 = 0;
            std::memset(header.reserved3, 0, sizeof(header.reserved3));
        }

        std::vector<uint8_t> finalize_packet(PacketType type, const std::vector<uint8_t> &payload)
        {
            CommonHeader header{};
            const uint32_t sequence = allocate_sequence();
            fill_common_header(header, type, sequence, static_cast<uint16_t>(payload.size()));

            std::vector<uint8_t> packet(sizeof(CommonHeader) + payload.size(), 0);
            std::memcpy(packet.data(), &header, sizeof(header));
            if (!payload.empty())
            {
                std::memcpy(packet.data() + sizeof(CommonHeader), payload.data(), payload.size());
            }

            switch (type)
            {
            case PacketType::DATA:
                ++stats_.data_packets;
                break;
            case PacketType::CONTROL:
                ++stats_.control_packets;
                break;
            case PacketType::ACK:
                ++stats_.ack_packets;
                break;
            case PacketType::HEARTBEAT:
                ++stats_.heartbeat_packets;
                break;
            case PacketType::RMA:
                ++stats_.rma_packets;
                break;
            }

            return packet;
        }

        void select_new_frame_shape()
        {
            current_total_frags_ = std::max<uint16_t>(1, total_frags_dist_(rng_));
            current_frag_index_ = 0;
            current_non_tail_payload_bytes_ = non_tail_payload_dist_(rng_);
            current_tail_payload_bytes_ = tail_payload_dist_(rng_);
        }

        std::vector<uint8_t> build_data_packet()
        {
            if (current_frag_index_ >= current_total_frags_)
            {
                ++frame_counter_;
                select_new_frame_shape();
            }

            const bool is_tail = (current_frag_index_ + 1 == current_total_frags_);
            const size_t raw_payload_size = is_tail ? current_tail_payload_bytes_ : current_non_tail_payload_bytes_;
            const size_t specific_size = sizeof(DataSpecificHeader);
            const size_t snapshot_size = is_tail ? sizeof(ExecutionSnapshot) : 0;
            const size_t payload_size = specific_size + snapshot_size + raw_payload_size;

            std::vector<uint8_t> payload(payload_size, 0);

            DataSpecificHeader specific{};
            specific.frame_counter = frame_counter_;
            specific.cpi_count = frame_counter_ / 2;
            specific.pulse_index = current_frag_index_;
            specific.sample_offset = static_cast<uint32_t>(current_frag_index_) * 256u;
            specific.sample_count = 256;
            specific.data_timestamp = now_ms();
            specific.health_summary = 0;
            specific.set_channel_mask_data_type_compat(0x0001u, 0x00u);
            specific.beam_id = static_cast<uint16_t>((frame_counter_ % 32u) + 1u);
            specific.frag_index = current_frag_index_;
            specific.total_frags = current_total_frags_;
            specific.tail_frag_payload_bytes = static_cast<uint16_t>(current_tail_payload_bytes_);
            std::memcpy(payload.data(), &specific, sizeof(specific));

            size_t offset = sizeof(DataSpecificHeader);
            if (is_tail)
            {
                ExecutionSnapshot snapshot{};
                snapshot.work_freq_index = static_cast<uint8_t>((frame_counter_ % 8u) + 1u);
                snapshot.mgc_gain = 100;
                snapshot.signal_bandwidth = 25;
                snapshot.pulse_width = 12;
                snapshot.pulse_period = 50;
                std::memcpy(payload.data() + offset, &snapshot, sizeof(snapshot));
                offset += sizeof(snapshot);
            }

            for (size_t i = 0; i < raw_payload_size; ++i)
            {
                payload[offset + i] = static_cast<uint8_t>((frame_counter_ + current_frag_index_ + i) & 0xFFu);
            }

            ++current_frag_index_;
            if (current_frag_index_ >= current_total_frags_)
            {
                ++frame_counter_;
                select_new_frame_shape();
            }

            return finalize_packet(PacketType::DATA, payload);
        }

        std::vector<uint8_t> build_control_packet()
        {
            ControlPayload control{};
            control.table_header.frame_counter = frame_counter_;
            control.table_header.frame_beam_total = options_.control_beams_per_frame;
            control.table_header.beams_per_second = options_.control_beams_per_frame;
            control.table_header.cpi_count_base = frame_counter_ * 4u;

            control.beam_items.resize(options_.control_beams_per_frame);
            for (uint16_t i = 0; i < options_.control_beams_per_frame; ++i)
            {
                ControlBeamItem &beam = control.beam_items[i];
                beam.beam_id = static_cast<uint16_t>(i + 1);
                beam.beam_work_status = 1;
                beam.antenna_broadening_sel = 0;
                beam.azimuth_angle = static_cast<int16_t>((i * 3) % 90);
                beam.elevation_angle = static_cast<int16_t>((i * 2) % 45);
                beam.work_freq_index = static_cast<uint8_t>((i % 8) + 1);
                beam.mgc_gain = 100;
                beam.period_combo_mode = 2;
                beam.long_code_waveform = 1;
                beam.short_code_waveform = 1;
                beam.signal_bandwidth = 25;
                beam.pulse_width = 12;
                beam.pulse_period = 50;
                beam.accumulation_count = 1;
                beam.sim_target_range = 1000;
                beam.sim_target_velocity = 0;
                beam.short_code_sample_count = 128;
                beam.long_code_sample_count = 128;
                beam.data_rate_mbps = 20;
                beam.reserved = 0;
            }

            std::vector<uint8_t> payload;
            if (!PayloadCodec::encode_control_payload(control, payload))
            {
                throw std::runtime_error("encode_control_payload failed");
            }
            return finalize_packet(PacketType::CONTROL, payload);
        }

        std::vector<uint8_t> build_ack_packet()
        {
            ScheduleApplyAckPayload ack{};
            ack.ack_frame_counter = frame_counter_;
            ack.acked_sequence_number = (next_sequence_ > 0) ? (next_sequence_ - 1) : 0;
            ack.ack_result = 0;
            ack.error_code = 0;
            ack.error_detail = 0;
            ack.applied_timestamp = now_ms();

            std::vector<uint8_t> payload;
            if (!PayloadCodec::encode_schedule_apply_ack_payload(ack, payload))
            {
                throw std::runtime_error("encode_schedule_apply_ack_payload failed");
            }
            return finalize_packet(PacketType::ACK, payload);
        }

        std::vector<uint8_t> build_heartbeat_packet()
        {
            HeartbeatInput input{};
            input.system_status_alive = 1;
            input.system_status_state = 2;
            input.core_temp = 680;
            input.op_mode = 3;
            input.time_coord_source = 1;
            input.device_number[0] = 0x11;
            input.device_number[1] = 0x22;
            input.device_number[2] = 0x33;
            input.device_number[3] = 0x44;
            input.time_offset = 0;
            input.error_counters[0] = 0;
            input.error_counters[1] = 1;
            input.error_counters[2] = 0;
            input.error_counters[3] = 0;
            input.lo_lock_flags = 0x03;
            input.supply_current = 25;
            input.tr_comm_status = 1;
            input.voltage_stability_flags = 0x0003;
            input.panel_temp = 420;

            std::vector<uint8_t> payload;
            if (!HeartbeatBuilder::build_payload(input, payload))
            {
                throw std::runtime_error("HeartbeatBuilder::build_payload failed");
            }
            return finalize_packet(PacketType::HEARTBEAT, payload);
        }

        std::vector<uint8_t> build_rma_packet(uint32_t session_id,
                                              uint64_t session_token,
                                              uint8_t cmd_type,
                                              uint16_t cmd_seq,
                                              const std::vector<uint8_t> &body)
        {
            RmaPayload payload_struct{};
            payload_struct.header.session_id = session_id;
            payload_struct.header.session_token = session_token;
            payload_struct.header.cmd_type = cmd_type;
            payload_struct.header.cmd_flags = 0;
            payload_struct.header.cmd_seq = cmd_seq;
            payload_struct.body = body;

            std::vector<uint8_t> payload;
            if (!PayloadCodec::encode_rma_payload(payload_struct, payload))
            {
                throw std::runtime_error("encode_rma_payload failed");
            }
            return finalize_packet(PacketType::RMA, payload);
        }

        void queue_rma_session()
        {
            const uint32_t session_id = static_cast<uint32_t>(rng_());
            const uint64_t token_hi = static_cast<uint64_t>(rng_());
            const uint64_t token_lo = static_cast<uint64_t>(rng_());
            const uint64_t session_token = (token_hi << 32u) | token_lo;

            uint16_t cmd_seq = 1;
            rma_queue_.push_back(build_rma_packet(session_id, session_token, 0x01, cmd_seq++, {}));

            for (size_t chunk = 0; chunk < 3; ++chunk)
            {
                std::vector<uint8_t> chunk_body(256, 0);
                for (size_t i = 0; i < chunk_body.size(); ++i)
                {
                    chunk_body[i] = static_cast<uint8_t>((chunk * 31u + i) & 0xFFu);
                }
                rma_queue_.push_back(build_rma_packet(session_id, session_token, 0x12, cmd_seq++, chunk_body));
            }

            rma_queue_.push_back(build_rma_packet(session_id, session_token, 0x7F, cmd_seq, {}));
        }

        void send_packet(const std::vector<uint8_t> &packet)
        {
            const ssize_t sent = sendto(sockfd_,
                                        packet.data(),
                                        packet.size(),
                                        0,
                                        reinterpret_cast<sockaddr *>(&target_addr_),
                                        sizeof(target_addr_));
            if (sent == static_cast<ssize_t>(packet.size()))
            {
                ++stats_.sent;
            }
            else
            {
                ++stats_.send_errors;
            }
        }

        SimulatorOptions options_;
        int sockfd_{-1};
        sockaddr_in target_addr_{};

        std::mt19937 rng_;
        std::uniform_real_distribution<double> unit_dist_;
        std::uniform_int_distribution<size_t> non_tail_payload_dist_;
        std::uniform_int_distribution<size_t> tail_payload_dist_;
        std::uniform_int_distribution<uint16_t> total_frags_dist_;
        std::uniform_int_distribution<int> reorder_delay_ms_dist_;

        std::priority_queue<DelayedPacket, std::vector<DelayedPacket>, DelayedPacketLater> reorder_heap_;
        std::deque<std::vector<uint8_t>> rma_queue_;
        std::deque<std::vector<uint8_t>> ack_queue_;

        uint32_t next_sequence_{0};
        uint16_t control_epoch_{1};

        uint32_t frame_counter_{0};
        uint16_t current_total_frags_{1};
        uint16_t current_frag_index_{0};
        size_t current_non_tail_payload_bytes_{256};
        size_t current_tail_payload_bytes_{128};

        RunStatistics stats_{};
    };

    void print_summary(const SimulatorOptions &opts, const RunStatistics &stats)
    {
        std::cout << "mode=" << opts.mode
                  << " target=" << opts.target_ip << ':' << opts.target_port
                  << " pps=" << opts.pps
                  << " duration=" << opts.duration_seconds << "s\n";

        std::cout << "generated=" << stats.generated
                  << " sent=" << stats.sent
                  << " dropped(loss)=" << stats.dropped_by_loss
                  << " delayed(reorder)=" << stats.delayed_for_reorder
                  << " send_errors=" << stats.send_errors << "\n";

        std::cout << "type_breakdown: "
                  << "data=" << stats.data_packets << " "
                  << "control=" << stats.control_packets << " "
                  << "ack=" << stats.ack_packets << " "
                  << "heartbeat=" << stats.heartbeat_packets << " "
                  << "rma=" << stats.rma_packets << "\n";
    }
} // namespace

int main(int argc, char **argv)
{
    try
    {
        SimulatorOptions options;
        if (!parse_args(argc, argv, options))
        {
            return 0;
        }

        UdpSimulator simulator(options);
        const RunStatistics stats = simulator.run();
        print_summary(options, stats);
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << "simulator error: " << e.what() << "\n";
        return 1;
    }
}
