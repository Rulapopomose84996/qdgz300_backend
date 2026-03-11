#ifndef RECEIVER_DELIVERY_STUB_CONSUMER_H
#define RECEIVER_DELIVERY_STUB_CONSUMER_H

#include "qdgz300/m01_receiver/delivery/rawblock_adapter.h"
#include "qdgz300/m01_receiver/delivery/raw_block.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace receiver
{
    namespace delivery
    {
        /**
         * @brief 消费者桩配置
         */
        struct StubConsumerConfig
        {
            uint8_t array_id{1};            // 阵面编号
            bool print_summary{true};       // 是否打印摘要
            bool write_to_file{false};      // 是否写入文件
            std::string output_file{};      // 输出文件路径
            size_t stats_interval_ms{1000}; // 统计输出间隔
        };

        /**
         * @brief 消费者桩统计信息
         */
        struct StubConsumerStatistics
        {
            std::atomic<uint64_t> total_blocks{0};      // 总处理块数
            std::atomic<uint64_t> complete_frames{0};   // 完整帧数
            std::atomic<uint64_t> incomplete_frames{0}; // 不完整帧数
            std::atomic<uint64_t> total_bytes{0};       // 总字节数
            std::atomic<uint64_t> last_cpi_seq{0};      // 最后CPI序列号
        };

        /**
         * @brief 消费者桩 - SPSC队列的消费端
         *
         * M01 §4.13 消费者桩规范
         * 职责：
         * - 从RawCPI_Q读取RawBlock
         * - 打印摘要（cpi_seq, array_id, data_size, flags, fragment_count）
         * - 统计每秒帧率、完整帧/不完整帧比例
         * - 可选：写入二进制文件供离线验证
         *
         * 注意：消费者线程不绑定CPU 16-18（避免干扰热路径）
         */
        class StubConsumer
        {
        public:
            using RawBlockQueue = RawBlockAdapter::RawBlockQueue;

            /**
             * @brief 构造消费者桩
             * @param config 配置
             * @param queue 输入SPSC队列
             */
            explicit StubConsumer(const StubConsumerConfig &config,
                                  std::shared_ptr<RawBlockQueue> queue);

            /**
             * @brief 析构
             */
            ~StubConsumer();

            /**
             * @brief 禁止拷贝
             */
            StubConsumer(const StubConsumer &) = delete;
            StubConsumer &operator=(const StubConsumer &) = delete;

            /**
             * @brief 启动消费者线程
             * @return 启动成功返回true
             */
            bool start();

            /**
             * @brief 停止消费者线程
             */
            void stop();

            /**
             * @brief 获取统计信息
             */
            const StubConsumerStatistics &get_statistics() const { return stats_; }

        private:
            void run();
            void process_block(const std::shared_ptr<RawBlock> &block);
            void print_statistics();
            bool open_output_file();
            void close_output_file();
            void write_block_to_file(const RawBlock &block);

            StubConsumerConfig config_;
            std::shared_ptr<RawBlockQueue> queue_;
            StubConsumerStatistics stats_;
            std::atomic<bool> running_{false};
            std::thread worker_;
            int output_fd_{-1};

            // 统计周期计数器
            uint64_t period_blocks_{0};
            uint64_t period_complete_{0};
            uint64_t period_incomplete_{0};
            uint64_t last_stats_time_ms_{0};
        };

    } // namespace delivery
} // namespace receiver

#endif // RECEIVER_DELIVERY_STUB_CONSUMER_H
