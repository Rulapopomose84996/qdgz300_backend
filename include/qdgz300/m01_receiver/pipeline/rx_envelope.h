/**
 * @file rx_envelope.h
 * @brief 实时接收层轻量信封——穿越 SPSC 队列的最小数据单元
 *
 * RxEnvelope 是实时接收层 (RxStage) 产出、后续处理层消费的唯一数据结构。
 * 设计原则：
 * - 仅携带已解析的通用报文头 + 接收元数据 + 零拷贝原始报文指针
 * - 禁止大对象构造、禁止动态内存分配
 * - 固定大小、cache-friendly
 * - Move-only（PacketBuffer 持有 PacketPool 资源）
 *
 * 数据流：
 * @code
 *  ArrayFaceReceiver (CPU 16/17/18)
 *       │ recvmmsg
 *       ▼
 *  RxStage::on_packet()
 *       │ parse ──▶ validate ──▶ fill envelope ──▶ SPSC push
 *       ▼
 *  SPSCQueue<RxEnvelope, 8192>
 *       │
 *       ▼  (processing thread, no dedicated core)
 *  Dispatcher ──▶ Reassembler ──▶ Reorderer ──▶ Delivery
 * @endcode
 */

#ifndef RECEIVER_PIPELINE_RX_ENVELOPE_H
#define RECEIVER_PIPELINE_RX_ENVELOPE_H

#include "qdgz300/m01_receiver/network/packet_pool.h"
#include "qdgz300/m01_receiver/protocol/protocol_types.h"
#include <cstdint>

namespace receiver
{
    namespace pipeline
    {

        /**
         * @struct RxEnvelope
         * @brief 实时接收层输出的轻量信封
         *
         * 字段分为三层：
         * 1. 已解析的协议头（CommonHeader，32 bytes packed）——避免消费侧重复解析
         * 2. 接收元数据（阵面编号、本地时间戳、报文长度）
         * 3. 零拷贝报文缓冲区（PacketBuffer，持有 PacketPool 分配的内存）
         *
         * sizeof(RxEnvelope) ≈ 80 bytes（含 PacketBuffer 的两个指针）
         */
        struct RxEnvelope
        {
            /// 已解析的 32 字节通用报文头（由 PacketParser 填充）
            protocol::CommonHeader header{};

            /// 来源阵面编号（1~3）
            uint8_t array_id{0};

            /// 本地接收时间戳（纳秒，CLOCK_MONOTONIC / steady_clock）
            uint64_t rx_timestamp_ns{0};

            /// 原始 UDP 报文总长度（字节）
            uint32_t packet_length{0};

            /// 零拷贝报文数据（从 PacketPool 分配，持有所有权）
            network::PacketBuffer packet_data;
            
            // ── 生命周期：默认构造 + move-only ──────────────────────
            RxEnvelope() = default;
            ~RxEnvelope() = default;

            RxEnvelope(const RxEnvelope &) = delete;
            RxEnvelope &operator=(const RxEnvelope &) = delete;

            RxEnvelope(RxEnvelope &&) noexcept = default;
            RxEnvelope &operator=(RxEnvelope &&) noexcept = default;
        };

    } // namespace pipeline
} // namespace receiver

#endif // RECEIVER_PIPELINE_RX_ENVELOPE_H
