/**
 * @file rx_stage.cpp
 * @brief 实时接收层 (RxStage) 实现
 *
 * 本文件实现 RxStage::on_packet()——收包热路径的唯一处理函数。
 * 所有操作严格限制为 O(1)，无动态分配，无日志输出。
 */

#include "qdgz300/m01_receiver/pipeline/rx_stage.h"

namespace receiver
{
    namespace pipeline
    {

        RxStage::RxStage(uint8_t array_id, uint8_t local_device_id)
            : array_id_(array_id),
              parser_(),
              validator_(local_device_id, protocol::Validator::Scope::DATA_AND_HEARTBEAT),
              queue_()
        {
        }

        void RxStage::on_packet(network::ReceivedPacket &&raw_packet)
        {
            rx_total_.fetch_add(1, std::memory_order_relaxed);

            // ── Step 1: 协议头解析（memcpy 32B + 字段提取） ──────────
            auto parsed = parser_.parse(raw_packet.data.get(), raw_packet.length);
            if (!parsed.has_value())
            {
                return;
            }
            parse_ok_.fetch_add(1, std::memory_order_relaxed);

            // ── Step 2: 基础合法性校验（magic/version/dest_id） ──────
            auto result = validator_.validate(parsed.value());
            if (result != protocol::ValidationResult::OK)
            {
                return;
            }
            validate_ok_.fetch_add(1, std::memory_order_relaxed);

            // ── Step 3: 填充信封（字段赋值 + zero-copy move） ────────
            RxEnvelope env;
            env.header = parsed->header;
            env.array_id = array_id_;
            env.rx_timestamp_ns = raw_packet.receive_timestamp_ns;
            env.packet_length = static_cast<uint32_t>(raw_packet.length);
            env.packet_data = std::move(raw_packet.data);

            // ── Step 4: 推入 SPSC 队列（drop-oldest 溢出策略） ───────
            queue_.drop_oldest_push(std::move(env));
            enqueued_.fetch_add(1, std::memory_order_relaxed);
        }

        RxStage::Stats RxStage::get_stats() const noexcept
        {
            return Stats{
                rx_total_.load(std::memory_order_relaxed),
                parse_ok_.load(std::memory_order_relaxed),
                validate_ok_.load(std::memory_order_relaxed),
                enqueued_.load(std::memory_order_relaxed),
                queue_.drop_count()};
        }

    } // namespace pipeline
} // namespace receiver
