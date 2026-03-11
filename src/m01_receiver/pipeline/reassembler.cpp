#include "qdgz300/m01_receiver/pipeline/reassembler.h"
#include <algorithm>
#include <chrono>
#include <cstring>

namespace receiver
{
    namespace pipeline
    {

        Reassembler::Reassembler(const ReassemblerConfig &config, FrameCompleteCallback callback)
            : config_(config),
              callback_(std::move(callback)),
              contexts_(0,
                        protocol::ReassemblyKeyHash{},
                        std::equal_to<protocol::ReassemblyKey>{},
                        ContextMapAllocator(config.numa_node))
        {
            timeout_ms_runtime_.store(config_.timeout_ms, std::memory_order_relaxed); // runtime scalar init, no ordering dependency
            contexts_.reserve(config_.max_contexts);
            const size_t frozen_capacity = std::max<size_t>(1, config_.max_contexts * 2);
            frozen_keys_ring_.resize(frozen_capacity);
        }

        Reassembler::~Reassembler() = default;

        void Reassembler::process_packet(const protocol::ParsedPacket &packet)
        {
            stats_.total_fragments_received.fetch_add(1, std::memory_order_relaxed); // counter-only, no ordering needed

            protocol::DataSpecificHeader specific;
            if (!parse_specific_header(packet.payload, packet.header.payload_len, specific))
            {
                return;
            }

            if (specific.total_frags == 0 || specific.total_frags > config_.max_total_frags)
            {
                return;
            }

            const size_t specific_size = sizeof(protocol::DataSpecificHeader);
            const size_t snapshot_size = sizeof(protocol::ExecutionSnapshot);
            const bool is_tail_fragment = specific.is_tail_fragment();
            const uint8_t *fragment_payload = nullptr;
            size_t fragment_payload_size = 0;
            protocol::ExecutionSnapshot tail_snapshot{};
            bool has_tail_snapshot = false;

            if (is_tail_fragment)
            {
                const size_t expected_payload_len =
                    specific_size + snapshot_size + static_cast<size_t>(specific.tail_frag_payload_bytes);
                if (packet.header.payload_len != expected_payload_len)
                {
                    return;
                }

                const uint8_t *snapshot_ptr = packet.payload + specific_size;
                std::memcpy(&tail_snapshot, snapshot_ptr, snapshot_size);
                has_tail_snapshot = true;

                fragment_payload = packet.payload + specific_size + snapshot_size;
                fragment_payload_size = packet.header.payload_len - (specific_size + snapshot_size);
            }
            else
            {
                if (packet.header.payload_len < specific_size)
                {
                    return;
                }
                fragment_payload = packet.payload + specific_size;
                fragment_payload_size = packet.header.payload_len - specific_size;
            }

            const protocol::ReassemblyKey key = extract_key(packet.header, specific);
            const uint64_t now_ms = get_current_time_ms();
            if (is_key_frozen(key, now_ms))
            {
                stats_.late_fragments.fetch_add(1, std::memory_order_relaxed); // counter-only, no ordering needed
                return;
            }
            ReassemblyContext *ctx = get_or_create_context(
                key,
                specific.total_frags,
                specific.tail_frag_payload_bytes,
                specific.data_timestamp,
                now_ms);
            if (ctx == nullptr)
            {
                return;
            }

            if (has_tail_snapshot)
            {
                ctx->execution_snapshot = tail_snapshot;
                ctx->has_execution_snapshot = true;
            }

            if (!add_fragment(ctx, specific, fragment_payload, fragment_payload_size, now_ms))
            {
                return;
            }

            if (ctx->check_complete())
            {
                complete_reassembly(key, ctx, false);
            }
        }

