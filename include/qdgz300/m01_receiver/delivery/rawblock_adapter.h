#pragma once

#include "qdgz300/common/constants.h"
#include "qdgz300/common/spsc_queue.h"
#include "qdgz300/m01_receiver/delivery/raw_block.h"
#include "qdgz300/m01_receiver/pipeline/reorderer.h"

#include <cstdint>
#include <memory>

namespace qdgz300::m01::delivery
{
    class RawBlockAdapter
    {
    public:
        using RawBlockQueue =
            qdgz300::SPSCQueue<std::shared_ptr<receiver::delivery::RawBlock>, RAWCPI_Q_CAPACITY>;

        explicit RawBlockAdapter(std::shared_ptr<RawBlockQueue> queue, uint8_t array_id);

        bool adapt_and_push(receiver::pipeline::OrderedPacket &&packet);

        uint64_t dropped_count() const noexcept;

    private:
        std::shared_ptr<RawBlockQueue> queue_;
        uint8_t array_id_;
    };
} // namespace qdgz300::m01::delivery

namespace receiver::delivery
{
    using RawBlockAdapter = ::qdgz300::m01::delivery::RawBlockAdapter;
}
