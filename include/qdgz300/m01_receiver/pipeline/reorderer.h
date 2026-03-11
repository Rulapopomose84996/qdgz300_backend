#ifndef RECEIVER_PIPELINE_REORDERER_H
#define RECEIVER_PIPELINE_REORDERER_H

#include "qdgz300/m01_receiver/protocol/protocol_types.h"
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <atomic>
#include <vector>

namespace receiver
{
    namespace pipeline
    {

        /**
         * @brief 乱序重排配置
         */
        struct ReorderConfig
        {
            size_t window_size = 512;
            uint32_t timeout_ms = 50;
            bool enable_zero_fill = true;
        };

        /**
         * @brief 重排后的输出报文
         */
        struct OrderedPacket
        {
            protocol::ParsedPacket packet;
            std::unique_ptr<uint8_t[]> owned_payload;
            size_t payload_size{0};
            bool is_zero_filled;
            bool is_incomplete_frame{false};
            uint32_t sequence_number;
        };

        /**
         * @brief 重排序窗口状态
         */
        struct alignas(64) ReorderWindow
        {
            uint32_t next_expected_seq{0};
            uint32_t buffered_packets{0};
            uint64_t last_advance_ns{0};
        };

        static_assert(alignof(ReorderWindow) == 64, "ReorderWindow must be cache-line aligned");
        static_assert(offsetof(ReorderWindow, next_expected_seq) == 0, "ReorderWindow.next_expected_seq offset mismatch");

        /**
         * @brief 序列重排器
         *
         * 职责：
         * - 缓冲乱序到达的报文并按序输出
         * - 处理超时推进与可选零填补洞
         * - 维护重排统计信息
         */
        class Reorderer
        {
        public:
            using OutputCallback = std::function<void(OrderedPacket &&)>;

            /**
             * @brief 构造重排器
             * @param config 重排配置
             * @param output_callback 有序输出回调
             */
            explicit Reorderer(const ReorderConfig &config, OutputCallback output_callback);

            /**
             * @brief 析构重排器
             */
            ~Reorderer();

            /**
             * @brief 插入一个待重排数据包
             * @param packet 已解析数据包
             * @return void
             */
            void insert(const protocol::ParsedPacket &packet);

            /**
             * @brief 插入一个已拥有 payload 所有权的数据包
             * @param header 已填充的协议头
             * @param payload 由调用方转移所有权的 payload
             * @param payload_size payload 字节数
             * @return void
             */
            void insert_owned(protocol::CommonHeader header,
                              std::unique_ptr<uint8_t[]> payload,
                              size_t payload_size);

            /**
             * @brief 执行超时检查并触发窗口推进
             * @return void
             */
            void check_timeout();

            /**
             * @brief 动态设置超时阈值
             * @param timeout_ms 超时时间（毫秒）
             * @return void
             */
            void set_timeout_ms(uint32_t timeout_ms);

            /**
             * @brief 强制刷新缓冲区
             * @return 刷新输出的报文数量
             */
            size_t flush();

            struct Statistics
            {
                uint64_t packets_in_order{0};
                uint64_t packets_out_of_order{0};
                uint64_t packets_duplicate{0};
                uint64_t packets_zero_filled{0};
                uint64_t sequences_advanced{0};
                size_t current_buffer_size{0};
            };

            /**
             * @brief 获取重排统计信息
             * @return 当前统计快照
             */
            Statistics get_statistics() const;

        private:
            class Impl;
            std::unique_ptr<Impl> impl_;

            ReorderConfig config_;
            OutputCallback output_callback_;
            std::atomic<uint32_t> timeout_ms_runtime_{50};

            void advance_window();
            OrderedPacket create_zero_filled_packet(uint32_t seq_num);
        };

    } // namespace pipeline
} // namespace receiver

#endif // RECEIVER_PIPELINE_REORDERER_H
