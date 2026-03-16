#ifndef RECEIVER_CAPTURE_PCAP_WRITER_H
#define RECEIVER_CAPTURE_PCAP_WRITER_H

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace receiver
{
    namespace capture
    {

        struct PcapWriterConfig
        {
            bool enabled = false;
            std::string spool_dir = "/var/spool/qdgz300/receiver";
            std::string archive_dir;
            size_t max_file_size_mb = 1024;
            size_t max_files = 10;
            size_t pending_queue_capacity = 4096;
            std::vector<uint8_t> filter_packet_types;
            std::vector<uint8_t> filter_source_ids;
        };

        struct PcapWriterStatistics
        {
            uint64_t enqueued_packets{0};
            uint64_t written_packets{0};
            uint64_t dropped_queue_full{0};
            uint64_t write_errors{0};
            uint64_t rotated_files{0};
            bool recording{false};
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
            PcapWriterStatistics get_statistics() const;
            std::string current_spool_file() const;
            std::string last_sealed_file() const;

        private:
            struct PendingPacket
            {
                uint64_t timestamp_us{0};
                std::vector<uint8_t> data;
            };

            void write_global_header();
            void write_record_header(uint32_t ts_sec, uint32_t ts_usec, uint32_t incl_len, uint32_t orig_len);
            void rotate_file_if_needed();

            bool open_new_file();
            bool seal_current_file();
            void run_writer_loop();
            void mark_write_error();
            bool passes_filters(const uint8_t *raw_data, size_t length) const;
            uint64_t max_file_size_bytes() const;

            PcapWriterConfig config_;
            std::ofstream current_file_;
            size_t current_file_size_{0};
            std::deque<PendingPacket> pending_packets_;
            std::atomic<uint64_t> packets_written_{0};
            std::atomic<uint64_t> enqueued_packets_{0};
            std::atomic<uint64_t> dropped_queue_full_{0};
            std::atomic<uint64_t> write_errors_{0};
            std::atomic<uint64_t> rotated_files_{0};
            std::atomic<bool> recording_{false};
            std::atomic<bool> accepting_packets_{false};
            uint64_t file_index_{0};
            size_t pending_record_size_{0};
            std::deque<std::string> recent_files_;
            std::string current_temp_file_path_;
            std::string current_sealed_file_path_;
            std::string last_sealed_file_path_;
            mutable std::mutex mutex_;
            std::condition_variable cv_;
            std::thread writer_thread_;
        };

    } // namespace capture
} // namespace receiver

#endif // RECEIVER_CAPTURE_PCAP_WRITER_H
