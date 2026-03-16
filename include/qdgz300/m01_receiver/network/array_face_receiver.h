/**
 * @file array_face_receiver.h
 * @brief 单阵面 UDP 接收器——一个独立线程 + 一个 socket 的高性能收包单元
 *
 * ArrayFaceReceiver 是网络层的最底层组件，每个实例对应一个物理阵面：
 * - 拥有独立的 UDP socket（绑定到特定 IP:Port）
 * - 拥有独立的收包线程（绑定到指定 CPU 核心 + NUMA 节点）
 * - 使用 Linux recvmmsg 系统调用批量接收报文以提升吞吐
 * - 内置 PacketPool 实现零拷贝缓冲区管理
 *
 * 线程模型：
 * @code
 *  start() ──▶ worker_ thread
 *                 │
 *                 ├── bind_current_thread_to_cpu()  // 设置 CPU + NUMA 亲和性
 *                 │
 *                 └── receive_loop()               // 主收包循环
 *                       │
 *                       ├── recvmmsg() 批量接收
 *                       ├── 源 ID 过滤
 *                       ├── DSCP → 优先级分类（HIGH / NORMAL）
 *                       ├── capture_hook 抓包旁路
 *                       ├── 心跳包优先投递
 *                       └── packet_handler_ 回调上层
 * @endcode
 *
 * 性能优化要点：
 * - recvmmsg 批量收包（减少系统调用次数）
 * - PacketPool 预分配 + 零拷贝（避免 malloc/free 开销）
 * - CPU 亲和性绑定（减少上下文切换和缓存失效）
 * - NUMA 节点绑定（确保内存分配在本地节点）
 * - __builtin_prefetch 预取提示（加速下一个报文的处理）
 * - 心跳包优先投递（确保心跳检测不被数据流阻塞）
 *
 * @see UdpReceiver   门面类，管理多个 ArrayFaceReceiver 实例
 * @see PacketPool     零拷贝缓冲区内存池
 */

#ifndef RECEIVER_NETWORK_ARRAY_FACE_RECEIVER_H
#define RECEIVER_NETWORK_ARRAY_FACE_RECEIVER_H

