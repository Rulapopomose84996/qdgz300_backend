#include "qdgz300/m01_receiver/capture/pcap_writer.h"

#include "qdgz300/m01_receiver/protocol/protocol_types.h"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <system_error>

#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace receiver
{
    namespace capture
    {
        namespace
        {
            constexpr uint32_t kPcapMagic = 0xA1B2C3D4u;
            constexpr uint16_t kPcapVersionMajor = 2u;
            constexpr uint16_t kPcapVersionMinor = 4u;
            constexpr int32_t kPcapThisZone = 0;
            constexpr uint32_t kPcapSigFigs = 0u;
            constexpr uint32_t kPcapSnapLen = 65535u;
            constexpr uint32_t kPcapLinkTypeUser0 = 147u;
            constexpr size_t kPcapGlobalHeaderSize = 24u;
            constexpr size_t kPcapRecordHeaderSize = 16u;
            constexpr size_t kPacketTypeOffset = 18u;
            constexpr size_t kSourceIdOffset = 20u;
            constexpr uint64_t kBytesPerMb = 1024ull * 1024ull;
            constexpr char kPathSep = '/';
            constexpr const char *kPartialSuffix = ".pcap.part";
            constexpr const char *kSealedSuffix = ".pcap";

            bool create_directory_if_missing(const std::string &path)
            {
                if (path.empty())
                {
                    return false;
                }

                const int rc = ::mkdir(path.c_str(), 0755);
                return rc == 0 || errno == EEXIST;
            }

            bool ensure_directories(const std::string &dir)
            {
                if (dir.empty())
                {
                    return false;
                }

                std::string normalized = dir;
                for (char &c : normalized)
                {
                    if (c == '/' || c == '\\')
                    {
                        c = kPathSep;
                    }
                }

                std::string current;
                size_t index = 0;
                if (!normalized.empty() && normalized[0] == kPathSep)
                {
                    current.push_back(kPathSep);
                    index = 1;
                }

                while (index < normalized.size())
                {
                    const size_t next = normalized.find(kPathSep, index);
                    const size_t end = (next == std::string::npos) ? normalized.size() : next;
                    if (end > index)
                    {
                        if (!current.empty() && current.back() != kPathSep)
                        {
                            current.push_back(kPathSep);
                        }
                        current.append(normalized, index, end - index);
                        if (!create_directory_if_missing(current))
                        {
                            return false;
                        }
                    }
                    if (next == std::string::npos)
                    {
                        break;
                    }
                    index = next + 1;
                }

                return true;
            }

            std::string join_path(const std::string &dir, const std::string &file)
            {
                if (dir.empty())
                {
                    return file;
                }
                if (dir.back() == '/' || dir.back() == '\\')
                {
                    return dir + file;
                }
                return dir + kPathSep + file;
            }

            void pin_writer_thread_to_management_cpus()
            {
#if defined(__linux__)
                cpu_set_t cpuset;
                CPU_ZERO(&cpuset);
                for (int cpu = 0; cpu <= 15; ++cpu)
                {
                    CPU_SET(cpu, &cpuset);
                }
                (void)pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
#endif
            }
        } // namespace

        PcapWriter::PcapWriter(const PcapWriterConfig &config)
            : config_(config)
        {
        }

        PcapWriter::~PcapWriter()
        {
            stop();
        }

        bool PcapWriter::start()
        {
            if (!config_.enabled)
            {
                return false;
            }

            std::lock_guard<std::mutex> lock(mutex_);
            if (recording_.load(std::memory_order_acquire))
            {
                return true;
            }

            if (!open_new_file())
            {
                return false;
            }

            pending_packets_.clear();
            packets_written_.store(0, std::memory_order_release);
            enqueued_packets_.store(0, std::memory_order_release);
            dropped_queue_full_.store(0, std::memory_order_release);
            write_errors_.store(0, std::memory_order_release);
            rotated_files_.store(0, std::memory_order_release);
            accepting_packets_.store(true, std::memory_order_release);
            recording_.store(true, std::memory_order_release);
            writer_thread_ = std::thread(&PcapWriter::run_writer_loop, this);
            return true;
        }

        void PcapWriter::stop()
        {
            accepting_packets_.store(false, std::memory_order_release);
            cv_.notify_all();

            if (writer_thread_.joinable())
            {
                writer_thread_.join();
            }

            std::lock_guard<std::mutex> lock(mutex_);
            (void)seal_current_file();
            pending_packets_.clear();
            recording_.store(false, std::memory_order_release);
        }

        void PcapWriter::write_packet(const uint8_t *raw_data, size_t length, uint64_t timestamp_us)
        {
            if (!accepting_packets_.load(std::memory_order_acquire))
            {
                return;
            }
            if (raw_data == nullptr || length == 0)
            {
                return;
            }
            if (!passes_filters(raw_data, length))
            {
                return;
            }

            PendingPacket packet;
            packet.timestamp_us = timestamp_us;
            packet.data.assign(raw_data, raw_data + length);

            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!accepting_packets_.load(std::memory_order_acquire))
                {
                    return;
                }
                const size_t queue_capacity = std::max<size_t>(1, config_.pending_queue_capacity);
                if (pending_packets_.size() >= queue_capacity)
                {
                    dropped_queue_full_.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                pending_packets_.push_back(std::move(packet));
                enqueued_packets_.fetch_add(1, std::memory_order_relaxed);
            }

            cv_.notify_one();
        }

        bool PcapWriter::is_recording() const
        {
            return recording_.load(std::memory_order_acquire);
        }

        uint64_t PcapWriter::packets_written() const
        {
            return packets_written_.load(std::memory_order_relaxed);
        }

        PcapWriterStatistics PcapWriter::get_statistics() const
        {
            PcapWriterStatistics stats;
            stats.enqueued_packets = enqueued_packets_.load(std::memory_order_relaxed);
            stats.written_packets = packets_written_.load(std::memory_order_relaxed);
            stats.dropped_queue_full = dropped_queue_full_.load(std::memory_order_relaxed);
            stats.write_errors = write_errors_.load(std::memory_order_relaxed);
            stats.rotated_files = rotated_files_.load(std::memory_order_relaxed);
            stats.recording = recording_.load(std::memory_order_acquire);
            return stats;
        }

        std::string PcapWriter::current_spool_file() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return current_temp_file_path_;
        }

        std::string PcapWriter::last_sealed_file() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return last_sealed_file_path_;
        }

        void PcapWriter::write_global_header()
        {
            current_file_.write(reinterpret_cast<const char *>(&kPcapMagic), sizeof(kPcapMagic));
            current_file_.write(reinterpret_cast<const char *>(&kPcapVersionMajor), sizeof(kPcapVersionMajor));
            current_file_.write(reinterpret_cast<const char *>(&kPcapVersionMinor), sizeof(kPcapVersionMinor));
            current_file_.write(reinterpret_cast<const char *>(&kPcapThisZone), sizeof(kPcapThisZone));
            current_file_.write(reinterpret_cast<const char *>(&kPcapSigFigs), sizeof(kPcapSigFigs));
            current_file_.write(reinterpret_cast<const char *>(&kPcapSnapLen), sizeof(kPcapSnapLen));
            current_file_.write(reinterpret_cast<const char *>(&kPcapLinkTypeUser0), sizeof(kPcapLinkTypeUser0));
            current_file_size_ = kPcapGlobalHeaderSize;
        }

        void PcapWriter::write_record_header(uint32_t ts_sec, uint32_t ts_usec, uint32_t incl_len, uint32_t orig_len)
        {
            current_file_.write(reinterpret_cast<const char *>(&ts_sec), sizeof(ts_sec));
            current_file_.write(reinterpret_cast<const char *>(&ts_usec), sizeof(ts_usec));
            current_file_.write(reinterpret_cast<const char *>(&incl_len), sizeof(incl_len));
            current_file_.write(reinterpret_cast<const char *>(&orig_len), sizeof(orig_len));
        }

        void PcapWriter::rotate_file_if_needed()
        {
            const uint64_t max_bytes = max_file_size_bytes();
            if (!current_file_.is_open())
            {
                return;
            }

            if ((static_cast<uint64_t>(current_file_size_) + static_cast<uint64_t>(pending_record_size_)) <= max_bytes)
            {
                return;
            }

            if (!seal_current_file())
            {
                mark_write_error();
                return;
            }
            rotated_files_.fetch_add(1, std::memory_order_relaxed);
            if (!open_new_file())
            {
                mark_write_error();
            }
        }

        bool PcapWriter::open_new_file()
        {
            if (!ensure_directories(config_.spool_dir))
            {
                return false;
            }

            const uint64_t now_us = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count());

            const std::string base_name =
                "receiver_spool_" + std::to_string(now_us) + "_" + std::to_string(file_index_++);
            current_sealed_file_path_ = join_path(config_.spool_dir, base_name + kSealedSuffix);
            current_temp_file_path_ = join_path(config_.spool_dir, base_name + kPartialSuffix);

            current_file_.open(current_temp_file_path_.c_str(), std::ios::binary | std::ios::trunc);
            if (!current_file_.is_open())
            {
                current_temp_file_path_.clear();
                current_sealed_file_path_.clear();
                return false;
            }

            write_global_header();
            if (!current_file_.good())
            {
                current_file_.close();
                (void)std::remove(current_temp_file_path_.c_str());
                current_temp_file_path_.clear();
                current_sealed_file_path_.clear();
                return false;
            }

            return true;
        }

        bool PcapWriter::seal_current_file()
        {
            if (current_file_.is_open())
            {
                current_file_.flush();
                current_file_.close();
            }

            if (current_temp_file_path_.empty() || current_sealed_file_path_.empty())
            {
                current_temp_file_path_.clear();
                current_sealed_file_path_.clear();
                return true;
            }

            if (::rename(current_temp_file_path_.c_str(), current_sealed_file_path_.c_str()) != 0)
            {
                return false;
            }

            recent_files_.push_back(current_sealed_file_path_);
            last_sealed_file_path_ = current_sealed_file_path_;
            const size_t keep_count = std::max<size_t>(1, config_.max_files);
            while (recent_files_.size() > keep_count)
            {
                const std::string old_path = recent_files_.front();
                recent_files_.pop_front();
                (void)std::remove(old_path.c_str());
            }

            current_temp_file_path_.clear();
            current_sealed_file_path_.clear();
            current_file_size_ = 0;
            return true;
        }

        void PcapWriter::run_writer_loop()
        {
            pin_writer_thread_to_management_cpus();

            while (true)
            {
                PendingPacket packet;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    cv_.wait(lock, [this]()
                             { return !pending_packets_.empty() || !accepting_packets_.load(std::memory_order_acquire); });

                    if (pending_packets_.empty())
                    {
                        if (!accepting_packets_.load(std::memory_order_acquire))
                        {
                            break;
                        }
                        continue;
                    }

                    packet = std::move(pending_packets_.front());
                    pending_packets_.pop_front();
                }

                std::lock_guard<std::mutex> lock(mutex_);
                if (!current_file_.is_open())
                {
                    mark_write_error();
                    break;
                }

                pending_record_size_ = kPcapRecordHeaderSize + packet.data.size();
                rotate_file_if_needed();
                if (!current_file_.is_open())
                {
                    break;
                }

                const uint32_t ts_sec = static_cast<uint32_t>(packet.timestamp_us / 1000000ull);
                const uint32_t ts_usec = static_cast<uint32_t>(packet.timestamp_us % 1000000ull);
                const uint32_t incl_len = static_cast<uint32_t>(packet.data.size());

                write_record_header(ts_sec, ts_usec, incl_len, incl_len);
                current_file_.write(reinterpret_cast<const char *>(packet.data.data()),
                                    static_cast<std::streamsize>(packet.data.size()));
                if (!current_file_.good())
                {
                    mark_write_error();
                    break;
                }

                current_file_size_ += pending_record_size_;
                packets_written_.fetch_add(1, std::memory_order_relaxed);
                pending_record_size_ = 0;
            }

            std::lock_guard<std::mutex> lock(mutex_);
            (void)seal_current_file();
            recording_.store(false, std::memory_order_release);
        }

        void PcapWriter::mark_write_error()
        {
            write_errors_.fetch_add(1, std::memory_order_relaxed);
            accepting_packets_.store(false, std::memory_order_release);
            recording_.store(false, std::memory_order_release);
            if (current_file_.is_open())
            {
                current_file_.flush();
                current_file_.close();
            }
        }

        bool PcapWriter::passes_filters(const uint8_t *raw_data, size_t length) const
        {
            if (!config_.filter_packet_types.empty())
            {
                if (length <= kPacketTypeOffset)
                {
                    return false;
                }
                const uint8_t type = raw_data[kPacketTypeOffset];
                const auto it = std::find(config_.filter_packet_types.begin(), config_.filter_packet_types.end(), type);
                if (it == config_.filter_packet_types.end())
                {
                    return false;
                }
            }

            if (!config_.filter_source_ids.empty())
            {
                if (length <= kSourceIdOffset)
                {
                    return false;
                }
                const uint8_t source_id = raw_data[kSourceIdOffset];
                const auto it = std::find(config_.filter_source_ids.begin(), config_.filter_source_ids.end(), source_id);
                if (it == config_.filter_source_ids.end())
                {
                    return false;
                }
            }

            return true;
        }

        uint64_t PcapWriter::max_file_size_bytes() const
        {
            return static_cast<uint64_t>(config_.max_file_size_mb) * kBytesPerMb;
        }

    } // namespace capture
} // namespace receiver
