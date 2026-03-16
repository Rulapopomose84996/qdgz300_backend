/**
 * @file udp_receiver.h
 * @brief 高性能 UDP 接收器——支持单阵面/三阵面拓扑的报文接收门面
 *
 * UdpReceiver 是网络层的顶层门面类（Facade），面向上层（app_init）提供统一的
 * start/stop/set_capture_hook 接口，其内部通过 Pimpl 模式管理一组
 * ArrayFaceReceiver 实例（每个实例对应一个阵面的独立收包线程）。
 *
 * 架构关系：
 * @code
 *  app_init  ──▶  UdpReceiver (本类，门面)
 *                     │
 *                     ├── ArrayFaceReceiver #1  (阵面1, bind 10.0.0.1:9999, CPU 16)
 *                     ├── ArrayFaceReceiver #2  (阵面2, bind 10.0.0.2:9999, CPU 17)
 *                     └── ArrayFaceReceiver #3  (阵面3, bind 10.0.0.3:9999, CPU 18)
 * @endcode
 *
 * 设计要点：
 * - 单阵面模式：array_faces 配为 1 个元素，退化为传统单 socket 接收
 * - 三阵面模式：3 个 ArrayFaceReceiver 各绑定不同 IP（对应独立网卡），相同端口
 * - 所有阵面共享同一统计计数器（UdpReceiverStatistics），使用 atomic 递增
 * - capture_hook 通过 shared_ptr + atomic load/store 实现运行时无锁替换
 *
 * @see ArrayFaceReceiver  实际的 socket + recvmmsg 收包实现
 * @see app_init.cpp       构造 UdpReceiver 并注册报文回调
 */

#ifndef RECEIVER_NETWORK_UDP_RECEIVER_H
#define RECEIVER_NETWORK_UDP_RECEIVER_H

