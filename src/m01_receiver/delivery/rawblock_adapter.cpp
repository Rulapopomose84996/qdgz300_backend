#include "qdgz300/m01_receiver/delivery/rawblock_adapter.h"

#include "qdgz300/m01_receiver/monitoring/logger.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace qdgz300::m01::delivery
{
    namespace
    {
        constexpr uint64_t kNanosecondsPerMillisecond = 1000000ULL;
    }

    RawBlockAdapter::RawBlockAdapter(std::shared_ptr<RawBlockQueue> queue, uint8_t array_id)
        : queue_(std::move(queue)), array_id_(array_id)
    {
    }

    bool RawBlockAdapter::adapt_and_push(receiver::pipeline::OrderedPacket &&packet)
    {
        if (!queue_)
        {
            LOG_WARN("RawBlockAdapter drop: queue is null");
            return false;
        }

        auto block = std::make_shared<receiver::delivery::RawBlock>();
        block->ingest_ts = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
        block->data_ts = packet.packet.header.timestamp * kNanosecondsPerMillisecond;
        block->array_id = array_id_;
        block->cpi_seq = packet.sequence_number;
        block->fragment_count = 1;
        block->data_size = static_cast<uint32_t>(packet.payload_size);
        block->flags = packet.is_zero_filled
                           ? static_cast<uint32_t>(receiver::delivery::RawBlockFlags::INCOMPLETE_FRAME)
                           : 0u;

        const size_t copy_size =
            std::min(packet.payload_size, static_cast<size_t>(receiver::delivery::RAW_BLOCK_PAYLOAD_SIZE));
        if (packet.owned_payload && copy_size > 0)
        {
            std::memcpy(block->payload, packet.owned_payload.get(), copy_size);
        }

        if (packet.payload_size > receiver::delivery::RAW_BLOCK_PAYLOAD_SIZE)
        {
            block->flags |= static_cast<uint32_t>(receiver::delivery::RawBlockFlags::INCOMPLETE_FRAME);
            block->data_size = static_cast<uint32_t>(receiver::delivery::RAW_BLOCK_PAYLOAD_SIZE);

            LOG_WARN("RawBlockAdapter truncated payload: seq=%u payload_size=%zu max_size=%zu",
                     static_cast<unsigned>(packet.sequence_number),
                     packet.payload_size,
                     static_cast<size_t>(receiver::delivery::RAW_BLOCK_PAYLOAD_SIZE));
        }

        return queue_->drop_oldest_push(std::move(block));
    }

    uint64_t RawBlockAdapter::dropped_count() const noexcept
    {
        return queue_ ? queue_->drop_count() : 0;
    }
} // namespace qdgz300::m01::delivery
