#ifndef RECEIVER_PIPELINE_REASSEMBLER_H
#define RECEIVER_PIPELINE_REASSEMBLER_H

#include "qdgz300/m01_receiver/network/common/numa_allocator.h"
#include "qdgz300/m01_receiver/network/packet_pool.h"
#include "qdgz300/m01_receiver/protocol/protocol_types.h"
#include <atomic>
#include <bitset>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace receiver
{
    namespace pipeline
    {

        /**
         * @brief 分片重组配置
         */
        struct ReassemblerConfig
        {
            uint32_t timeout_ms = 100;
            size_t max_contexts = 1024;
            uint16_t max_total_frags = 1024;
            size_t sample_count_fixed = 4096;
            size_t max_reasm_bytes_per_key = 16u * 1024u * 1024u;
            int numa_node = 1;
            size_t cache_align_bytes = 64;
            bool prefetch_hints_enabled = true;
        };

        /**
         * @brief 零拷贝分片引用——持有 PacketPool buffer 所有权，避免 memcpy
         *
         * 分片到达时将原始 PacketBuffer 的所有权转移到 FragmentRef 中，
         * 仅记录 payload 在 buffer 内的偏移和长度。实际数据拷贝延迟到
         * complete_reassembly() 中一次性顺序写入，比逐片随机写入更 cache-friendly。
         *
         * 对于无法提供 PacketBuffer 的向后兼容路径（如单元测试），
         * FragmentRef 也支持从原始指针拷贝构造（heap_buffer 回退）。
         */
        struct FragmentRef
        {
            /// 零拷贝路径：持有 PacketPool 分配的网络缓冲区
            network::PacketBuffer pool_buffer;
            /// 回退路径：持有堆分配的数据副本（仅在无 PacketBuffer 时使用）
            std::unique_ptr<uint8_t[]> heap_buffer;
            /// 指向实际 fragment payload 数据（pool_buffer 或 heap_buffer 内部）
            const uint8_t *data{nullptr};
            /// fragment payload 字节数
            size_t data_size{0};

            FragmentRef() = default;

            /// 零拷贝构造：从 PacketBuffer 移入，记录 payload 指针和大小
            FragmentRef(network::PacketBuffer &&buf, const uint8_t *payload, size_t size)
                : pool_buffer(std::move(buf)), data(payload), data_size(size) {}

            /// 回退构造：从原始指针拷贝到堆上（向后兼容）
            FragmentRef(const uint8_t *src, size_t size)
                : heap_buffer(std::make_unique<uint8_t[]>(size)), data_size(size)
            {
                std::memcpy(heap_buffer.get(), src, size);
                data = heap_buffer.get();
            }

            FragmentRef(const FragmentRef &) = delete;
            FragmentRef &operator=(const FragmentRef &) = delete;
            FragmentRef(FragmentRef &&) noexcept = default;
            FragmentRef &operator=(FragmentRef &&) noexcept = default;

            bool has_data() const { return data != nullptr && data_size > 0; }
        };

        /**
         * @brief 重组上下文
         */
        struct alignas(64) ReassemblyContext
        {
            protocol::ReassemblyKey key;
            uint16_t total_frags;
            uint16_t tail_frag_payload_bytes;
            uint64_t first_frag_time_ms;
            uint64_t data_timestamp;
            protocol::ExecutionSnapshot execution_snapshot;
            bool has_execution_snapshot;

            std::bitset<1024> received_fragments;
            std::vector<size_t> fragment_sizes;
            /// 零拷贝分片引用表——每个槽对应一个 frag_index，持有原始 buffer 所有权
            std::vector<FragmentRef> fragment_refs;
            size_t total_payload_size;

            uint16_t received_count;
            /// 已接收的分片 payload 字节总量（用于 max_reasm_bytes_per_key 溢出检查）
            size_t received_payload_bytes;
            bool is_complete;
            bool is_timeout;

            /**
             * @brief 默认构造重组上下文
             */
            ReassemblyContext()
                : total_frags(0), tail_frag_payload_bytes(0),
                  first_frag_time_ms(0), data_timestamp(0),
                  execution_snapshot{}, has_execution_snapshot(false),
                  total_payload_size(0),
                  received_count(0), received_payload_bytes(0),
                  is_complete(false), is_timeout(false) {}

            /**
             * @brief 判断是否收齐全部分片
             * @return 已收齐返回true
             */
            bool check_complete() const
            {
                return received_count == total_frags;
            }

            /**
             * @brief 判断重组上下文是否超时
             * @param current_time_ms 当前时间（毫秒）
             * @param timeout_ms 超时阈值（毫秒）
             * @return 超时返回true
             */
            bool check_timeout(uint64_t current_time_ms, uint32_t timeout_ms) const
            {
                return (current_time_ms - first_frag_time_ms) >= timeout_ms;
            }
        };

        static_assert(alignof(ReassemblyContext) == 64, "ReassemblyContext must be cache-line aligned");

        /**
         * @brief 重组完成帧
         */
        struct ReassembledFrame
        {
            protocol::ReassemblyKey key;
            std::unique_ptr<uint8_t[]> data;
            size_t total_size;
            uint64_t data_timestamp;
            protocol::ExecutionSnapshot execution_snapshot;
            bool has_execution_snapshot;
            bool is_complete;
            uint16_t missing_fragments_count;
            std::vector<uint16_t> missing_frag_indices;

            /**
             * @brief 默认构造重组结果帧
             */
            ReassembledFrame()
                : data(nullptr), total_size(0), data_timestamp(0),
                  execution_snapshot{}, has_execution_snapshot(false),
                  is_complete(true), missing_fragments_count(0) {}
        };

        /**
         * @brief 分片重组器
         *
         * 职责：
         * - 按重组键聚合分片并拼装完整帧
         * - 检测重复/迟到/超时分片
         * - 输出完整帧与统计信息
         */
        class Reassembler
        {
        public:
            using FrameCompleteCallback = std::function<void(ReassembledFrame &&)>;

            /**
             * @brief 构造重组器
             * @param config 重组配置
             * @param callback 帧完成回调
             */
            explicit Reassembler(const ReassemblerConfig &config, FrameCompleteCallback callback);

            /**
             * @brief 析构重组器
             */
            ~Reassembler();

            /**
             * @brief 禁止拷贝构造
             */
            Reassembler(const Reassembler &) = delete;

            /**
             * @brief 禁止拷贝赋值
             * @return Reassembler&
             */
            Reassembler &operator=(const Reassembler &) = delete;

            /**
             * @brief 处理一个待重组数据包（向后兼容路径，内部拷贝 payload）
             * @param packet 已解析数据包
             * @return void
             */
            void process_packet(const protocol::ParsedPacket &packet);

            /**
             * @brief 零拷贝处理——接管 PacketBuffer 所有权，避免分片 memcpy
             *
             * 生产环境热路径入口。PacketBuffer 在分片重组期间保持引用，
             * 仅在 complete_reassembly() 中执行一次顺序拷贝组装最终帧。
             *
             * @param packet 已解析数据包（payload 指针指向 buffer 内部）
             * @param buffer 从 PacketPool 分配的网络缓冲区（所有权转移）
             */
            void process_packet_zero_copy(const protocol::ParsedPacket &packet,
                                          network::PacketBuffer &&buffer);

            /**
             * @brief 检查并清理超时上下文
             * @return void
             */
            void check_timeouts();

            /**
             * @brief 动态设置重组超时时间
             * @param timeout_ms 超时时间（毫秒）
             * @return void
             */
            void set_timeout_ms(uint32_t timeout_ms);

            /**
             * @brief 强制刷新全部上下文
             * @return 被刷新并输出的上下文数量
             */
            size_t flush_all();

            struct Statistics
            {
                std::atomic<uint64_t> total_fragments_received{0};
                std::atomic<uint64_t> duplicate_fragments{0};
                std::atomic<uint64_t> late_fragments{0};
                std::atomic<uint64_t> frames_completed{0};
                std::atomic<uint64_t> frames_incomplete{0};
                std::atomic<uint64_t> total_missing_fragments{0};
                std::atomic<uint64_t> contexts_created{0};
                std::atomic<uint64_t> contexts_destroyed{0};
                std::atomic<uint64_t> contexts_overflow{0};
                std::atomic<uint64_t> reasm_bytes_overflow{0};
            };

            /**
             * @brief 获取重组统计信息
             * @return 当前统计信息的只读引用
             */
            const Statistics &get_statistics() const { return stats_; }

            /**
             * @brief 重置重组统计信息
             * @return void
             */
            void reset_statistics();

        private:
            protocol::ReassemblyKey extract_key(const protocol::CommonHeader &common,
                                                const protocol::DataSpecificHeader &specific) const;

            bool parse_specific_header(const uint8_t *payload, size_t payload_len,
                                       protocol::DataSpecificHeader &header) const;

            ReassemblyContext *get_or_create_context(const protocol::ReassemblyKey &key,
                                                     uint16_t total_frags,
                                                     uint16_t tail_frag_payload_bytes,
                                                     uint64_t data_timestamp,
                                                     uint64_t receive_time_ms);

            bool add_fragment(ReassemblyContext *ctx,
                              const protocol::DataSpecificHeader &header,
                              const uint8_t *fragment_payload,
                              size_t fragment_payload_size,
                              uint64_t receive_time_ms);

            bool add_fragment_zero_copy(ReassemblyContext *ctx,
                                        const protocol::DataSpecificHeader &header,
                                        network::PacketBuffer &&buffer,
                                        const uint8_t *fragment_payload,
                                        size_t fragment_payload_size,
                                        uint64_t receive_time_ms);

            void complete_reassembly(const protocol::ReassemblyKey &key, ReassemblyContext *ctx, bool is_timeout);

            size_t calculate_fragment_payload_size(uint16_t frag_index,
                                                   uint16_t total_frags,
                                                   uint16_t tail_frag_payload_bytes,
                                                   uint8_t channel_count,
                                                   uint8_t data_type) const;

            size_t calculate_total_payload_size(uint16_t total_frags,
                                                uint16_t tail_frag_payload_bytes,
                                                uint8_t channel_count,
                                                uint8_t data_type) const;

            size_t calculate_fragment_offset(uint16_t frag_index,
                                             uint16_t total_frags,
                                             uint16_t tail_frag_payload_bytes,
                                             uint8_t channel_count,
                                             uint8_t data_type) const;

            size_t get_complex_size(uint8_t data_type) const;
            void destroy_context(const protocol::ReassemblyKey &key);
            bool is_key_frozen(const protocol::ReassemblyKey &key, uint64_t now_ms);
            void add_frozen_key(const protocol::ReassemblyKey &key, uint64_t expire_ms);

            ReassemblerConfig config_;
            FrameCompleteCallback callback_;
            using ContextMapValue = std::pair<const protocol::ReassemblyKey, std::unique_ptr<ReassemblyContext>>;
            using ContextMapAllocator = network::common::NumaAllocator<ContextMapValue>;
            using ContextMap = std::unordered_map<protocol::ReassemblyKey,
                                                  std::unique_ptr<ReassemblyContext>,
                                                  protocol::ReassemblyKeyHash,
                                                  std::equal_to<protocol::ReassemblyKey>,
                                                  ContextMapAllocator>;
            ContextMap contexts_;
            struct FrozenKeyEntry
            {
                protocol::ReassemblyKey key{};
                uint64_t expire_ms{0};
                bool valid{false};
            };
            std::vector<FrozenKeyEntry> frozen_keys_ring_;
            size_t frozen_keys_next_insert_{0};
            Statistics stats_;
            std::atomic<uint32_t> timeout_ms_runtime_{100};

            uint64_t get_current_time_ms() const;
        };

    } // namespace pipeline
} // namespace receiver

#endif // RECEIVER_PIPELINE_REASSEMBLER_H
