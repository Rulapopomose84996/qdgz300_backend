#include "qdgz300/m01_receiver/delivery/delivery_interface.h"
#include "qdgz300/m01_receiver/monitoring/logger.h"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

#if defined(__linux__) && defined(RECEIVER_HAS_LIBNUMA)
#include <numa.h>
#endif

namespace receiver
{
    namespace delivery
    {

        CallbackDelivery::CallbackDelivery(Callback callback)
            : callback_(std::move(callback))
        {
        }

        bool CallbackDelivery::deliver(const pipeline::OrderedPacket &packet)
        {
            if (!callback_)
            {
                stats_.delivery_errors++;
                return false;
            }

            callback_(packet);
            stats_.delivered_packets++;
            stats_.bytes_delivered += packet.packet.header.payload_len;
            return true;
        }

        void CallbackDelivery::flush()
        {
        }

        DeliveryInterface::Statistics CallbackDelivery::get_statistics() const
        {
            return stats_;
        }

        class SharedMemoryDelivery::Impl
        {
        public:
            std::string shm_name;
            size_t shm_size{0};
            Statistics stats;
            bool ready{false};

            static constexpr uint32_t kMagic = 0x53484D31u;
            static constexpr uint32_t kVersion = 1u;
            static constexpr uint32_t kRecordMarker = 0x52454344u;
            static constexpr uint32_t kWrapMarker = 0x57524150u;

            struct alignas(64) SharedMemoryHeader
            {
                uint32_t magic{0};
                uint32_t version{0};
                uint64_t total_size{0};
                uint64_t ring_size{0};
                uint64_t head{0};
                uint64_t tail{0};
                pthread_mutex_t mutex{};
                pthread_cond_t data_ready{};
                uint8_t initialized{0};
                uint8_t reserved[63]{};
            };

            struct RecordHeader
            {
                uint32_t marker{0};
                uint32_t payload_size{0};
                uint32_t sequence_number{0};
                uint8_t is_zero_filled{0};
                uint8_t reserved[3]{};
            };

            int fd{-1};
            void *mapping{nullptr};
            SharedMemoryHeader *header{nullptr};
            uint8_t *ring{nullptr};

            static uint64_t used_space(uint64_t head, uint64_t tail, uint64_t ring_size)
            {
                if (head >= tail)
                {
                    return head - tail;
                }
                return ring_size - (tail - head);
            }

            static uint64_t free_space(uint64_t head, uint64_t tail, uint64_t ring_size)
            {
                const uint64_t used = used_space(head, tail, ring_size);
                return (ring_size - used - 1u);
            }

            void write_ring(uint64_t offset, const uint8_t *src, size_t nbytes)
            {
                if (nbytes == 0)
                {
                    return;
                }
                const uint64_t ring_size = header->ring_size;
                if (offset + nbytes <= ring_size)
                {
                    std::memcpy(ring + offset, src, nbytes);
                    return;
                }

                const size_t first = static_cast<size_t>(ring_size - offset);
                std::memcpy(ring + offset, src, first);
                std::memcpy(ring, src + first, nbytes - first);
            }

            void read_ring(uint64_t offset, uint8_t *dst, size_t nbytes)
            {
                if (nbytes == 0)
                {
                    return;
                }
                const uint64_t ring_size = header->ring_size;
                if (offset + nbytes <= ring_size)
                {
                    std::memcpy(dst, ring + offset, nbytes);
                    return;
                }

                const size_t first = static_cast<size_t>(ring_size - offset);
                std::memcpy(dst, ring + offset, first);
                std::memcpy(dst + first, ring, nbytes - first);
            }

            bool drop_oldest_unlocked()
            {
                if (header->head == header->tail)
                {
                    return false;
                }

                RecordHeader rec{};
                read_ring(header->tail, reinterpret_cast<uint8_t *>(&rec), sizeof(rec));
                if (rec.marker == kWrapMarker)
                {
                    header->tail = 0;
                    return true;
                }
                if (rec.marker != kRecordMarker)
                {
                    header->tail = header->head;
                    return false;
                }

                const uint64_t advance = sizeof(RecordHeader) + rec.payload_size;
                header->tail = (header->tail + advance) % header->ring_size;
                return true;
            }
        };

