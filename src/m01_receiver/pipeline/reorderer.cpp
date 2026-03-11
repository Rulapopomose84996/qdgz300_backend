#include "qdgz300/m01_receiver/pipeline/reorderer.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

namespace receiver
{
    namespace pipeline
    {

        class Reorderer::Impl
        {
        public:
            struct StoredPacket
            {
                uint32_t sequence{0};
                protocol::CommonHeader header;
                std::unique_ptr<uint8_t[]> payload;
                size_t payload_size{0};
                bool occupied{false};
            };

            bool initialized{false};
            ReorderWindow window{};
            size_t base_index{0};
            std::vector<StoredPacket> ring_slots;
            std::vector<uint64_t> occupancy_bitmap;
            std::chrono::steady_clock::time_point last_advance_time{std::chrono::steady_clock::now()};
            Statistics stats;

            bool bit_test(size_t index) const
            {
                const size_t word = index / 64;
                const size_t bit = index % 64;
                return (occupancy_bitmap[word] & (1ull << bit)) != 0;
            }

            void bit_set(size_t index)
            {
                const size_t word = index / 64;
                const size_t bit = index % 64;
                occupancy_bitmap[word] |= (1ull << bit);
            }

            void bit_clear(size_t index)
            {
                const size_t word = index / 64;
                const size_t bit = index % 64;
                occupancy_bitmap[word] &= ~(1ull << bit);
            }
        };

        Reorderer::Reorderer(const ReorderConfig &config, OutputCallback output_callback)
            : impl_(std::make_unique<Impl>()), config_(config), output_callback_(std::move(output_callback))
        {
            const size_t window_size = std::max<size_t>(1, config_.window_size);
            impl_->ring_slots.resize(window_size);
            impl_->occupancy_bitmap.assign((window_size + 63) / 64, 0);
            timeout_ms_runtime_.store(config_.timeout_ms, std::memory_order_release); // publish initial timeout to readers
        }

        Reorderer::~Reorderer() = default;

        void Reorderer::insert(const protocol::ParsedPacket &packet)
        {
            const uint32_t seq = packet.header.sequence_number;
            if (!impl_->initialized)
            {
                impl_->initialized = true;
                impl_->window.next_expected_seq = seq;
            }

            const uint32_t expected = impl_->window.next_expected_seq;
            if (seq == expected)
            {
                OrderedPacket out;
                out.packet.header = packet.header;
                const size_t payload_len = packet.header.payload_len;
                if (payload_len > 0 && packet.payload != nullptr)
                {
                    out.owned_payload = std::make_unique<uint8_t[]>(payload_len);
                    std::memcpy(out.owned_payload.get(), packet.payload, payload_len);
                    out.payload_size = payload_len;
                    out.packet.payload = out.owned_payload.get();
                }
                else
                {
                    out.payload_size = 0;
                    out.packet.payload = nullptr;
                }
                out.packet.total_size = protocol::COMMON_HEADER_SIZE + out.payload_size;
                out.is_zero_filled = false;
                out.is_incomplete_frame = (packet.header.ext_flags & 0x01u) != 0;
                out.sequence_number = seq;
                if (output_callback_)
                {
                    output_callback_(std::move(out));
                }

                impl_->stats.packets_in_order++;
                impl_->stats.sequences_advanced++;
                impl_->window.next_expected_seq = impl_->window.next_expected_seq + 1;
                impl_->base_index = (impl_->base_index + 1) % impl_->ring_slots.size();
                impl_->last_advance_time = std::chrono::steady_clock::now();
                impl_->window.last_advance_ns = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        impl_->last_advance_time.time_since_epoch())
                        .count());
                advance_window();
                impl_->window.buffered_packets = static_cast<uint32_t>(impl_->stats.current_buffer_size);
                impl_->stats.current_buffer_size = impl_->window.buffered_packets;
                return;
            }

            if (!protocol::is_sequence_newer(seq, expected))
            {
                impl_->stats.packets_duplicate++;
                return;
            }

            const uint32_t delta = seq - expected;
            if (delta >= static_cast<uint32_t>(impl_->ring_slots.size()))
            {
                impl_->stats.packets_duplicate++;
                return;
            }

            const size_t slot_index = (impl_->base_index + static_cast<size_t>(delta)) % impl_->ring_slots.size();
            if (impl_->bit_test(slot_index))
            {
                const auto &existing = impl_->ring_slots[slot_index];
                if (existing.occupied && existing.sequence == seq)
                {
                    impl_->stats.packets_duplicate++;
                }
                else
                {
                    impl_->stats.packets_duplicate++;
                }
                return;
            }

