#pragma once

#include "qdgz300/m01_receiver/protocol/protocol_types.h"
#include "qdgz300/m01_receiver/protocol/crc32c.h"

#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

namespace fpga_emulator
{

    /// Configuration for CPI-level packet generation
    struct GeneratorConfig
    {
        uint8_t source_id{0x11};            // DeviceID (DACS_01=0x11, DACS_02=0x12, DACS_03=0x13)
        uint8_t dest_id{0x01};              // SPS
        uint16_t cpi_frags{128};            // Fragments per CPI
        size_t frag_payload_bytes{1400};    // Raw payload bytes per fragment (body after headers)
        double drop_rate{0.0};              // 0.0~1.0 simulated packet loss
        size_t reorder_depth{0};            // Reorder injection depth (0 = no reorder)
        uint32_t heartbeat_interval_cpi{0}; // Send heartbeat every N CPIs (0 = disabled)
    };

    /**
     * @brief High-fidelity V3.1 protocol packet generator
     *
     * Generates full CPI frames as sequences of DATA packets with proper
     * CommonHeader(32B) + DataSpecificHeader(40B) + payload + CRC32C trailer.
     * Supports simulated packet loss, reorder injection, and heartbeat interleaving.
     */
    class PacketGenerator
    {
    public:
        /// Legacy constructor: single-fragment DATA packets
        explicit PacketGenerator(size_t payload_bytes);

        /// Full-config constructor for CPI generation
        explicit PacketGenerator(const GeneratorConfig &config);

        /// Generate a single DATA packet (legacy API)
        std::vector<uint8_t> make_data_packet();

        /// Generate all packets for one CPI (multiple fragments)
        std::vector<std::vector<uint8_t>> make_cpi_packets();

        /// Generate a heartbeat packet (CommonHeader + HeartbeatPayload)
        std::vector<uint8_t> make_heartbeat_packet();

        /// Apply drop/reorder simulation to a batch of packets
        std::vector<std::vector<uint8_t>> apply_impairments(std::vector<std::vector<uint8_t>> packets);

        /// Current CPI sequence counter
        uint32_t current_cpi_seq() const { return next_cpi_count_; }

    private:
        uint64_t now_ms() const;
        void fill_common_header(uint8_t *buf, uint16_t payload_len, uint8_t packet_type);
        void append_crc32c(std::vector<uint8_t> &packet);

        GeneratorConfig config_;
        uint32_t next_sequence_{1};
        uint32_t next_frame_counter_{1};
        uint32_t next_cpi_count_{1};
        uint32_t cpi_since_heartbeat_{0};

        std::mt19937 rng_{42};
        std::uniform_real_distribution<double> drop_dist_{0.0, 1.0};
    };

} // namespace fpga_emulator
