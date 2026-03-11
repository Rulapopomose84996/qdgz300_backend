#include "qdgz300/m01_receiver/delivery/stub_consumer.h"
#include "qdgz300/m01_receiver/monitoring/logger.h"

#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace receiver
{
    namespace delivery
    {
        namespace
        {
            uint64_t now_ms_epoch()
            {
                return static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count());
            }

            const char *flags_to_string(uint32_t flags)
            {
                static thread_local char buffer[64];
                buffer[0] = '\0';

                bool has_any = false;
                if (flags & static_cast<uint32_t>(RawBlockFlags::INCOMPLETE_FRAME))
                {
                    std::strcat(buffer, "INCOMPLETE");
                    has_any = true;
                }
                if (flags & static_cast<uint32_t>(RawBlockFlags::SNAPSHOT_PRESENT))
                {
                    if (has_any)
                        std::strcat(buffer, "|");
                    std::strcat(buffer, "SNAPSHOT");
                    has_any = true;
                }
                if (flags & static_cast<uint32_t>(RawBlockFlags::HEARTBEAT_RELATED))
                {
                    if (has_any)
                        std::strcat(buffer, "|");
                    std::strcat(buffer, "HEARTBEAT");
                    has_any = true;
                }

                if (!has_any)
                {
                    std::strcpy(buffer, "NONE");
                }

                return buffer;
            }
        } // namespace

        StubConsumer::StubConsumer(const StubConsumerConfig &config,
                                   std::shared_ptr<RawBlockQueue> queue)
            : config_(config), queue_(std::move(queue))
        {
        }

        StubConsumer::~StubConsumer()
        {
            stop();
        }

        bool StubConsumer::start()
        {
            if (running_.exchange(true, std::memory_order_acq_rel))
            {
                return true;
            }

            if (config_.write_to_file && !open_output_file())
            {
                running_.store(false, std::memory_order_release);
                return false;
            }

            last_stats_time_ms_ = now_ms_epoch();
            worker_ = std::thread(&StubConsumer::run, this);

            LOG_INFO("StubConsumer started: array_id=%u print=%d write=%d output=%s",
                     static_cast<unsigned>(config_.array_id),
                     config_.print_summary ? 1 : 0,
                     config_.write_to_file ? 1 : 0,
                     config_.output_file.c_str());

            return true;
        }

        void StubConsumer::stop()
        {
            if (!running_.exchange(false, std::memory_order_acq_rel))
            {
                return;
            }

            if (worker_.joinable())
            {
                worker_.join();
            }

            close_output_file();

            LOG_INFO("StubConsumer stopped: array_id=%u total_blocks=%lu complete=%lu incomplete=%lu",
                     static_cast<unsigned>(config_.array_id),
                     stats_.total_blocks.load(std::memory_order_relaxed),
                     stats_.complete_frames.load(std::memory_order_relaxed),
                     stats_.incomplete_frames.load(std::memory_order_relaxed));
        }

        void StubConsumer::run()
        {
            // 消费者线程不绑定CPU，避免干扰热路径（CPU 16-18）
            while (running_.load(std::memory_order_acquire))
            {
                auto block = queue_ ? queue_->try_pop() : std::nullopt;
                if (block.has_value())
                {
                    if (*block)
                    {
                        process_block(*block);
                    }
                }
                else
                {
                    // 队列为空，短暂休眠避免空轮询
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                }

                // 定期输出统计信息
                const uint64_t now = now_ms_epoch();
                if (now - last_stats_time_ms_ >= config_.stats_interval_ms)
                {
                    print_statistics();
                    last_stats_time_ms_ = now;
                }
            }

            // 最终统计
            print_statistics();
        }

        void StubConsumer::process_block(const std::shared_ptr<RawBlock> &block)
        {
            stats_.total_blocks.fetch_add(1, std::memory_order_relaxed);
            stats_.total_bytes.fetch_add(block->data_size, std::memory_order_relaxed);
            stats_.last_cpi_seq.store(block->cpi_seq, std::memory_order_relaxed);

            period_blocks_++;

            const bool is_incomplete = block->has_flag(RawBlockFlags::INCOMPLETE_FRAME);
            if (is_incomplete)
            {
                stats_.incomplete_frames.fetch_add(1, std::memory_order_relaxed);
                period_incomplete_++;
            }
            else
            {
                stats_.complete_frames.fetch_add(1, std::memory_order_relaxed);
                period_complete_++;
            }

            if (config_.print_summary)
            {
                LOG_DEBUG("RawBlock: array_id=%u cpi_seq=%u frags=%u data_size=%u flags=%s ingest_ts=%lu data_ts=%lu",
                          static_cast<unsigned>(block->array_id),
                          static_cast<unsigned>(block->cpi_seq),
                          static_cast<unsigned>(block->fragment_count),
                          static_cast<unsigned>(block->data_size),
                          flags_to_string(block->flags),
                          block->ingest_ts,
                          block->data_ts);
            }

            if (config_.write_to_file && output_fd_ >= 0)
            {
                write_block_to_file(*block);
            }
        }

        void StubConsumer::print_statistics()
        {
            if (period_blocks_ == 0)
            {
                return;
            }

            const uint64_t queue_dropped = queue_ ? queue_->drop_count() : 0;
            const size_t queue_depth = queue_ ? queue_->size() : 0;

            LOG_INFO("StubConsumer[%u] stats: period_blocks=%lu complete=%lu incomplete=%lu "
                     "queue_depth=%zu queue_dropped=%lu last_cpi=%lu",
                     static_cast<unsigned>(config_.array_id),
                     period_blocks_,
                     period_complete_,
                     period_incomplete_,
                     queue_depth,
                     queue_dropped,
                     stats_.last_cpi_seq.load(std::memory_order_relaxed));

            // 重置周期计数器
            period_blocks_ = 0;
            period_complete_ = 0;
            period_incomplete_ = 0;
        }

        bool StubConsumer::open_output_file()
        {
            if (config_.output_file.empty())
            {
                return false;
            }

            output_fd_ = open(config_.output_file.c_str(),
                              O_WRONLY | O_CREAT | O_TRUNC,
                              0644);
            if (output_fd_ < 0)
            {
                LOG_ERROR("Failed to open output file: %s (errno=%d)",
                          config_.output_file.c_str(), errno);
                return false;
            }

            return true;
        }

        void StubConsumer::close_output_file()
        {
            if (output_fd_ >= 0)
            {
                close(output_fd_);
                output_fd_ = -1;
            }
        }

        void StubConsumer::write_block_to_file(const RawBlock &block)
        {
            // 写入RawBlock头部（不包含payload数组本身的完整声明）
            struct RawBlockHeader
            {
                uint64_t ingest_ts;
                uint64_t data_ts;
                uint8_t array_id;
                uint32_t cpi_seq;
                uint16_t fragment_count;
                uint32_t data_size;
                uint32_t flags;
            } __attribute__((packed));

            RawBlockHeader header{};
            header.ingest_ts = block.ingest_ts;
            header.data_ts = block.data_ts;
            header.array_id = block.array_id;
            header.cpi_seq = block.cpi_seq;
            header.fragment_count = block.fragment_count;
            header.data_size = block.data_size;
            header.flags = block.flags;

            // 写入头部
            if (write(output_fd_, &header, sizeof(header)) != sizeof(header))
            {
                LOG_WARN("Failed to write block header to file (errno=%d)", errno);
                return;
            }

            // 写入有效载荷（只写实际数据大小）
            if (block.data_size > 0)
            {
                const ssize_t written = write(output_fd_, block.payload, block.data_size);
                if (written != static_cast<ssize_t>(block.data_size))
                {
                    LOG_WARN("Failed to write block payload to file: expected=%u written=%zd (errno=%d)",
                             block.data_size, written, errno);
                }
            }
        }

    } // namespace delivery
} // namespace receiver