            Impl::StoredPacket stored;
            stored.sequence = seq;
            stored.header = packet.header;
            const size_t payload_len = packet.header.payload_len;
            if (payload_len > 0 && packet.payload != nullptr)
            {
                stored.payload = std::make_unique<uint8_t[]>(payload_len);
                std::memcpy(stored.payload.get(), packet.payload, payload_len);
                stored.payload_size = payload_len;
            }
            else
            {
                stored.payload_size = 0;
            }
            stored.occupied = true;
            impl_->ring_slots[slot_index] = std::move(stored);
            impl_->bit_set(slot_index);
            impl_->stats.packets_out_of_order++;
            impl_->stats.current_buffer_size++;
            impl_->window.buffered_packets = static_cast<uint32_t>(impl_->stats.current_buffer_size);
            impl_->stats.current_buffer_size = impl_->window.buffered_packets;
        }

        void Reorderer::insert_owned(protocol::CommonHeader header,
                                     std::unique_ptr<uint8_t[]> payload,
                                     size_t payload_size)
        {
            const uint32_t seq = header.sequence_number;
            if (!impl_->initialized)
            {
                impl_->initialized = true;
                impl_->window.next_expected_seq = seq;
            }

            const uint32_t expected = impl_->window.next_expected_seq;
            if (seq == expected)
            {
                OrderedPacket out;
                out.packet.header = header;
                out.payload_size = payload_size;
                out.owned_payload = std::move(payload);
                out.packet.payload = out.payload_size == 0 ? nullptr : out.owned_payload.get();
                out.packet.total_size = protocol::COMMON_HEADER_SIZE + out.payload_size;
                out.is_zero_filled = false;
                out.is_incomplete_frame = (header.ext_flags & 0x01u) != 0;
                out.sequence_number = seq;
                if (output_callback_)
                {
                    output_callback_(std::move(out));
                }

                impl_->stats.packets_in_order++;
                impl_->stats.sequences_advanced++;
                impl_->window.next_expected_seq = impl_->window.next_expected_seq + 1;
                impl_->base_index = (impl_->base_index + 1) % impl_->ring_slots.size();
                impl_->last_advance_time = std::chrono::steady_clock::now();
                impl_->window.last_advance_ns = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        impl_->last_advance_time.time_since_epoch())
                        .count());
                advance_window();
                impl_->window.buffered_packets = static_cast<uint32_t>(impl_->stats.current_buffer_size);
                impl_->stats.current_buffer_size = impl_->window.buffered_packets;
                return;
            }

            if (!protocol::is_sequence_newer(seq, expected))
            {
                impl_->stats.packets_duplicate++;
                return;
            }

            const uint32_t delta = seq - expected;
            if (delta >= static_cast<uint32_t>(impl_->ring_slots.size()))
            {
                impl_->stats.packets_duplicate++;
                return;
            }

            const size_t slot_index = (impl_->base_index + static_cast<size_t>(delta)) % impl_->ring_slots.size();
            if (impl_->bit_test(slot_index))
            {
                const auto &existing = impl_->ring_slots[slot_index];
                if (existing.occupied && existing.sequence == seq)
                {
                    impl_->stats.packets_duplicate++;
                }
                else
                {
                    impl_->stats.packets_duplicate++;
                }
                return;
            }

            Impl::StoredPacket stored;
            stored.sequence = seq;
            stored.header = header;
            stored.payload = std::move(payload);
            stored.payload_size = payload_size;
            stored.occupied = true;
            impl_->ring_slots[slot_index] = std::move(stored);
            impl_->bit_set(slot_index);
            impl_->stats.packets_out_of_order++;
            impl_->stats.current_buffer_size++;
            impl_->window.buffered_packets = static_cast<uint32_t>(impl_->stats.current_buffer_size);
            impl_->stats.current_buffer_size = impl_->window.buffered_packets;
        }

        void Reorderer::check_timeout()
        {
            if (!impl_->initialized)
            {
                return;
            }

            const auto now = std::chrono::steady_clock::now();
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - impl_->last_advance_time);
            const uint32_t timeout_ms = timeout_ms_runtime_.load(std::memory_order_acquire); // consume latest timeout published by reloader
            if (elapsed.count() < static_cast<int64_t>(timeout_ms))
            {
                return;
            }

            if (config_.enable_zero_fill)
            {
                OrderedPacket gap = create_zero_filled_packet(impl_->window.next_expected_seq);
                if (output_callback_)
                {
                    output_callback_(std::move(gap));
                }
                impl_->stats.packets_zero_filled++;
            }

            impl_->stats.sequences_advanced++;
            impl_->window.next_expected_seq = impl_->window.next_expected_seq + 1;
            impl_->base_index = (impl_->base_index + 1) % impl_->ring_slots.size();
            impl_->last_advance_time = now;
            impl_->window.last_advance_ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count());
            advance_window();
            impl_->window.buffered_packets = static_cast<uint32_t>(impl_->stats.current_buffer_size);
            impl_->stats.current_buffer_size = impl_->window.buffered_packets;
        }

        void Reorderer::advance_window()
        {
            while (true)
            {
                const size_t head = impl_->base_index;
                if (!impl_->bit_test(head))
                {
                    break;
                }

                auto &slot = impl_->ring_slots[head];
                if (!slot.occupied || slot.sequence != impl_->window.next_expected_seq)
                {
                    break;
                }

                OrderedPacket out;
                out.packet.header = slot.header;
                out.owned_payload = std::move(slot.payload);
                out.payload_size = slot.payload_size;
                out.packet.payload = out.payload_size == 0 ? nullptr : out.owned_payload.get();
                out.packet.total_size = protocol::COMMON_HEADER_SIZE + out.payload_size;
                out.is_zero_filled = false;
                out.is_incomplete_frame = (slot.header.ext_flags & 0x01u) != 0;
                out.sequence_number = impl_->window.next_expected_seq;

                if (output_callback_)
                {
                    output_callback_(std::move(out));
                }

                slot.sequence = 0;
                slot.payload_size = 0;
                slot.occupied = false;
                impl_->bit_clear(head);
                if (impl_->stats.current_buffer_size > 0)
                {
                    impl_->stats.current_buffer_size--;
                }
                impl_->stats.packets_in_order++;
                impl_->stats.sequences_advanced++;
                impl_->window.next_expected_seq = impl_->window.next_expected_seq + 1;
                impl_->base_index = (impl_->base_index + 1) % impl_->ring_slots.size();
                impl_->last_advance_time = std::chrono::steady_clock::now();
                impl_->window.last_advance_ns = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        impl_->last_advance_time.time_since_epoch())
                        .count());
            }
        }

        OrderedPacket Reorderer::create_zero_filled_packet(uint32_t seq_num)
        {
            OrderedPacket pkt;
            pkt.packet.header.magic = protocol::PROTOCOL_MAGIC;
            pkt.packet.header.sequence_number = seq_num;
            pkt.packet.header.packet_type = static_cast<uint8_t>(protocol::PacketType::DATA);
            pkt.packet.header.protocol_version = protocol::PROTOCOL_VERSION;
            pkt.packet.header.payload_len = 0;
            pkt.packet.payload = nullptr;
            pkt.packet.total_size = protocol::COMMON_HEADER_SIZE;
            pkt.payload_size = 0;
            pkt.is_zero_filled = true;
            pkt.is_incomplete_frame = true;
            pkt.sequence_number = seq_num;
            return pkt;
        }

        Reorderer::Statistics Reorderer::get_statistics() const
        {
            Statistics snapshot;
            snapshot.packets_in_order = impl_->stats.packets_in_order;
            snapshot.packets_out_of_order = impl_->stats.packets_out_of_order;
            snapshot.packets_duplicate = impl_->stats.packets_duplicate;
            snapshot.packets_zero_filled = impl_->stats.packets_zero_filled;
            snapshot.sequences_advanced = impl_->stats.sequences_advanced;
            snapshot.current_buffer_size = impl_->stats.current_buffer_size;
            return snapshot;
        }

        size_t Reorderer::flush()
        {
            std::vector<std::pair<uint32_t, size_t>> pending;
            pending.reserve(impl_->stats.current_buffer_size);
            for (size_t i = 0; i < impl_->ring_slots.size(); ++i)
            {
                if (!impl_->bit_test(i))
                {
                    continue;
                }
                const auto &slot = impl_->ring_slots[i];
                if (!slot.occupied)
                {
                    continue;
                }
                pending.emplace_back(slot.sequence, i);
            }

            std::sort(pending.begin(), pending.end(), [](const auto &lhs, const auto &rhs)
                      { return lhs.first < rhs.first; });

            for (const auto &entry : pending)
            {
                auto &slot = impl_->ring_slots[entry.second];
                OrderedPacket out;
                out.packet.header = slot.header;
                out.owned_payload = std::move(slot.payload);
                out.payload_size = slot.payload_size;
                out.packet.payload = out.payload_size == 0 ? nullptr : out.owned_payload.get();
                out.packet.total_size = protocol::COMMON_HEADER_SIZE + out.payload_size;
                out.is_zero_filled = false;
                out.is_incomplete_frame = (slot.header.ext_flags & 0x01u) != 0;
                out.sequence_number = slot.sequence;
                if (output_callback_)
                {
                    output_callback_(std::move(out));
                }

                slot.sequence = 0;
                slot.payload_size = 0;
                slot.occupied = false;
                impl_->bit_clear(entry.second);
                impl_->stats.packets_in_order++;
            }

            impl_->stats.current_buffer_size = 0;
            impl_->window.buffered_packets = 0;
            return pending.size();
        }

        void Reorderer::set_timeout_ms(uint32_t timeout_ms)
        {
            timeout_ms_runtime_.store(timeout_ms, std::memory_order_release); // publish reloaded timeout to timeout checker
        }

    } // namespace pipeline
} // namespace receiver