        void Reassembler::process_packet_zero_copy(const protocol::ParsedPacket &packet,
                                                     network::PacketBuffer &&buffer)
        {
            stats_.total_fragments_received.fetch_add(1, std::memory_order_relaxed);

            protocol::DataSpecificHeader specific;
            if (!parse_specific_header(packet.payload, packet.header.payload_len, specific))
            {
                return;
            }

            if (specific.total_frags == 0 || specific.total_frags > config_.max_total_frags)
            {
                return;
            }

            const size_t specific_size = sizeof(protocol::DataSpecificHeader);
            const size_t snapshot_size = sizeof(protocol::ExecutionSnapshot);
            const bool is_tail_fragment = specific.is_tail_fragment();
            const uint8_t *fragment_payload = nullptr;
            size_t fragment_payload_size = 0;
            protocol::ExecutionSnapshot tail_snapshot{};
            bool has_tail_snapshot = false;

            if (is_tail_fragment)
            {
                const size_t expected_payload_len =
                    specific_size + snapshot_size + static_cast<size_t>(specific.tail_frag_payload_bytes);
                if (packet.header.payload_len != expected_payload_len)
                {
                    return;
                }

                const uint8_t *snapshot_ptr = packet.payload + specific_size;
                std::memcpy(&tail_snapshot, snapshot_ptr, snapshot_size);
                has_tail_snapshot = true;

                fragment_payload = packet.payload + specific_size + snapshot_size;
                fragment_payload_size = packet.header.payload_len - (specific_size + snapshot_size);
            }
            else
            {
                if (packet.header.payload_len < specific_size)
                {
                    return;
                }
                fragment_payload = packet.payload + specific_size;
                fragment_payload_size = packet.header.payload_len - specific_size;
            }

            const protocol::ReassemblyKey key = extract_key(packet.header, specific);
            const uint64_t now_ms = get_current_time_ms();
            if (is_key_frozen(key, now_ms))
            {
                stats_.late_fragments.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            ReassemblyContext *ctx = get_or_create_context(
                key,
                specific.total_frags,
                specific.tail_frag_payload_bytes,
                specific.data_timestamp,
                now_ms);
            if (ctx == nullptr)
            {
                return;
            }

            if (has_tail_snapshot)
            {
                ctx->execution_snapshot = tail_snapshot;
                ctx->has_execution_snapshot = true;
            }

            if (!add_fragment_zero_copy(ctx, specific, std::move(buffer),
                                         fragment_payload, fragment_payload_size, now_ms))
            {
                return;
            }

            if (ctx->check_complete())
            {
                complete_reassembly(key, ctx, false);
            }
        }

        void Reassembler::check_timeouts()
        {
            const uint64_t now_ms = get_current_time_ms();
            const uint32_t timeout_ms = timeout_ms_runtime_.load(std::memory_order_relaxed); // runtime scalar, ordering independent
            std::vector<protocol::ReassemblyKey> timed_out;
            timed_out.reserve(contexts_.size());

            for (const auto &entry : contexts_)
            {
                const ReassemblyContext &ctx = *entry.second;
                if (ctx.check_timeout(now_ms, timeout_ms))
                {
                    timed_out.push_back(entry.first);
                }
            }

            for (const auto &key : timed_out)
            {
                auto it = contexts_.find(key);
                if (it != contexts_.end())
                {
                    complete_reassembly(key, it->second.get(), true);
                }
            }
        }

        size_t Reassembler::flush_all()
        {
            std::vector<protocol::ReassemblyKey> keys;
            keys.reserve(contexts_.size());
            for (const auto &entry : contexts_)
            {
                keys.push_back(entry.first);
            }

            for (const auto &key : keys)
            {
                auto it = contexts_.find(key);
                if (it != contexts_.end())
                {
                    complete_reassembly(key, it->second.get(), true);
                }
            }

            return keys.size();
        }

        void Reassembler::reset_statistics()
        {
            stats_.total_fragments_received.store(0, std::memory_order_relaxed); // counter reset, no ordering needed
            stats_.duplicate_fragments.store(0, std::memory_order_relaxed); // counter reset, no ordering needed
            stats_.late_fragments.store(0, std::memory_order_relaxed); // counter reset, no ordering needed
            stats_.frames_completed.store(0, std::memory_order_relaxed); // counter reset, no ordering needed
            stats_.frames_incomplete.store(0, std::memory_order_relaxed); // counter reset, no ordering needed
            stats_.total_missing_fragments.store(0, std::memory_order_relaxed); // counter reset, no ordering needed
            stats_.contexts_created.store(0, std::memory_order_relaxed); // counter reset, no ordering needed
            stats_.contexts_destroyed.store(0, std::memory_order_relaxed); // counter reset, no ordering needed
            stats_.contexts_overflow.store(0, std::memory_order_relaxed); // counter reset, no ordering needed
            stats_.reasm_bytes_overflow.store(0, std::memory_order_relaxed); // counter reset, no ordering needed
            for (auto &entry : frozen_keys_ring_)
            {
                entry.valid = false;
                entry.expire_ms = 0;
            }
            frozen_keys_next_insert_ = 0;
        }

        protocol::ReassemblyKey Reassembler::extract_key(
            const protocol::CommonHeader &common,
            const protocol::DataSpecificHeader &specific) const
        {
            protocol::ReassemblyKey key;
            key.control_epoch = common.control_epoch;
            key.source_id = common.source_id;
            key.frame_counter = specific.frame_counter;
            key.beam_id = specific.beam_id;
            key.cpi_count = specific.cpi_count;
            key.pulse_index = specific.pulse_index;
            key.channel_mask = specific.get_channel_mask();
            key.data_type = specific.get_data_type_raw();
            return key;
        }

        bool Reassembler::parse_specific_header(const uint8_t *payload, size_t payload_len,
                                                protocol::DataSpecificHeader &header) const
        {
            if (payload == nullptr || payload_len < sizeof(protocol::DataSpecificHeader))
            {
                return false;
            }

            std::memcpy(&header, payload, sizeof(header));
            if (header.total_frags == 0 || header.total_frags > config_.max_total_frags)
            {
                return false;
            }
            if (header.frag_index >= header.total_frags)
            {
                return false;
            }
            return true;
        }

        ReassemblyContext *Reassembler::get_or_create_context(
            const protocol::ReassemblyKey &key,
            uint16_t total_frags,
            uint16_t tail_frag_payload_bytes,
            uint64_t data_timestamp,
            uint64_t receive_time_ms)
        {
            auto it = contexts_.find(key);
            if (it != contexts_.end())
            {
                return it->second.get();
            }

            if (contexts_.size() >= config_.max_contexts)
            {
                stats_.contexts_overflow.fetch_add(1, std::memory_order_relaxed); // counter-only, no ordering needed
                return nullptr;
            }

            auto ctx = std::make_unique<ReassemblyContext>();
            ctx->key = key;
            ctx->total_frags = total_frags;
            ctx->tail_frag_payload_bytes = tail_frag_payload_bytes;
            ctx->first_frag_time_ms = receive_time_ms;
            ctx->data_timestamp = data_timestamp;
            ctx->fragment_sizes.assign(total_frags, 0);
            ctx->fragment_refs.resize(total_frags);
            const uint8_t channel_count = static_cast<uint8_t>(protocol::popcount16(key.channel_mask));
            ctx->total_payload_size = calculate_total_payload_size(
                total_frags,
                tail_frag_payload_bytes,
                channel_count,
                key.data_type);

            ReassemblyContext *ptr = ctx.get();
            contexts_.emplace(key, std::move(ctx));
            stats_.contexts_created.fetch_add(1, std::memory_order_relaxed); // counter-only, no ordering needed
            return ptr;
        }

        bool Reassembler::add_fragment(ReassemblyContext *ctx,
                                       const protocol::DataSpecificHeader &header,
                                       const uint8_t *fragment_payload,
                                       size_t fragment_payload_size,
                                       uint64_t receive_time_ms)
        {
            if (ctx == nullptr || fragment_payload == nullptr)
            {
                return false;
            }

            if (header.frag_index >= ctx->total_frags || header.frag_index >= 1024)
            {
                return false;
            }

            if (ctx->received_fragments.test(header.frag_index))
            {
                stats_.duplicate_fragments.fetch_add(1, std::memory_order_relaxed); // counter-only, no ordering needed
                return false;
            }

            if (ctx->received_payload_bytes + fragment_payload_size > config_.max_reasm_bytes_per_key)
            {
                stats_.reasm_bytes_overflow.fetch_add(1, std::memory_order_relaxed); // counter-only, no ordering needed
                return false;
            }

            const uint8_t channel_count = static_cast<uint8_t>(protocol::popcount16(ctx->key.channel_mask));
            const size_t expected_size = calculate_fragment_payload_size(
                header.frag_index,
                ctx->total_frags,
                ctx->tail_frag_payload_bytes,
                channel_count,
                ctx->key.data_type);
            if (fragment_payload_size != expected_size)
            {
                return false;
            }

            // 向后兼容路径：从原始指针拷贝构造 FragmentRef（堆分配回退）
            if (fragment_payload_size > 0 && fragment_payload != nullptr)
            {
                ctx->fragment_refs[header.frag_index] = FragmentRef(fragment_payload, fragment_payload_size);
            }

            ctx->fragment_sizes[header.frag_index] = fragment_payload_size;
            ctx->received_fragments.set(header.frag_index);
            ctx->received_count++;
            ctx->received_payload_bytes += fragment_payload_size;
            ctx->is_complete = ctx->check_complete();
            return true;
        }

        bool Reassembler::add_fragment_zero_copy(ReassemblyContext *ctx,
                                                  const protocol::DataSpecificHeader &header,
                                                  network::PacketBuffer &&buffer,
                                                  const uint8_t *fragment_payload,
                                                  size_t fragment_payload_size,
                                                  uint64_t receive_time_ms)
        {
            if (ctx == nullptr || fragment_payload == nullptr)
            {
                return false;
            }

            if (header.frag_index >= ctx->total_frags || header.frag_index >= 1024)
            {
                return false;
            }

            if (ctx->received_fragments.test(header.frag_index))
            {
                stats_.duplicate_fragments.fetch_add(1, std::memory_order_relaxed);
                return false;
            }

            if (ctx->received_payload_bytes + fragment_payload_size > config_.max_reasm_bytes_per_key)
            {
                stats_.reasm_bytes_overflow.fetch_add(1, std::memory_order_relaxed);
                return false;
            }

            const uint8_t channel_count = static_cast<uint8_t>(protocol::popcount16(ctx->key.channel_mask));
            const size_t expected_size = calculate_fragment_payload_size(
                header.frag_index,
                ctx->total_frags,
                ctx->tail_frag_payload_bytes,
                channel_count,
                ctx->key.data_type);
            if (fragment_payload_size != expected_size)
            {
                return false;
            }

            // 零拷贝路径：转移 PacketBuffer 所有权，记录 payload 指针和大小
            if (fragment_payload_size > 0)
            {
                ctx->fragment_refs[header.frag_index] = FragmentRef(
                    std::move(buffer), fragment_payload, fragment_payload_size);
            }

            ctx->fragment_sizes[header.frag_index] = fragment_payload_size;
            ctx->received_fragments.set(header.frag_index);
            ctx->received_count++;
            ctx->received_payload_bytes += fragment_payload_size;
            ctx->is_complete = ctx->check_complete();
            return true;
        }

        void Reassembler::complete_reassembly(const protocol::ReassemblyKey &key,
                                              ReassemblyContext *ctx,
                                              bool is_timeout)
        {
            if (ctx == nullptr)
            {
                return;
            }

            ReassembledFrame frame;
            frame.key = key;
            frame.data_timestamp = ctx->data_timestamp;
            frame.execution_snapshot = ctx->execution_snapshot;
            frame.has_execution_snapshot = ctx->has_execution_snapshot;
            frame.total_size = ctx->total_payload_size;

            // 从 fragment_refs 一次性顺序组装最终帧缓冲区
            const uint8_t channel_count = static_cast<uint8_t>(protocol::popcount16(key.channel_mask));
            auto assembled = std::make_unique<uint8_t[]>(ctx->total_payload_size);
            size_t write_pos = 0;
            for (uint16_t i = 0; i < ctx->total_frags; ++i)
            {
                const size_t frag_expected_size = calculate_fragment_payload_size(
                    i, ctx->total_frags, ctx->tail_frag_payload_bytes,
                    channel_count, key.data_type);

                if (ctx->received_fragments.test(i) &&
                    i < ctx->fragment_refs.size() &&
                    ctx->fragment_refs[i].has_data())
                {
                    // 已接收分片：顺序拷贝（单次，cache-friendly）
                    std::memcpy(assembled.get() + write_pos,
                                ctx->fragment_refs[i].data,
                                ctx->fragment_refs[i].data_size);
                }
                else
                {
                    // 缺失分片：零填充
                    std::memset(assembled.get() + write_pos, 0, frag_expected_size);
                    frame.is_complete = false;
                    frame.missing_frag_indices.push_back(i);
                }
                write_pos += frag_expected_size;
            }
            frame.data = std::move(assembled);

            frame.missing_fragments_count = static_cast<uint16_t>(frame.missing_frag_indices.size());

            if (frame.missing_fragments_count == 0 && !is_timeout)
            {
                stats_.frames_completed.fetch_add(1, std::memory_order_relaxed); // counter-only, no ordering needed
            }
            else
            {
                stats_.frames_incomplete.fetch_add(1, std::memory_order_relaxed); // counter-only, no ordering needed
                stats_.total_missing_fragments.fetch_add(frame.missing_fragments_count, std::memory_order_relaxed); // counter-only, no ordering needed
            }

            if (callback_)
            {
                callback_(std::move(frame));
            }

            if (is_timeout)
            {
                const uint32_t timeout_ms = timeout_ms_runtime_.load(std::memory_order_relaxed); // runtime scalar, ordering independent
                add_frozen_key(key, get_current_time_ms() + timeout_ms);
            }

            destroy_context(key);
        }

        size_t Reassembler::calculate_fragment_payload_size(uint16_t frag_index,
                                                            uint16_t total_frags,
                                                            uint16_t tail_frag_payload_bytes,
                                                            uint8_t channel_count,
                                                            uint8_t data_type) const
        {
            if (frag_index + 1 == total_frags)
            {
                return tail_frag_payload_bytes;
            }

            return config_.sample_count_fixed * get_complex_size(data_type) * channel_count;
        }

        size_t Reassembler::calculate_total_payload_size(uint16_t total_frags,
                                                         uint16_t tail_frag_payload_bytes,
                                                         uint8_t channel_count,
                                                         uint8_t data_type) const
        {
            size_t total_size = 0;
            for (uint16_t i = 0; i < total_frags; ++i)
            {
                total_size += calculate_fragment_payload_size(
                    i,
                    total_frags,
                    tail_frag_payload_bytes,
                    channel_count,
                    data_type);
            }
            return total_size;
        }

        size_t Reassembler::calculate_fragment_offset(uint16_t frag_index,
                                                      uint16_t total_frags,
                                                      uint16_t tail_frag_payload_bytes,
                                                      uint8_t channel_count,
                                                      uint8_t data_type) const
        {
            size_t offset = 0;
            for (uint16_t i = 0; i < frag_index; ++i)
            {
                offset += calculate_fragment_payload_size(
                    i,
                    total_frags,
                    tail_frag_payload_bytes,
                    channel_count,
                    data_type);
            }
            return offset;
        }

        size_t Reassembler::get_complex_size(uint8_t data_type) const
        {
            switch (data_type)
            {
            case 0x00:
            case 0x01:
                return 4;
            case 0x02:
                return 8;
            default:
                return 4;
            }
        }

        void Reassembler::destroy_context(const protocol::ReassemblyKey &key)
        {
            auto it = contexts_.find(key);
            if (it != contexts_.end())
            {
                contexts_.erase(it);
                stats_.contexts_destroyed.fetch_add(1, std::memory_order_relaxed); // counter-only, no ordering needed
            }
        }

        bool Reassembler::is_key_frozen(const protocol::ReassemblyKey &key, uint64_t now_ms)
        {
            for (auto &entry : frozen_keys_ring_)
            {
                if (!entry.valid)
                {
                    continue;
                }

                if (entry.expire_ms <= now_ms)
                {
                    entry.valid = false;
                    continue;
                }

                if (entry.key == key)
                {
                    return true;
                }
            }
            return false;
        }

        void Reassembler::add_frozen_key(const protocol::ReassemblyKey &key, uint64_t expire_ms)
        {
            for (auto &entry : frozen_keys_ring_)
            {
                if (entry.valid && entry.key == key)
                {
                    entry.expire_ms = expire_ms;
                    return;
                }
            }

            FrozenKeyEntry &slot = frozen_keys_ring_[frozen_keys_next_insert_];
            slot.key = key;
            slot.expire_ms = expire_ms;
            slot.valid = true;
            frozen_keys_next_insert_ = (frozen_keys_next_insert_ + 1) % frozen_keys_ring_.size();
        }

        uint64_t Reassembler::get_current_time_ms() const
        {
            const auto now = std::chrono::system_clock::now();
            return static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
        }

        void Reassembler::set_timeout_ms(uint32_t timeout_ms)
        {
            timeout_ms_runtime_.store(timeout_ms, std::memory_order_relaxed); // runtime scalar update, no ordering dependency
        }

    } // namespace pipeline
} // namespace receiver