#include "qdgz300/m01_receiver/network/packet_pool.h"     // PacketBuffer（零拷贝报文缓冲区）
#include "qdgz300/m01_receiver/protocol/protocol_types.h" // PacketPriority / CommonHeader

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace receiver
{
    namespace network
    {

        /**
         * @struct ArrayFaceBinding
         * @brief 单个阵面的网络绑定参数
         *
         * 每个阵面对应 FPGA 的一个物理发射面，通过独立的网卡/IP 地址接收数据。
         * 三面阵模式下有 3 个 ArrayFaceBinding，单面阵模式下有 1 个。
         */
        struct ArrayFaceBinding
        {
            uint8_t array_id = 1;              ///< 阵面编号（1~3）
            uint8_t expected_source_id = 0x11; ///< 期望的 FPGA 源设备 ID（用于源过滤）
            std::string bind_ip = "0.0.0.0";   ///< 绑定 IP 地址（对应网卡网口，如 10.0.0.1）
            uint16_t listen_port = 9999;       ///< 监听端口（三阵面使用相同端口）
            int cpu_affinity = 16;             ///< 接收线程绑定的 CPU 核心号
            bool source_filter_enabled = true; ///< 是否按 expected_source_id 过滤非预期报文
        };

        /**
         * @struct UdpReceiverConfig
         * @brief UDP 接收器完整配置
         *
         * 包含阵面绑定列表和全局性能参数。当 array_faces 非空时使用多阵面模式，
         * 为空时使用 bind_ip + listen_port 作为单阵面的回退配置。
         */
        struct UdpReceiverConfig
        {
            /// 阵面绑定列表。非空时为多阵面模式；为空时回退到单阵面遗留模式。
            std::vector<ArrayFaceBinding> array_faces;

            // ── 遗留单阵面参数（仅当 array_faces 为空时使用） ────────
            uint16_t listen_port = 9999;     ///< 监听端口
            std::string bind_ip = "0.0.0.0"; ///< 绑定 IP

            // ── 性能参数（各阵面共享） ───────────────────────────────
            size_t recv_batch_size = 64;        ///< recvmmsg 单次批量接收报文数
            size_t recv_drain_rounds = 4;       ///< 单次唤醒后连续 drain socket queue 的最大轮数
            size_t socket_rcvbuf_mb = 256;      ///< SO_RCVBUF 大小（MB）
            size_t packet_pool_mb_per_face = 64; ///< 每阵面 PacketPool 预算（MB）
            bool enable_so_reuseport = true;    ///< 是否启用 SO_REUSEPORT
            bool enable_ip_freebind = false;    ///< 是否允许 bind 到当前未配置在本机上的 IPv4 地址
            int worker_threads = 8;             ///< 工作线程数（当前未使用，每阵面 1 线程）
            int numa_node = 1;                  ///< 内存分配绑定 NUMA 节点
            int cpu_affinity_start = 16;        ///< CPU 亲和性起始核（遗留参数）
            int cpu_affinity_end = 31;          ///< CPU 亲和性结束核（遗留参数）
            bool prefetch_hints_enabled = true; ///< 是否启用缓存预取指令

            /// 抓包钩子（可选）：每个原始报文接收后回调，用于 pcap 落盘
            std::function<void(const uint8_t *, size_t, uint64_t)> capture_hook;
        };

        /**
         * @struct ReceivedPacket
         * @brief 单个接收到的 UDP 报文及其元数据
         *
         * 由 ArrayFaceReceiver 的回调填充，通过 UdpReceiver 的 PacketCallback
         * 传递给 app_init 中注册的报文处理管线。
         */
        struct ReceivedPacket
        {
            PacketBuffer data;                                                   ///< 零拷贝报文数据缓冲区（从 PacketPool 分配）
            size_t length;                                                       ///< 有效数据长度（字节）
            uint64_t receive_timestamp_ns;                                       ///< 接收时间戳（纳秒，steady_clock）
            uint8_t array_id{0};                                                 ///< 来源阵面编号（1~3）
            protocol::PacketPriority priority{protocol::PacketPriority::NORMAL}; ///< 报文优先级（HIGH=心跳，NORMAL=数据）
        };

        /**
         * @struct UdpReceiverStatistics
         * @brief UDP 接收器全局统计计数器
         *
         * 所有阵面的 ArrayFaceReceiver 共享同一组 atomic 计数器，
         * 使用 relaxed 内存序递增（仅需数值正确性，不需要同步语义）。
         */
        struct UdpReceiverStatistics
        {
            std::atomic<uint64_t> packets_received{0};     ///< 成功接收的报文总数
            std::atomic<uint64_t> packets_filtered{0};     ///< 源 ID 过滤丢弃的报文数
            std::atomic<uint64_t> bytes_received{0};       ///< 接收的总字节数
            std::atomic<uint64_t> recv_errors{0};          ///< 接收系统调用错误次数
            std::atomic<uint64_t> affinity_failures{0};    ///< CPU 亲和性设置失败次数
            std::atomic<uint64_t> socket_bind_failures{0}; ///< Socket 绑定失败次数
        };

        struct ArrayFaceRuntimeStats
        {
            uint8_t array_id{0};
            bool affinity_verified{false};
            PacketPool::Statistics packet_pool{};
        };

        /**
         * @class UdpReceiver
         * @brief UDP 高性能接收器门面
         *
         * 职责：
         * 1. 根据 UdpReceiverConfig 中的 array_faces 列表创建 N 个 ArrayFaceReceiver
         * 2. 统一管理 start/stop 生命周期
         * 3. 将各阵面收到的报文统一通过 PacketCallback 回调上层
         * 4. 提供 set_capture_hook() 用于运行时动态挂载/卸载 pcap 抓包
         *
         * 线程模型：
         * - start() 后每个阵面各有一个独立的收包线程（绑定到指定 CPU 核心）
         * - 回调在各自阵面的收包线程上下文中执行
         * - 统计计数器通过 atomic 实现无锁并发递增
         */
        class UdpReceiver
        {
        public:
            /// 报文回调类型：接收到完整报文后调用，参数为右值引用（支持零拷贝移动）
            using PacketCallback = std::function<void(ReceivedPacket &&)>;
            /// 抓包钩子类型：(原始数据指针, 长度, 时间戳_微秒)
            using CaptureHook = std::function<void(const uint8_t *, size_t, uint64_t)>;

            /**
             * @brief 构造 UDP 接收器
             *
             * @param config   接收器配置（阵面绑定 + 性能参数）
             * @param callback 报文回调（每个收到的有效报文都会调用此回调）
             *
             * @note 构造时仅保存配置和回调，不创建 socket 也不启动线程。
             */
            explicit UdpReceiver(const UdpReceiverConfig &config, PacketCallback callback);

            /**
             * @brief 析构：自动调用 stop() 停止所有收包线程并关闭 socket
             */
            ~UdpReceiver();

            UdpReceiver(const UdpReceiver &) = delete;            ///< 禁止拷贝构造
            UdpReceiver &operator=(const UdpReceiver &) = delete; ///< 禁止拷贝赋值

            /**
             * @brief 启动所有阵面的接收器
             *
             * 执行步骤：
             * 1. resolve_bindings() 解析阵面绑定列表
             * 2. 为每个绑定创建 ArrayFaceReceiver 并调用其 start()
             * 3. 任一阵面启动失败则回滚已启动的阵面并返回 false
             *
             * @return 是否全部阵面启动成功
             * @retval true  所有阵面均已启动
             * @retval false 至少一个阵面启动失败（已清理）
             */
            bool start();

            /**
             * @brief 停止所有阵面的接收器
             *
             * 设置 running_ = false，逐一 stop 各 ArrayFaceReceiver
             * （join 收包线程 + close socket）。
             */
            void stop();

            /**
             * @brief 动态设置/清除抓包钩子
             *
             * 通过 shared_ptr + atomic store 实现运行时无锁替换。
             * 传入空 hook 表示卸载当前钩子。
             *
             * @param hook 新的抓包回调（空则卸载）
             */
            void set_capture_hook(CaptureHook hook);

            /**
             * @brief 获取全局接收统计信息
             * @return 当前统计信息的只读引用（各计数器为 atomic，可并发读取）
             */
            const UdpReceiverStatistics &get_statistics() const { return stats_; }
            std::vector<ArrayFaceRuntimeStats> get_face_runtime_stats() const;

        private:
            /**
             * @brief 解析阵面绑定列表
             *
             * 若 config_.array_faces 非空则直接使用；
             * 否则根据遗留单阵面参数生成一个默认的 ArrayFaceBinding。
             *
             * @return 最终使用的阵面绑定列表
             */
            std::vector<ArrayFaceBinding> resolve_bindings() const;

            UdpReceiverConfig config_;                    ///< 接收器配置（构造时拷贝）
            PacketCallback callback_;                     ///< 报文回调（转交给各 ArrayFaceReceiver）
            UdpReceiverStatistics stats_;                 ///< 全局统计计数器（各阵面共享）
            std::atomic<bool> running_{false};            ///< 运行状态标志
            std::shared_ptr<CaptureHook> capture_hook_{}; ///< 抓包钩子（atomic load/store 热替换）

            class Impl;                  ///< Pimpl 前向声明
            std::unique_ptr<Impl> impl_; ///< Pimpl 实例（持有 ArrayFaceReceiver 列表）
        };

    } // namespace network
} // namespace receiver

#endif // RECEIVER_NETWORK_UDP_RECEIVER_H
