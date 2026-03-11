// include/qdgz300/common/numa_utils.h
// NUMA 绑定辅助工具
#pragma once
#include <cstdint>
#include <cstddef>

namespace qdgz300
{

    /// 将当前线程绑定到指定 CPU 核心
    /// @return 0=成功, -1=失败
    int bind_thread_to_cpu(int cpu_core) noexcept;

    /// 验证当前线程是否绑定在指定 CPU 核心上
    /// @return true=当前线程正运行在指定核心, false=未绑定或不匹配
    bool verify_cpu_affinity(int expected_cpu) noexcept;

    /// 在指定 NUMA 节点上分配内存
    /// @return 分配的内存指针，失败返回 nullptr
    void *numa_alloc(size_t size, int numa_node) noexcept;

    /// 释放 NUMA 分配的内存
    void numa_free(void *ptr, size_t size) noexcept;

    /// 获取当前线程绑定的 NUMA 节点
    int get_current_numa_node() noexcept;

    /// 获取指定 CPU 核心所属的 NUMA 节点
    /// @return NUMA 节点编号，失败返回 -1
    int get_numa_node(int cpu_core) noexcept;

    /// 设置线程调度策略 (SCHED_FIFO)
    /// @param priority 优先级 (60-80 for data plane)
    /// @return 0=成功, -1=失败
    int set_realtime_priority(int priority) noexcept;

} // namespace qdgz300