        SharedMemoryDelivery::SharedMemoryDelivery(const std::string &shm_name, size_t shm_size)
            : impl_(std::make_unique<Impl>())
        {
            impl_->shm_name = shm_name;
            impl_->shm_size = shm_size;

            if (impl_->shm_name.empty() || impl_->shm_name[0] != '/' ||
                impl_->shm_size <= sizeof(Impl::SharedMemoryHeader) + sizeof(Impl::RecordHeader) + 1u)
            {
                return;
            }

            impl_->fd = shm_open(impl_->shm_name.c_str(), O_CREAT | O_RDWR, 0666);
            if (impl_->fd < 0)
            {
                return;
            }

            if (ftruncate(impl_->fd, static_cast<off_t>(impl_->shm_size)) != 0)
            {
                close(impl_->fd);
                impl_->fd = -1;
                return;
            }

            impl_->mapping = mmap(nullptr, impl_->shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, impl_->fd, 0);
            if (impl_->mapping == MAP_FAILED)
            {
                impl_->mapping = nullptr;
                close(impl_->fd);
                impl_->fd = -1;
                return;
            }

            impl_->header = reinterpret_cast<Impl::SharedMemoryHeader *>(impl_->mapping);
            impl_->ring = reinterpret_cast<uint8_t *>(impl_->mapping) + sizeof(Impl::SharedMemoryHeader);

            if (impl_->header->magic != Impl::kMagic ||
                impl_->header->version != Impl::kVersion ||
                impl_->header->initialized == 0)
            {
                std::memset(impl_->mapping, 0, impl_->shm_size);
                impl_->header->magic = Impl::kMagic;
                impl_->header->version = Impl::kVersion;
                impl_->header->total_size = impl_->shm_size;
                impl_->header->ring_size = impl_->shm_size - sizeof(Impl::SharedMemoryHeader);
                impl_->header->head = 0;
                impl_->header->tail = 0;

                pthread_mutexattr_t mutex_attr;
                pthread_mutexattr_init(&mutex_attr);
                pthread_mutexattr_setpshared(&mutex_attr, PTHREAD_PROCESS_SHARED);
                pthread_mutex_init(&impl_->header->mutex, &mutex_attr);
                pthread_mutexattr_destroy(&mutex_attr);

                pthread_condattr_t cond_attr;
                pthread_condattr_init(&cond_attr);
                pthread_condattr_setpshared(&cond_attr, PTHREAD_PROCESS_SHARED);
                pthread_cond_init(&impl_->header->data_ready, &cond_attr);
                pthread_condattr_destroy(&cond_attr);

                impl_->header->initialized = 1;
            }

#if defined(__linux__) && defined(RECEIVER_HAS_LIBNUMA)
            if (numa_available() != -1)
            {
                numa_set_preferred(1);
            }
#endif

            impl_->ready = true;
        }

        SharedMemoryDelivery::~SharedMemoryDelivery()
        {
            if (impl_->mapping != nullptr)
            {
                munmap(impl_->mapping, impl_->shm_size);
                impl_->mapping = nullptr;
            }
            if (impl_->fd >= 0)
            {
                close(impl_->fd);
                impl_->fd = -1;
            }
        }

        bool SharedMemoryDelivery::deliver(const pipeline::OrderedPacket &packet)
        {
            if (!impl_->ready || impl_->header == nullptr)
            {
                impl_->stats.delivery_errors++;
                return false;
            }

            const uint8_t *payload = packet.packet.payload;
            const uint32_t payload_size = static_cast<uint32_t>(packet.packet.header.payload_len);
            const uint64_t required = sizeof(Impl::RecordHeader) + payload_size;
            if (required >= impl_->header->ring_size)
            {
                impl_->stats.delivery_errors++;
                return false;
            }

            pthread_mutex_lock(&impl_->header->mutex);
            while (Impl::free_space(impl_->header->head, impl_->header->tail, impl_->header->ring_size) < required)
            {
                if (!impl_->drop_oldest_unlocked())
                {
                    pthread_mutex_unlock(&impl_->header->mutex);
                    impl_->stats.delivery_errors++;
                    return false;
                }
            }

            if (impl_->header->head + sizeof(Impl::RecordHeader) > impl_->header->ring_size ||
                impl_->header->head + required > impl_->header->ring_size)
            {
                Impl::RecordHeader wrap{};
                wrap.marker = Impl::kWrapMarker;
                impl_->write_ring(impl_->header->head, reinterpret_cast<const uint8_t *>(&wrap), sizeof(wrap));
                impl_->header->head = 0;
            }

            Impl::RecordHeader rec{};
            rec.marker = Impl::kRecordMarker;
            rec.payload_size = payload_size;
            rec.sequence_number = packet.sequence_number;
            rec.is_zero_filled = packet.is_zero_filled ? 1u : 0u;

            impl_->write_ring(impl_->header->head, reinterpret_cast<const uint8_t *>(&rec), sizeof(rec));
            impl_->header->head = (impl_->header->head + sizeof(rec)) % impl_->header->ring_size;
            if (payload_size > 0 && payload != nullptr)
            {
                impl_->write_ring(impl_->header->head, payload, payload_size);
                impl_->header->head = (impl_->header->head + payload_size) % impl_->header->ring_size;
            }

            pthread_cond_signal(&impl_->header->data_ready);
            pthread_mutex_unlock(&impl_->header->mutex);

            impl_->stats.delivered_packets++;
            impl_->stats.bytes_delivered += payload_size;
            return true;
        }

        void SharedMemoryDelivery::flush()
        {
            if (impl_->mapping != nullptr)
            {
                (void)msync(impl_->mapping, impl_->shm_size, MS_ASYNC);
            }
        }