#include "qdgz300/m01_receiver/network/packet_pool.h"     // PacketBuffer / PacketPool
#include "qdgz300/m01_receiver/protocol/protocol_types.h" // PacketPriority / CommonHeader / PacketType

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace receiver
{
    namespace network
    {
        /**
         * @struct ArrayFaceReceiverConfig
         * @brief 单阵面接收器配置
         *
         * 包含阵面特定参数（网络绑定、CPU 亲和性）和全局性能参数
         * （批量大小、缓冲区大小、NUMA 节点等）。
         */
        struct ArrayFaceReceiverConfig
        {
            uint8_t array_id = 1;               ///< 阵面编号（1~3）
            uint8_t expected_source_id = 0x11;  ///< 期望的 FPGA 源 ID（用于源过滤）
            std::string bind_ip = "0.0.0.0";    ///< 绑定 IP 地址（对应物理网卡）
            uint16_t listen_port = 9999;        ///< 监听端口号（三阵面共用同一端口）
            int cpu_affinity = 16;              ///< 收包线程绑定的 CPU 核心号
            bool source_filter_enabled = true;  ///< 是否启用源 ID 过滤（丢弃非预期 source_id 的包）
            size_t recv_batch_size = 64;        ///< recvmmsg 单次批量大小（上限 64）
            size_t recv_drain_rounds = 4;       ///< 单次唤醒后最多连续 drain socket queue 的轮数
            size_t socket_rcvbuf_mb = 256;      ///< SO_RCVBUF 内核接收缓冲区大小（MB）
            size_t packet_pool_mb = 64;         ///< 每阵面 PacketPool 预算（MB）
            bool enable_ip_freebind = false;    ///< 是否允许绑定到当前未配置在本机上的 IPv4 地址（Linux IP_FREEBIND）
            int numa_node = 1;                  ///< 内存分配绑定的 NUMA 节点
            bool prefetch_hints_enabled = true; ///< 是否启用 __builtin_prefetch 缓存预取
        };

        /**
         * @struct ArrayFaceStatsSink
         * @brief 统计指针集合——指向 UdpReceiverStatistics 中的 atomic 计数器
         *
         * 所有阵面的 ArrayFaceReceiver 共享同一组计数器，通过指针间接递增，
         * 实现无需额外聚合的全局统计。
         */
        struct ArrayFaceStatsSink
        {
            std::atomic<uint64_t> *packets_received{nullptr};     ///< 指向全局收包计数
            std::atomic<uint64_t> *packets_filtered{nullptr};     ///< 指向全局过滤计数
            std::atomic<uint64_t> *bytes_received{nullptr};       ///< 指向全局字节计数
            std::atomic<uint64_t> *recv_errors{nullptr};          ///< 指向全局错误计数
            std::atomic<uint64_t> *affinity_failures{nullptr};    ///< 指向亲和性失败计数
            std::atomic<uint64_t> *socket_bind_failures{nullptr}; ///< 指向绑定失败计数
        };

        /**
         * @class ArrayFaceReceiver
         * @brief 单阵面 UDP 高性能接收器
         *
         * 生命周期：
         *   构造 → start()（创建 socket + 启动线程）→ receive_loop()（阻塞收包）
         *   → stop()（设 running_=false → join 线程 → close socket）→ 析构
         */
        class ArrayFaceReceiver
        {
        public:
            /// 报文处理回调：(buffer, length, timestamp_ns, array_id, priority)
            using PacketHandler = std::function<void(PacketBuffer &&, size_t, uint64_t, uint8_t, protocol::PacketPriority)>;
            /// 抓包钩子：(raw_data, length, timestamp_us)
            using CaptureHook = std::function<void(const uint8_t *, size_t, uint64_t)>;
            /// 抓包钩子提供者：返回当前有效的 CaptureHook 的 shared_ptr
            using CaptureHookProvider = std::function<std::shared_ptr<CaptureHook>()>;
            struct RuntimeStats
            {
                uint8_t array_id{0};
                bool affinity_verified{false};
                PacketPool::Statistics packet_pool{};
            };

            /**
             * @brief 构造单阵面接收器
             *
             * @param config                阵面配置（IP/端口/CPU亲和性/性能参数）
             * @param packet_handler        报文处理回调（每个有效报文回调一次）
             * @param capture_hook_provider 抓包钩子提供者（通过 UdpReceiver 的 atomic shared_ptr 获取）
             * @param stats_sink            统计指针集合（指向共享的 atomic 计数器）
             *
             * @note 构造时会预创建 PacketPool（大小 = batch_size * 4）。
             */
            ArrayFaceReceiver(const ArrayFaceReceiverConfig &config,
                              PacketHandler packet_handler,
                              CaptureHookProvider capture_hook_provider,
                              ArrayFaceStatsSink stats_sink);

            /// 析构：自动调用 stop()
            ~ArrayFaceReceiver();

            ArrayFaceReceiver(const ArrayFaceReceiver &) = delete;            ///< 禁止拷贝
            ArrayFaceReceiver &operator=(const ArrayFaceReceiver &) = delete; ///< 禁止赋值

            /**
             * @brief 启动接收器：创建 socket 并启动收包线程
             * @return 是否启动成功（socket 创建 + bind 成功）
             */
            bool start();

            /**
             * @brief 停止接收器：设 running=false → join 线程 → close socket
             */
            void stop();

            /// 查询 CPU 亲和性是否验证通过
            bool affinity_verified() const { return affinity_verified_.load(std::memory_order_acquire); }

            uint8_t array_id() const { return config_.array_id; }        ///< 获取阵面编号
            uint16_t listen_port() const { return config_.listen_port; } ///< 获取监听端口
            RuntimeStats get_runtime_stats() const;

        private:
            /**
             * @brief 初始化 UDP socket
             *
             * 创建 SOCK_DGRAM → 设置 SO_RCVBUF → 设置 SO_RCVTIMEO (100ms)
             * → 启用 IP_RECVTOS（Linux）→ bind 到指定 IP:Port
             *
             * @return socket fd（成功）/ -1（失败）
             */
            int initialize_socket();

            /**
             * @brief 主收包循环（在 worker_ 线程中执行）
             *
             * Linux 路径使用 recvmmsg 批量收包，非 Linux 路径使用 recvfrom 逐包接收。
             * 每个批次的处理流程：
             * 1. 从 PacketPool 分配接收缓冲区
             * 2. recvmmsg / recvfrom 系统调用
             * 3. 对每个包：源 ID 过滤 → 打时间戳 → DSCP 优先级分类 → 抓包钩子
             * 4. 按优先级分桶（heartbeat_batch / data_batch）
             * 5. 先投递心跳包，再投递数据包
             * 6. 回收未使用的缓冲区
             */
            void receive_loop();

            /**
             * @brief 线程入口：设置 CPU 亲和性后进入 receive_loop
             */
            void run();

            /**
             * @brief 将当前线程绑定到配置指定的 CPU 核心和 NUMA 节点
             * @return 是否绑定并验证成功
             */
            bool bind_current_thread_to_cpu();

            ArrayFaceReceiverConfig config_;            ///< 阵面配置
            PacketHandler packet_handler_;              ///< 报文处理回调
            CaptureHookProvider capture_hook_provider_; ///< 抓包钩子提供者
            ArrayFaceStatsSink stats_sink_;             ///< 统计指针集合

            std::atomic<bool> running_{false};           ///< 运行状态（控制收包循环退出）
            std::atomic<bool> affinity_verified_{false}; ///< CPU 亲和性验证结果
            int sockfd_{-1};                             ///< UDP socket 文件描述符
            std::thread worker_;                         ///< 收包工作线程
            std::unique_ptr<PacketPool> packet_pool_;    ///< 零拷贝报文缓冲池
        };

    } // namespace network
} // namespace receiver

#endif // RECEIVER_NETWORK_ARRAY_FACE_RECEIVER_H
