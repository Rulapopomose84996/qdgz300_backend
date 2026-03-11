#ifndef RECEIVER_CAPTURE_PCAP_WRITER_H
#define RECEIVER_CAPTURE_PCAP_WRITER_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace receiver
{
    namespace capture
    {

        struct PcapWriterConfig
        {
            bool enabled = false;
            std::string output_dir = "/var/log/receiver";
            size_t max_file_size_mb = 1024;
            size_t max_files = 10;
            std::vector<uint8_t> filter_packet_types;
            std::vector<uint8_t> filter_source_ids;
        };

        class PcapWriter
        {
        public:
            explicit PcapWriter(const PcapWriterConfig &config);
            ~PcapWriter();

            bool start();
            void stop();

            void write_packet(const uint8_t *raw_data, size_t length, uint64_t timestamp_us);

            bool is_recording() const;
            uint64_t packets_written() const;

        private:
            void write_global_header();
            void write_record_header(uint32_t ts_sec, uint32_t ts_usec, uint32_t incl_len, uint32_t orig_len);
            void rotate_file_if_needed();

            bool open_new_file();
            bool passes_filters(const uint8_t *raw_data, size_t length) const;
            uint64_t max_file_size_bytes() const;

            PcapWriterConfig config_;
            std::ofstream current_file_;
            size_t current_file_size_{0};
            std::atomic<uint64_t> packets_written_{0};
            std::atomic<bool> recording_{false};
            uint64_t file_index_{0};
            size_t pending_record_size_{0};
            std::deque<std::string> recent_files_;
            mutable std::mutex mutex_;
        };

    } // namespace capture
} // namespace receiver

#endif // RECEIVER_CAPTURE_PCAP_WRITER_H