        DeliveryInterface::Statistics SharedMemoryDelivery::get_statistics() const
        {
            return impl_->stats;
        }

        class UnixSocketDelivery::Impl
        {
        public:
            std::string socket_path;
            uint32_t reconnect_interval_ms{100};
            uint32_t current_backoff_ms{100};
            Statistics stats;
            int sock_fd{-1};
            std::chrono::steady_clock::time_point next_reconnect{
                std::chrono::steady_clock::time_point::min()};

            bool connect_with_backoff()
            {
                if (socket_path.empty())
                {
                    return false;
                }

                const auto now = std::chrono::steady_clock::now();
                if (now < next_reconnect)
                {
                    return false;
                }

                if (sock_fd >= 0)
                {
                    close(sock_fd);
                    sock_fd = -1;
                }

                sock_fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
                if (sock_fd < 0)
                {
                    schedule_reconnect();
                    return false;
                }

                sockaddr_un addr{};
                addr.sun_family = AF_UNIX;
                if (socket_path.size() >= sizeof(addr.sun_path))
                {
                    close(sock_fd);
                    sock_fd = -1;
                    schedule_reconnect();
                    return false;
                }
                std::memcpy(addr.sun_path, socket_path.c_str(), socket_path.size() + 1);

                if (connect(sock_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0)
                {
                    close(sock_fd);
                    sock_fd = -1;
                    schedule_reconnect();
                    return false;
                }

                current_backoff_ms = std::max<uint32_t>(reconnect_interval_ms, 1u);
                next_reconnect = std::chrono::steady_clock::time_point::min();
                return true;
            }

            void schedule_reconnect()
            {
                next_reconnect =
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(current_backoff_ms);
                current_backoff_ms = std::min<uint32_t>(current_backoff_ms * 2u, 5000u);
            }
        };

        UnixSocketDelivery::UnixSocketDelivery(const std::string &socket_path, uint32_t reconnect_interval_ms)
            : impl_(std::make_unique<Impl>())
        {
            impl_->socket_path = socket_path;
            impl_->reconnect_interval_ms = std::max<uint32_t>(reconnect_interval_ms, 1u);
            impl_->current_backoff_ms = impl_->reconnect_interval_ms;
            (void)impl_->connect_with_backoff();
        }

        UnixSocketDelivery::~UnixSocketDelivery()
        {
            if (impl_->sock_fd >= 0)
            {
                close(impl_->sock_fd);
                impl_->sock_fd = -1;
            }
        }

        bool UnixSocketDelivery::deliver(const pipeline::OrderedPacket &packet)
        {
            if (impl_->sock_fd < 0 && !impl_->connect_with_backoff())
            {
                impl_->stats.delivery_errors++;
                return false;
            }

            const uint32_t payload_size = static_cast<uint32_t>(packet.packet.header.payload_len);
            const uint32_t flags = 0u;
            std::vector<uint8_t> wire(8u + payload_size, 0u);
            std::memcpy(wire.data(), &payload_size, sizeof(payload_size));
            std::memcpy(wire.data() + 4u, &flags, sizeof(flags));
            if (payload_size > 0 && packet.packet.payload != nullptr)
            {
                std::memcpy(wire.data() + 8u, packet.packet.payload, payload_size);
            }

            const ssize_t sent = send(impl_->sock_fd, wire.data(), wire.size(), 0);
            if (sent != static_cast<ssize_t>(wire.size()))
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    LOG_WARN("Unix socket delivery dropped frame due to EAGAIN");
                    impl_->stats.delivery_errors++;
                    return false;
                }

                if (errno == EPIPE || errno == ECONNRESET)
                {
                    LOG_WARN("Unix socket delivery peer disconnected, scheduling reconnect");
                    close(impl_->sock_fd);
                    impl_->sock_fd = -1;
                    impl_->schedule_reconnect();
                    impl_->stats.delivery_errors++;
                    return false;
                }

                impl_->stats.delivery_errors++;
                return false;
            }

            impl_->stats.delivered_packets++;
            impl_->stats.bytes_delivered += payload_size;
            return true;
        }

        void UnixSocketDelivery::flush()
        {
            if (impl_->sock_fd < 0)
            {
                return;
            }

            const uint32_t frame_size = 0u;
            const uint32_t flags = 0xFFFFFFFFu;
            uint8_t eof_record[8];
            std::memcpy(eof_record, &frame_size, sizeof(frame_size));
            std::memcpy(eof_record + 4u, &flags, sizeof(flags));
            (void)send(impl_->sock_fd, eof_record, sizeof(eof_record), 0);

            close(impl_->sock_fd);
            impl_->sock_fd = -1;
        }

        DeliveryInterface::Statistics UnixSocketDelivery::get_statistics() const
        {
            return impl_->stats;
        }

    } // namespace delivery
} // namespace receiver
