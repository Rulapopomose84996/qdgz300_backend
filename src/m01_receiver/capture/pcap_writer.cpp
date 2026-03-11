#include "qdgz300/m01_receiver/capture/pcap_writer.h"

#include "qdgz300/m01_receiver/protocol/protocol_types.h"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <string>

#include <sys/stat.h>
#include <sys/types.h>

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

            packets_written_.store(0, std::memory_order_release);
            recording_.store(true, std::memory_order_release);
            return true;
        }

        void PcapWriter::stop()
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (current_file_.is_open())
            {
                current_file_.flush();
                current_file_.close();
            }
            recording_.store(false, std::memory_order_release);
        }

        void PcapWriter::write_packet(const uint8_t *raw_data, size_t length, uint64_t timestamp_us)
        {
            if (!recording_.load(std::memory_order_acquire))
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

            std::lock_guard<std::mutex> lock(mutex_);
            if (!recording_.load(std::memory_order_acquire) || !current_file_.is_open())
            {
                return;
            }

            pending_record_size_ = kPcapRecordHeaderSize + length;
            rotate_file_if_needed();
            if (!current_file_.is_open())
            {
                return;
            }

            const uint32_t ts_sec = static_cast<uint32_t>(timestamp_us / 1000000ull);
            const uint32_t ts_usec = static_cast<uint32_t>(timestamp_us % 1000000ull);
            const uint32_t incl_len = static_cast<uint32_t>(length);

            write_record_header(ts_sec, ts_usec, incl_len, incl_len);
            current_file_.write(reinterpret_cast<const char *>(raw_data), static_cast<std::streamsize>(length));
            if (!current_file_.good())
            {
                current_file_.close();
                recording_.store(false, std::memory_order_release);
                return;
            }

            current_file_size_ += pending_record_size_;
            packets_written_.fetch_add(1, std::memory_order_relaxed);
            pending_record_size_ = 0;
        }

        bool PcapWriter::is_recording() const
        {
            return recording_.load(std::memory_order_acquire);
        }

        uint64_t PcapWriter::packets_written() const
        {
            return packets_written_.load(std::memory_order_relaxed);
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

            current_file_.flush();
            current_file_.close();
            (void)open_new_file();
        }

        bool PcapWriter::open_new_file()
        {
            if (!ensure_directories(config_.output_dir))
            {
                return false;
            }

            const uint64_t now_us = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count());
            const std::string file_name =
                "receiver_capture_" + std::to_string(now_us) + "_" + std::to_string(file_index_++) + ".pcap";
            const std::string file_path = join_path(config_.output_dir, file_name);

            current_file_.open(file_path.c_str(), std::ios::binary | std::ios::trunc);
            if (!current_file_.is_open())
            {
                return false;
            }

            write_global_header();
            if (!current_file_.good())
            {
                current_file_.close();
                return false;
            }

            recent_files_.push_back(file_path);
            const size_t keep_count = std::max<size_t>(1, config_.max_files);
            while (recent_files_.size() > keep_count)
            {
                const std::string old_path = recent_files_.front();
                recent_files_.pop_front();
                (void)std::remove(old_path.c_str());
            }

            return true;
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
