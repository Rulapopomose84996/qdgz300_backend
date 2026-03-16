#include "packet_generator.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <numeric>

namespace fpga_emulator
{

    // ─── Legacy constructor (single-frag data packets) ──────────────────────────
    PacketGenerator::PacketGenerator(size_t payload_bytes)
    {
        config_.source_id = static_cast<uint8_t>(receiver::protocol::DeviceID::DACS_01);
        config_.dest_id = static_cast<uint8_t>(receiver::protocol::DeviceID::SPS);
        config_.cpi_frags = 1;
        config_.frag_payload_bytes =
            (payload_bytes < sizeof(receiver::protocol::DataSpecificHeader))
                ? sizeof(receiver::protocol::DataSpecificHeader)
                : payload_bytes;
    }

    // ─── Full-config constructor ────────────────────────────────────────────────
    PacketGenerator::PacketGenerator(const GeneratorConfig &config)
        : config_(config)
    {
    }

    uint64_t PacketGenerator::now_ms() const
    {
        const auto now = std::chrono::system_clock::now();
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
    }

    void PacketGenerator::fill_common_header(uint8_t *buf, uint16_t payload_len, uint8_t packet_type)
    {
        using receiver::protocol::CommonHeader;
        using receiver::protocol::PROTOCOL_MAGIC;
        using receiver::protocol::PROTOCOL_VERSION;

        CommonHeader h{};
        h.magic = PROTOCOL_MAGIC;
        h.sequence_number = next_sequence_++;
        h.timestamp = now_ms();
        h.payload_len = payload_len;
        h.packet_type = packet_type;
        h.protocol_version = PROTOCOL_VERSION;
        h.source_id = config_.source_id;
        h.dest_id = config_.dest_id;
        h.control_epoch = 1;
        std::memcpy(buf, &h, sizeof(h));
    }

    void PacketGenerator::append_crc32c(std::vector<uint8_t> &packet)
    {
        const uint32_t crc = receiver::protocol::crc32c(packet.data(), packet.size());
        const size_t old_size = packet.size();
        packet.resize(old_size + sizeof(crc));
        std::memcpy(packet.data() + old_size, &crc, sizeof(crc));
    }

    // ─── Legacy single-packet API ──────────────────────────────────────────────
    std::vector<uint8_t> PacketGenerator::make_data_packet()
    {
        using receiver::protocol::CommonHeader;
        using receiver::protocol::DataSpecificHeader;

        const size_t payload_bytes = config_.frag_payload_bytes;
        std::vector<uint8_t> packet(sizeof(CommonHeader) + payload_bytes, 0);

        fill_common_header(packet.data(),
                           static_cast<uint16_t>(payload_bytes),
                           static_cast<uint8_t>(receiver::protocol::PacketType::DATA));

        DataSpecificHeader s{};
        s.frame_counter = next_frame_counter_++;
        s.cpi_count = s.frame_counter;
        s.pulse_index = 0;
        s.sample_offset = 0;
        s.sample_count = 256;
        s.data_timestamp = now_ms();
        s.health_summary = 0;
        s.set_channel_mask_data_type_compat(0x0001u, 0x00u);
        s.beam_id = 1;
        s.frag_index = 0;
        s.total_frags = 1;
        s.tail_frag_payload_bytes = static_cast<uint16_t>(payload_bytes - sizeof(DataSpecificHeader));
        std::memcpy(packet.data() + sizeof(CommonHeader), &s, sizeof(s));

        // Fill body after specific header with pattern data
        for (size_t i = sizeof(CommonHeader) + sizeof(DataSpecificHeader); i < packet.size(); ++i)
        {
            packet[i] = static_cast<uint8_t>(i & 0xFFu);
        }

        return packet;
    }

