#ifndef RECEIVER_DELIVERY_RAW_BLOCK_H
#define RECEIVER_DELIVERY_RAW_BLOCK_H

#include <cstddef>
#include <cstdint>

namespace receiver
{
    namespace delivery
    {
        /**
         * @brief RawBlock标志位定义
         */
        enum class RawBlockFlags : uint32_t
        {
            INCOMPLETE_FRAME = 0x01,  // bit0: 帧不完整（有缺失分片）
            SNAPSHOT_PRESENT = 0x02,  // bit1: 包含Execution Snapshot
            HEARTBEAT_RELATED = 0x04, // bit2: 与心跳相关
        };

        /**
         * @brief RAW有效载荷最大尺寸
         *
         * 根据M01规范：
         * - 单CPI最大约1.5MB（1024分片 × 1472字节/分片）
         * - 预留2MB以容纳Headers和Snapshot
         */
        constexpr size_t RAW_BLOCK_PAYLOAD_SIZE = 2 * 1024 * 1024; // 2MB

        /**
         * @brief RawBlock - 重组后的完整CPI帧数据块
         *
         * M01 §2.2 冻结定义
         * 用于SPSC队列投递到下游信号处理流水线
         */
        struct RawBlock
        {
            uint64_t ingest_ts;                      // 入口时间戳 (ns) - 首个分片接收时刻
            uint64_t data_ts;                        // 数据时间戳 (ns) - 来自DACS DataTimestamp
            uint8_t array_id;                        // 阵面编号 1/2/3
            uint32_t cpi_seq;                        // CPI序列号 (CpiCount)
            uint16_t fragment_count;                 // 分片总数 (TotalFrags)
            uint32_t data_size;                      // RAW有效载荷总字节数
            uint32_t flags;                          // 标志位组合 (RawBlockFlags)
            uint8_t payload[RAW_BLOCK_PAYLOAD_SIZE]; // RAW IQ数据有效载荷

            /**
             * @brief 检查标志位
             */
            bool has_flag(RawBlockFlags flag) const
            {
                return (flags & static_cast<uint32_t>(flag)) != 0;
            }

            /**
             * @brief 设置标志位
             */
            void set_flag(RawBlockFlags flag)
            {
                flags |= static_cast<uint32_t>(flag);
            }

            /**
             * @brief 清除标志位
             */
            void clear_flag(RawBlockFlags flag)
            {
                flags &= ~static_cast<uint32_t>(flag);
            }
        };

        static_assert(sizeof(RawBlock) > RAW_BLOCK_PAYLOAD_SIZE, "RawBlock size verification");

    } // namespace delivery
} // namespace receiver

#endif // RECEIVER_DELIVERY_RAW_BLOCK_H
