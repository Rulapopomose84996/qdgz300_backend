// src/common/numa_utils.cpp
// NUMA 绑定辅助工具实现
#include "qdgz300/common/numa_utils.h"
#include <cstdlib>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#ifdef QDGZ300_HAS_LIBNUMA
#include <numa.h>
#endif
#endif

namespace qdgz300
{

        int bind_thread_to_cpu(int cpu_core) noexcept
        {
#ifdef __linux__
                cpu_set_t cpuset;
                CPU_ZERO(&cpuset);
                CPU_SET(cpu_core, &cpuset);
                return pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
#else
                (void)cpu_core;
                return -1;
#endif
        }

        bool verify_cpu_affinity(int expected_cpu) noexcept
        {
#ifdef __linux__
                cpu_set_t cpuset;
                CPU_ZERO(&cpuset);
                if (pthread_getaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) != 0)
                {
                        return false;
                }
                // 检查是否仅绑定在指定核心上
                if (!CPU_ISSET(expected_cpu, &cpuset))
                {
                        return false;
                }
                // 可选：验证当前实际运行的 CPU 是否匹配
                int current_cpu = sched_getcpu();
                return current_cpu == expected_cpu;
#else
                (void)expected_cpu;
                return false;
#endif
        }

        void *numa_alloc(size_t size, int numa_node) noexcept
        {
#if defined(__linux__) && defined(QDGZ300_HAS_LIBNUMA)
                return numa_alloc_onnode(size, numa_node);
#else
                (void)numa_node;
                return std::malloc(size);
#endif
        }

        void numa_free(void *ptr, size_t size) noexcept
        {
#if defined(__linux__) && defined(QDGZ300_HAS_LIBNUMA)
                ::numa_free(ptr, size);
#else
                (void)size;
                std::free(ptr);
#endif
        }

        int get_current_numa_node() noexcept
        {
#if defined(__linux__) && defined(QDGZ300_HAS_LIBNUMA)
                return numa_node_of_cpu(sched_getcpu());
#else
                return 0;
#endif
        }

        int get_numa_node(int cpu_core) noexcept
        {
#if defined(__linux__) && defined(QDGZ300_HAS_LIBNUMA)
                return numa_node_of_cpu(cpu_core);
#else
                (void)cpu_core;
                return 0;
#endif
        }

        int set_realtime_priority(int priority) noexcept
        {
#ifdef __linux__
                struct sched_param param;
                param.sched_priority = priority;
                return pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
#else
                (void)priority;
                return -1;
#endif
        }

} // namespace qdgz300
