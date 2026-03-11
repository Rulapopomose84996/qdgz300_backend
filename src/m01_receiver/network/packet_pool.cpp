#include "qdgz300/m01_receiver/network/packet_pool.h"

#include "qdgz300/m01_receiver/monitoring/metrics.h"
#include "qdgz300/m01_receiver/network/common/numa_allocator.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <vector>

namespace receiver
{
    namespace network
    {
        namespace
        {
            struct FallbackHeader
            {
                uint32_t magic;
            };

            constexpr uint32_t kFallbackMagic = 0xFACC0DE1u;
        } // namespace

        class PacketPool::Impl
        {
        public:
            struct FreeNode
            {
                uint8_t *buffer{nullptr};
                FreeNode *next{nullptr};
            };

            uint8_t *pool_storage{nullptr};
            size_t pool_bytes{0};
            int numa_node{1};

            std::vector<FreeNode> nodes;
            std::atomic<FreeNode *> free_head{nullptr};

            std::uintptr_t pool_begin{0};
            std::uintptr_t pool_end{0};

            std::atomic<size_t> total_size{0};
            std::atomic<size_t> available{0};
            std::atomic<size_t> allocated{0};
            std::atomic<size_t> alloc_count{0};
            std::atomic<size_t> dealloc_count{0};
            std::atomic<size_t> fallback_alloc{0};
        };

        PacketPool::PacketPool(size_t packet_size, size_t pool_size, int numa_node)
            : packet_size_(packet_size), pool_size_(pool_size), impl_(std::make_unique<Impl>())
        {
            impl_->numa_node = numa_node;
            if (packet_size_ == 0 || pool_size_ == 0)
            {
                monitoring::MetricsCollector::instance().set_numa_local_memory_pct(0.0);
                return;
            }

            const size_t bytes_total = packet_size_ * pool_size_;
            const auto alloc_begin = std::chrono::steady_clock::now();
            common::NumaAllocator<uint8_t> allocator(numa_node);
            try
            {
                impl_->pool_storage = allocator.allocate(bytes_total);
                impl_->pool_bytes = bytes_total;
            }
            catch (const std::bad_alloc &)
            {
                impl_->pool_storage = nullptr;
                impl_->pool_bytes = 0;
            }
            const auto alloc_end = std::chrono::steady_clock::now();
            const auto alloc_ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(alloc_end - alloc_begin).count());
            monitoring::MetricsCollector::instance().observe_packet_pool_allocation_latency_ns(alloc_ns);

            if (impl_->pool_storage == nullptr)
            {
                monitoring::MetricsCollector::instance().set_numa_local_memory_pct(0.0);
                return;
            }

            impl_->nodes.resize(pool_size_);
            uint8_t *base = impl_->pool_storage;
            impl_->pool_begin = reinterpret_cast<std::uintptr_t>(base);
            impl_->pool_end = impl_->pool_begin + bytes_total;

            for (size_t i = 0; i < pool_size_; ++i)
            {
                auto &node = impl_->nodes[i];
                node.buffer = base + (i * packet_size_);
                node.next = (i + 1 < pool_size_) ? &impl_->nodes[i + 1] : nullptr;
            }
            impl_->free_head.store(impl_->nodes.empty() ? nullptr : &impl_->nodes[0], std::memory_order_release);

            impl_->total_size.store(pool_size_, std::memory_order_relaxed);
            impl_->available.store(pool_size_, std::memory_order_relaxed);
            impl_->allocated.store(0, std::memory_order_relaxed);
            impl_->alloc_count.store(0, std::memory_order_relaxed);
            impl_->dealloc_count.store(0, std::memory_order_relaxed);
            impl_->fallback_alloc.store(0, std::memory_order_relaxed);
            monitoring::MetricsCollector::instance().set_numa_local_memory_pct(100.0);
        }

        PacketPool::~PacketPool()
        {
            if (impl_ && impl_->pool_storage != nullptr && impl_->pool_bytes != 0)
            {
                common::NumaAllocator<uint8_t> allocator(impl_->numa_node);
                allocator.deallocate(impl_->pool_storage, impl_->pool_bytes);
                impl_->pool_storage = nullptr;
                impl_->pool_bytes = 0;
            }
        }

        uint8_t *PacketPool::allocate()
        {
            impl_->alloc_count.fetch_add(1, std::memory_order_relaxed);

            Impl::FreeNode *head = impl_->free_head.load(std::memory_order_acquire);
            while (head != nullptr)
            {
                Impl::FreeNode *next = head->next;
                if (impl_->free_head.compare_exchange_weak(head, next, std::memory_order_acq_rel, std::memory_order_acquire))
                {
                    impl_->available.fetch_sub(1, std::memory_order_relaxed);
                    impl_->allocated.fetch_add(1, std::memory_order_relaxed);
                    return head->buffer;
                }
            }

            impl_->fallback_alloc.fetch_add(1, std::memory_order_relaxed);
            auto *raw = new (std::nothrow) uint8_t[sizeof(FallbackHeader) + packet_size_];
            if (raw == nullptr)
            {
                return nullptr;
            }

            auto *hdr = reinterpret_cast<FallbackHeader *>(raw);
            hdr->magic = kFallbackMagic;
            return raw + sizeof(FallbackHeader);
        }

        void PacketPool::deallocate(uint8_t *buffer)
        {
            if (buffer == nullptr)
            {
                return;
            }

            impl_->dealloc_count.fetch_add(1, std::memory_order_relaxed);

            const std::uintptr_t addr = reinterpret_cast<std::uintptr_t>(buffer);
            if (addr >= impl_->pool_begin && addr < impl_->pool_end)
            {
                const size_t offset = static_cast<size_t>(addr - impl_->pool_begin);
                if (packet_size_ == 0 || (offset % packet_size_) != 0)
                {
                    return;
                }

                const size_t index = offset / packet_size_;
                if (index >= impl_->nodes.size())
                {
                    return;
                }

                Impl::FreeNode *node = &impl_->nodes[index];
                Impl::FreeNode *head = impl_->free_head.load(std::memory_order_acquire);
                do
                {
                    node->next = head;
                } while (!impl_->free_head.compare_exchange_weak(head, node, std::memory_order_acq_rel, std::memory_order_acquire));

                impl_->available.fetch_add(1, std::memory_order_relaxed);
                impl_->allocated.fetch_sub(1, std::memory_order_relaxed);
                return;
            }

            uint8_t *raw = buffer - sizeof(FallbackHeader);
            auto *hdr = reinterpret_cast<FallbackHeader *>(raw);
            if (hdr->magic == kFallbackMagic)
            {
                hdr->magic = 0;
                delete[] raw;
                return;
            }

            delete[] buffer;
        }

        PacketPool::Statistics PacketPool::get_statistics() const
        {
            Statistics copy{};
            copy.total_size = impl_->total_size.load(std::memory_order_relaxed);
            copy.available = impl_->available.load(std::memory_order_relaxed);
            copy.allocated = impl_->allocated.load(std::memory_order_relaxed);
            copy.alloc_count = impl_->alloc_count.load(std::memory_order_relaxed);
            copy.dealloc_count = impl_->dealloc_count.load(std::memory_order_relaxed);
            copy.fallback_alloc = impl_->fallback_alloc.load(std::memory_order_relaxed);
            return copy;
        }

    } // namespace network
} // namespace receiver
