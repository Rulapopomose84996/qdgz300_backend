#ifndef RECEIVER_NETWORK_COMMON_NUMA_ALLOCATOR_H
#define RECEIVER_NETWORK_COMMON_NUMA_ALLOCATOR_H

#include <cstddef>
#include <new>
#include <type_traits>

#if defined(RECEIVER_HAS_LIBNUMA)
#include <numa.h>
#endif

namespace receiver
{
    namespace network
    {
        namespace common
        {
            template <typename T>
            class NumaAllocator
            {
            public:
                using value_type = T;

                NumaAllocator() noexcept : node_(1) {}
                explicit NumaAllocator(int node) noexcept : node_(node) {}

                template <typename U>
                NumaAllocator(const NumaAllocator<U> &other) noexcept : node_(other.node()) {}

                T *allocate(std::size_t n)
                {
                    if (n == 0)
                    {
                        return nullptr;
                    }

                    const std::size_t bytes = n * sizeof(T);

#if defined(RECEIVER_HAS_LIBNUMA)
                    if (node_ >= 0 && numa_available() != -1)
                    {
                        void *ptr = numa_alloc_onnode(bytes, node_);
                        if (ptr == nullptr)
                        {
                            throw std::bad_alloc();
                        }
                        return static_cast<T *>(ptr);
                    }
#endif
                    return static_cast<T *>(::operator new(bytes));
                }

                void deallocate(T *p, std::size_t n) noexcept
                {
                    if (p == nullptr)
                    {
                        return;
                    }

#if defined(RECEIVER_HAS_LIBNUMA)
                    if (node_ >= 0 && numa_available() != -1)
                    {
                        numa_free(p, n * sizeof(T));
                        return;
                    }
#endif
                    ::operator delete(p);
                }

                template <typename U>
                struct rebind
                {
                    using other = NumaAllocator<U>;
                };

                int node() const noexcept { return node_; }

                bool operator==(const NumaAllocator &other) const noexcept
                {
                    return node_ == other.node_;
                }

                bool operator!=(const NumaAllocator &other) const noexcept
                {
                    return !(*this == other);
                }

            private:
                int node_;
            };
        } // namespace common
    } // namespace network
} // namespace receiver

#endif // RECEIVER_NETWORK_COMMON_NUMA_ALLOCATOR_H
