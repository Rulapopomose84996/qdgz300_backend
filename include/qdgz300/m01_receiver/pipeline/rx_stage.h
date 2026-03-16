/**
 * @file rx_stage.h
 * @brief 实时接收层 (RxStage)——收包热路径的唯一处理单元
 *
 * RxStage 是分层设计中"实时接收层"的核心类，运行在 CPU 16/17/18 的
 * ArrayFaceReceiver 收包线程上下文中。其职责被严格限制为：
 *
 *  1. 协议头解析（PacketParser::parse，仅提取 CommonHeader）
 *  2. 基础合法性校验（Validator::validate，magic/version/dest_id）
 *  3. 打接收时间戳（由 ArrayFaceReceiver 在 recvmmsg 后已完成）
 *  4. 填充 RxEnvelope 并推入 SPSC 队列
 *
 * 绝对禁止在本类中：
 * - 大对象构造（Reassembler/Reorderer/Dispatcher）
 * - 复杂 map/unordered_map 查找
 * - 动态内存频繁分配
 * - 日志大量输出
 * - 下游业务判断
 *
 * 线程模型：
 * @code
 *  ArrayFaceReceiver::receive_loop() [CPU 16/17/18]
 *       │
 *       └──▶ RxStage::on_packet()   ← 本类，热路径唯一入口
 *                │
 *                ├── PacketParser::parse()      (memcpy 32B + pointer)
 *                ├── Validator::validate()       (几个 if 比较)
 *                ├── fill RxEnvelope             (字段赋值 + move)
 *                └── SPSCQueue::drop_oldest_push (一次 atomic store)
 * @endcode
 *
 * @see RxEnvelope  穿越队列的轻量数据单元
 * @see SPSCQueue   lock-free ring buffer (common/spsc_queue.h)
 */

#ifndef RECEIVER_PIPELINE_RX_STAGE_H
#define RECEIVER_PIPELINE_RX_STAGE_H

#include "qdgz300/m01_receiver/pipeline/rx_envelope.h"
#include "qdgz300/m01_receiver/network/udp_receiver.h"
#include "qdgz300/m01_receiver/protocol/packet_parser.h"
#include "qdgz300/m01_receiver/protocol/validator.h"
#include "qdgz300/common/spsc_queue.h"

#include <atomic>
#include <cstdint>

namespace receiver
{
    namespace pipeline
    {

        /**
         * @class RxStage
         * @brief 单阵面实时接收层
         *
         * 每个阵面（ArrayFaceReceiver）拥有一个独立的 RxStage 实例。
         * 内含自己的 PacketParser、Validator 和 SPSC 队列，无共享状态。
         */
        class RxStage
        {
        public:
            /// SPSC 队列容量（power-of-2，可存 65535 个信封）
            static constexpr size_t QUEUE_CAPACITY = 65536;

            /// 队列类型别名
            using Queue = qdgz300::SPSCQueue<RxEnvelope, QUEUE_CAPACITY>;

            /**
             * @brief 构造单阵面实时接收层
             *
             * @param array_id        阵面编号（1~3）
             * @param local_device_id 本机设备 ID（用于 Validator 的 dest_id 校验）
             */
            RxStage(uint8_t array_id, uint8_t local_device_id);

            ~RxStage() = default;

            RxStage(const RxStage &) = delete;
            RxStage &operator=(const RxStage &) = delete;

            /**
             * @brief 热路径入口——处理单个接收到的 UDP 报文
             *
             * 在 ArrayFaceReceiver 的收包线程（CPU 16/17/18）中被调用。
             * 执行流程：parse → validate → fill envelope → SPSC push
             *
             * @param raw_packet 从 ArrayFaceReceiver 收到的原始报文（右值移动）
             *
             * @note 此函数是性能最敏感的代码路径，所有操作必须 O(1) 且无分配。
             */
            void on_packet(network::ReceivedPacket &&raw_packet);

            /// 获取 SPSC 队列引用（供消费侧 pop）
            Queue &queue() noexcept { return queue_; }
            const Queue &queue() const noexcept { return queue_; }

            /**
             * @struct Stats
             * @brief 实时接收层统计快照
             */
            struct Stats
            {
                uint64_t rx_total;    ///< 收到的报文总数
                uint64_t parse_ok;    ///< 解析成功数
                uint64_t validate_ok; ///< 校验通过数
                uint64_t enqueued;    ///< 成功入队数
                uint64_t queue_drops; ///< 队列满丢弃数（drop-oldest 触发次数）
                size_t queue_high_watermark; ///< 历史最高队列深度
            };

            /**
             * @brief 获取当前统计快照
             * @return Stats 值对象（所有计数器使用 relaxed 读取）
             */
            Stats get_stats() const noexcept;

        private:
            uint8_t array_id_;
            protocol::PacketParser parser_;
            protocol::Validator validator_;
            Queue queue_;

            // 统计计数器（cache-line 对齐，避免与队列 head/tail 的 false sharing）
            alignas(64) std::atomic<uint64_t> rx_total_{0};
            std::atomic<uint64_t> parse_ok_{0};
            std::atomic<uint64_t> validate_ok_{0};
            std::atomic<uint64_t> enqueued_{0};
            std::atomic<size_t> queue_high_watermark_{0};
        };

    } // namespace pipeline
} // namespace receiver

#endif // RECEIVER_PIPELINE_RX_STAGE_H