    // ─── Full CPI generation (N fragments) ─────────────────────────────────────
    std::vector<std::vector<uint8_t>> PacketGenerator::make_cpi_packets()
    {
        using receiver::protocol::CommonHeader;
        using receiver::protocol::DataSpecificHeader;
        using receiver::protocol::ExecutionSnapshot;

        const uint32_t frame_counter = next_frame_counter_++;
        const uint32_t cpi_count = next_cpi_count_++;
        const uint64_t data_ts = now_ms();
        const uint16_t total_frags = config_.cpi_frags;

        std::vector<std::vector<uint8_t>> packets;
        packets.reserve(total_frags);

        for (uint16_t frag_idx = 0; frag_idx < total_frags; ++frag_idx)
        {
            const bool is_tail = (frag_idx + 1 == total_frags);
            const size_t snapshot_size = is_tail ? sizeof(ExecutionSnapshot) : 0;
            // Body after DataSpecificHeader: payload pattern bytes
            const size_t body_bytes = config_.frag_payload_bytes;
            const size_t specific_and_body = sizeof(DataSpecificHeader) + snapshot_size + body_bytes;
            const uint16_t payload_len = static_cast<uint16_t>(specific_and_body);

            std::vector<uint8_t> packet(sizeof(CommonHeader) + specific_and_body, 0);

            fill_common_header(packet.data(), payload_len,
                               static_cast<uint8_t>(receiver::protocol::PacketType::DATA));

            DataSpecificHeader s{};
            s.frame_counter = frame_counter;
            s.cpi_count = cpi_count;
            s.pulse_index = 0;
            s.sample_offset = static_cast<uint32_t>(frag_idx) * 256u;
            s.sample_count = 256;
            s.data_timestamp = data_ts;
            s.health_summary = 0;
            s.set_channel_mask_data_type_compat(0x0001u, 0x00u);
            s.beam_id = 1;
            s.frag_index = frag_idx;
            s.total_frags = total_frags;
            s.tail_frag_payload_bytes = static_cast<uint16_t>(body_bytes);
            std::memcpy(packet.data() + sizeof(CommonHeader), &s, sizeof(s));

            size_t offset = sizeof(CommonHeader) + sizeof(DataSpecificHeader);

            // Tail fragment includes ExecutionSnapshot
            if (is_tail)
            {
                ExecutionSnapshot snapshot{};
                snapshot.work_freq_index = 1;
                snapshot.azimuth_angle = static_cast<int16_t>(cpi_count % 3600);
                std::memcpy(packet.data() + offset, &snapshot, sizeof(snapshot));
                offset += sizeof(snapshot);
            }

            // Fill body with deterministic pattern
            for (size_t i = 0; i < body_bytes; ++i)
            {
                packet[offset + i] = static_cast<uint8_t>((cpi_count + frag_idx + i) & 0xFFu);
            }

            append_crc32c(packet);
            packets.push_back(std::move(packet));
        }

        // Track heartbeat interval
        ++cpi_since_heartbeat_;

        return packets;
    }

    // ─── Heartbeat packet ──────────────────────────────────────────────────────
    std::vector<uint8_t> PacketGenerator::make_heartbeat_packet()
    {
        using receiver::protocol::CommonHeader;
        using receiver::protocol::HeartbeatPayload;

        const uint16_t payload_len = static_cast<uint16_t>(sizeof(HeartbeatPayload));
        std::vector<uint8_t> packet(sizeof(CommonHeader) + sizeof(HeartbeatPayload), 0);

        fill_common_header(packet.data(), payload_len,
                           static_cast<uint8_t>(receiver::protocol::PacketType::HEARTBEAT));

        HeartbeatPayload hb{};
        hb.system_status_alive = 1;
        hb.system_status_state = 0;
        hb.core_temp = 45;
        hb.op_mode = 1;

        // Fill CRC field inside HeartbeatPayload
        const size_t hb_data_len = sizeof(HeartbeatPayload) - sizeof(uint32_t); // Exclude trailing crc32c field
        hb.crc32c = receiver::protocol::crc32c(
            reinterpret_cast<const uint8_t *>(&hb), hb_data_len);

        std::memcpy(packet.data() + sizeof(CommonHeader), &hb, sizeof(hb));

        cpi_since_heartbeat_ = 0;
        return packet;
    }

    // ─── Apply drop / reorder impairments ──────────────────────────────────────
    std::vector<std::vector<uint8_t>>
    PacketGenerator::apply_impairments(std::vector<std::vector<uint8_t>> packets)
    {
        // Drop simulation
        if (config_.drop_rate > 0.0)
        {
            std::vector<std::vector<uint8_t>> filtered;
            filtered.reserve(packets.size());
            for (auto &pkt : packets)
            {
                if (drop_dist_(rng_) >= config_.drop_rate)
                {
                    filtered.push_back(std::move(pkt));
                }
            }
            packets = std::move(filtered);
        }

        // Reorder simulation: shuffle within sliding windows of reorder_depth
        if (config_.reorder_depth > 1 && packets.size() > 1)
        {
            const size_t depth = config_.reorder_depth;
            for (size_t i = 0; i < packets.size(); ++i)
            {
                const size_t window_end = std::min(i + depth, packets.size());
                std::uniform_int_distribution<size_t> dist(i, window_end - 1);
                const size_t j = dist(rng_);
                if (i != j)
                {
                    std::swap(packets[i], packets[j]);
                }
            }
        }

        return packets;
    }

} // namespace fpga_emulator
