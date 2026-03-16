/**
 * @file array_face_receiver.cpp
 * @brief 单阵面高性能 UDP 接收器实现——socket 初始化、CPU 绑定、recvmmsg 批量收包
 *
 * 本文件是整个接收系统的最底层实现，直接与操作系统内核交互：
 *
 * 核心系统调用：
 * - socket(AF_INET, SOCK_DGRAM)  创建 UDP socket
 * - setsockopt(SO_RCVBUF)         设置内核接收缓冲区（默认 256MB）
 * - setsockopt(SO_RCVTIMEO)       设置接收超时（100ms，避免 recvmmsg 永久阻塞）
 * - setsockopt(IP_RECVTOS)        启用 TOS 字段接收（用于 DSCP 优先级分类）
 * - bind()                         绑定到指定 IP:Port
 * - recvmmsg()                     Linux 特有的批量接收（单次最多收 64 个包）
 * - pthread_setaffinity_np()       绑定线程到指定 CPU 核心
 * - numa_run_on_node()             绑定到 NUMA 节点（可选，需 libnuma）
 *
 * 性能关键路径 (receive_loop) 的设计：
 * 1. 从 PacketPool 批量预分配缓冲区
 * 2. 一次 recvmmsg 收取多个报文
 * 3. 对每个报文：prefetch 下一个 → 源过滤 → 时间戳 → DSCP 分类 → 抓包
 * 4. 按优先级分桶（heartbeat 先于 data 投递）
 * 5. 回收未使用的缓冲区（归还 PacketPool）
 *
 * @see ArrayFaceReceiver   头文件中的类声明和接口文档
 * @see UdpReceiver          管理多个 ArrayFaceReceiver 的门面
 */

#include "qdgz300/m01_receiver/network/array_face_receiver.h"
#include "qdgz300/m01_receiver/monitoring/logger.h"
#include "qdgz300/m01_receiver/monitoring/metrics.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <vector>

// ── Linux 系统头文件（socket / 网络 / 线程亲和性） ──────────────────
#include <arpa/inet.h>  // inet_pton
#include <cerrno>       // errno
#include <netinet/in.h> // sockaddr_in / IPPROTO_IP / IP_TOS
#include <pthread.h>    // pthread_setaffinity_np / pthread_getaffinity_np
#include <sched.h>      // cpu_set_t / CPU_ZERO / CPU_SET / CPU_ISSET
#include <sys/socket.h> // socket / bind / setsockopt / recvmmsg
#include <sys/time.h>   // timeval（SO_RCVTIMEO 参数）
#include <unistd.h>     // close
#if defined(RECEIVER_HAS_LIBNUMA)
#include <numa.h> // numa_run_on_node / numa_set_preferred
#endif

namespace receiver
{
    namespace network
    {
        // =================================================================
        // 匿名命名空间：文件内部辅助工具（仅本编译单元可见）
        // =================================================================
        namespace
        {
            constexpr size_t kPacketBufferSize = 65535;

            /**
             * @brief 发出 CPU 缓存预取提示
             *
             * 告诉 CPU "即将读取 ptr 指向的内存"，减少后续访问的缓存缺失延迟。
             * - 参数 0: 读取意图（非写入）
             * - 参数 3: 最高时间局部性（数据会被多次访问）
             *
             * @param ptr 即将访问的内存地址
             */
            inline void prefetch_read(const void *ptr)
            {
#if defined(__clang__) || defined(__GNUC__)
                __builtin_prefetch(ptr, 0, 3);
#else
                (void)ptr; // 非 GCC/Clang 编译器忽略（无实际开销）
#endif
            }

            /**
             * @brief 计算实际使用的 recvmmsg 批量大小
             *
             * 确保批量大小 ≥ 1 且 ≤ 64（Linux recvmmsg 的实际上限）。
             *
             * @param config 阵面配置
             * @return 调整后的批量大小
             */
            inline size_t effective_batch_size(const ArrayFaceReceiverConfig &config)
            {
                constexpr size_t kDefaultBatch = 1;
                size_t batch = std::max(kDefaultBatch, config.recv_batch_size);
#if defined(__linux__)
                constexpr size_t kMaxLinuxRecvmmsgBatch = 64;
                batch = std::min(batch, kMaxLinuxRecvmmsgBatch);
#endif
                return batch;
            }

            /**
             * @brief 安全地递增一个 atomic 计数器（空指针安全）
             *
             * 使用 relaxed 内存序，因为统计计数器只需要最终一致性，
             * 不需要与其他操作建立 happens-before 关系。
             *
             * @param counter 指向 atomic 计数器的指针（可为 nullptr）
             * @param delta   增量值（默认 1）
             */
            inline void atomic_increment(std::atomic<uint64_t> *counter, uint64_t delta = 1)
            {
                if (counter != nullptr)
                {
                    counter->fetch_add(delta, std::memory_order_relaxed);
                }
            }

#if defined(__linux__)
            /**
             * @brief 从 recvmmsg 的控制消息（cmsg）中提取 DSCP 值
             *
             * IP 头的 TOS 字段格式：[DSCP 6bit][ECN 2bit]
             * 右移 2 位即得到 DSCP 值。
             *
             * 本函数遍历 cmsg 链表查找 IP_TOS 类型的控制消息。
             *
             * @param hdr recvmmsg 返回的 msghdr 结构
             * @return DSCP 值（0~63）；未找到则返回 0
             */
            inline uint8_t extract_dscp_from_cmsg(const msghdr &hdr)
            {
                msghdr *mutable_hdr = const_cast<msghdr *>(&hdr);
                for (cmsghdr *cmsg = CMSG_FIRSTHDR(mutable_hdr); cmsg != nullptr; cmsg = CMSG_NXTHDR(mutable_hdr, cmsg))
                {
                    if (cmsg->cmsg_level == IPPROTO_IP && cmsg->cmsg_type == IP_TOS)
                    {
                        const auto *tos_ptr = reinterpret_cast<const uint8_t *>(CMSG_DATA(cmsg));
                        return static_cast<uint8_t>((*tos_ptr) >> 2); // TOS[7:2] = DSCP
                    }
                }
                return 0;
            }
#endif

            /**
             * @brief 根据 DSCP 值和报文类型判断优先级
             *
             * 优先级判定规则（优先级从高到低）：
             * 1. DSCP = 48 (CS6) → HIGH（心跳包通常使用 CS6 标记）
             * 2. packet_type == HEARTBEAT → HIGH
             * 3. 其他 → NORMAL
             *
             * @param buffer  报文原始数据指针
             * @param length  报文长度
             * @param dscp    从 IP 头提取的 DSCP 值
             * @return 报文优先级
             */
            inline protocol::PacketPriority classify_priority(const uint8_t *buffer, size_t length, uint8_t dscp)
            {
                constexpr uint8_t kHeartbeatDscpCs6 = 48; // DSCP CS6 = 48
                if (dscp == kHeartbeatDscpCs6)
                {
                    return protocol::PacketPriority::HIGH;
                }
                // 回退检查：直接读取协议头中的 packet_type 字段
                if (length > offsetof(protocol::CommonHeader, packet_type) && buffer != nullptr &&
                    buffer[offsetof(protocol::CommonHeader, packet_type)] == static_cast<uint8_t>(protocol::PacketType::HEARTBEAT))
                {
                    return protocol::PacketPriority::HIGH;
                }
                return protocol::PacketPriority::NORMAL;
            }
        } // namespace

        /**
         * @brief 构造函数：保存配置并预创建缓冲池
         *
         * PacketPool 按最小批次和目标预算共同决定缓冲区数量，
         * 确保在突发流量和 drain 多轮场景下有足够的预分配内存可用。
         */
        ArrayFaceReceiver::ArrayFaceReceiver(const ArrayFaceReceiverConfig &config,
                                             PacketHandler packet_handler,
                                             CaptureHookProvider capture_hook_provider,
                                             ArrayFaceStatsSink stats_sink)
            : config_(config),
              packet_handler_(std::move(packet_handler)),
              capture_hook_provider_(std::move(capture_hook_provider)),
              stats_sink_(stats_sink)
        {
            const size_t min_pool_size = effective_batch_size(config_) * std::max<size_t>(4, config_.recv_drain_rounds);
            const size_t pool_bytes = config_.packet_pool_mb * 1024u * 1024u;
            const size_t budget_pool_size = std::max<size_t>(1, (pool_bytes + (kPacketBufferSize - 1)) / kPacketBufferSize);
            const size_t pool_size = std::max(min_pool_size, budget_pool_size);
            packet_pool_ = std::make_unique<PacketPool>(kPacketBufferSize, pool_size, config_.numa_node);
        }

        ArrayFaceReceiver::~ArrayFaceReceiver()
        {
            stop(); // 确保析构时线程已 join、socket 已关闭
        }

        /**
         * @brief 启动接收器：创建 socket + 启动收包线程
         *
         * 使用 CAS 保证幂等：如果已经 running，直接返回 true。
         */
        bool ArrayFaceReceiver::start()
        {
            if (running_.exchange(true, std::memory_order_acq_rel))
            {
                return true; // 已在运行，幂等返回
            }

            // 创建并绑定 socket
            sockfd_ = initialize_socket();
            if (sockfd_ < 0)
            {
                running_.store(false, std::memory_order_release);
                atomic_increment(stats_sink_.socket_bind_failures);
                atomic_increment(stats_sink_.recv_errors);
                return false;
            }

            // 启动收包线程（run() → bind_cpu → receive_loop）
            worker_ = std::thread(&ArrayFaceReceiver::run, this);
            return true;
        }

        /**
         * @brief 停止接收器：等待 worker 线程退出并关闭 socket
         *
         * 设置 running_ = false 后，receive_loop 中的 recvmmsg 会在
         * SO_RCVTIMEO (100ms) 超时后检查 running_ 并退出循环。
         */
        void ArrayFaceReceiver::stop()
        {
            if (!running_.exchange(false, std::memory_order_acq_rel))
            {
                return; // 已处于停止状态
            }

            if (worker_.joinable())
            {
                worker_.join(); // 等待收包线程退出
            }

            if (sockfd_ >= 0)
            {
                close(sockfd_); // 关闭 socket（释放端口）
                sockfd_ = -1;
            }
        }

        /**
         * @brief 初始化 UDP socket
         *
         * 步骤：
         * 1. 创建 AF_INET + SOCK_DGRAM (UDP) socket
         * 2. 设置 SO_RCVBUF（内核接收缓冲区，默认 256MB）
         *    - 大缓冲区可吸收突发流量，防止在管线处理延迟时丢包
         * 3. 设置 SO_RCVTIMEO = 100ms
         *    - 使 recvmmsg 不会无限阻塞，每 100ms 可检查 running_ 标志
         * 4. 启用 IP_RECVTOS（仅 Linux）
         *    - 使 recvmmsg 返回 IP TOS 字段，用于 DSCP 优先级分类
         * 5. bind 到指定 IP:Port
         *
         * @return socket fd（成功），-1（失败且已 close）
         */
        int ArrayFaceReceiver::initialize_socket()
        {
            int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
            if (sockfd < 0)
            {
                LOG_ERROR("ArrayFaceReceiver[%u] socket() failed for %s:%u errno=%d (%s)",
                          static_cast<unsigned>(config_.array_id),
                          config_.bind_ip.c_str(),
                          static_cast<unsigned>(config_.listen_port),
                          errno,
                          std::strerror(errno));
                return -1;
            }

            // 设置内核接收缓冲区大小（MB → bytes）
            const int rcvbuf = static_cast<int>(config_.socket_rcvbuf_mb * 1024 * 1024);
            if (setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) != 0)
            {
                const int err = errno;
                close(sockfd);
                LOG_ERROR("ArrayFaceReceiver[%u] setsockopt(SO_RCVBUF=%d) failed for %s:%u errno=%d (%s)",
                          static_cast<unsigned>(config_.array_id),
                          rcvbuf,
                          config_.bind_ip.c_str(),
                          static_cast<unsigned>(config_.listen_port),
                          err,
                          std::strerror(err));
                return -1;
            }

#if defined(__linux__) && defined(IP_FREEBIND)
            if (config_.enable_ip_freebind)
            {
                int freebind = 1;
                if (setsockopt(sockfd, IPPROTO_IP, IP_FREEBIND, &freebind, sizeof(freebind)) != 0)
                {
                    const int err = errno;
                    close(sockfd);
                    LOG_ERROR("ArrayFaceReceiver[%u] setsockopt(IP_FREEBIND) failed for %s:%u errno=%d (%s)",
                              static_cast<unsigned>(config_.array_id),
                              config_.bind_ip.c_str(),
                              static_cast<unsigned>(config_.listen_port),
                              err,
                              std::strerror(err));
                    return -1;
                }
            }
#endif

            // 设置接收超时 100ms（使 recvmmsg 不会永久阻塞）
            timeval rcv_timeout{};
            rcv_timeout.tv_sec = 0;
            rcv_timeout.tv_usec = 100000; // 100ms
            if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &rcv_timeout, sizeof(rcv_timeout)) != 0)
            {
                const int err = errno;
                close(sockfd);
                LOG_ERROR("ArrayFaceReceiver[%u] setsockopt(SO_RCVTIMEO) failed for %s:%u errno=%d (%s)",
                          static_cast<unsigned>(config_.array_id),
                          config_.bind_ip.c_str(),
                          static_cast<unsigned>(config_.listen_port),
                          err,
                          std::strerror(err));
                return -1;
            }

#if defined(__linux__)
            // 启用 IP TOS 辅助数据（recvmmsg 会在 cmsg 中返回 TOS 字段）
            int recv_tos = 1;
            (void)setsockopt(sockfd, IPPROTO_IP, IP_RECVTOS, &recv_tos, sizeof(recv_tos));
#endif

            // 绑定到指定 IP:Port
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(config_.listen_port);
            if (inet_pton(AF_INET, config_.bind_ip.c_str(), &addr.sin_addr) != 1)
            {
                close(sockfd);
                LOG_ERROR("ArrayFaceReceiver[%u] invalid bind_ip=%s",
                          static_cast<unsigned>(config_.array_id),
                          config_.bind_ip.c_str());
                return -1; // IP 地址格式无效
            }

            if (bind(sockfd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
            {
                const int err = errno;
                close(sockfd);
                LOG_ERROR("ArrayFaceReceiver[%u] bind() failed for %s:%u errno=%d (%s)",
                          static_cast<unsigned>(config_.array_id),
                          config_.bind_ip.c_str(),
                          static_cast<unsigned>(config_.listen_port),
                          err,
                          std::strerror(err));
                return -1; // 端口已被占用或权限不足
            }

            return sockfd;
        }

        /**
         * @brief 将当前线程绑定到指定 CPU 核心和 NUMA 节点
         *
         * NUMA 绑定（可选，需编译时定义 RECEIVER_HAS_LIBNUMA）：
         *   numa_run_on_node()    — 限制线程只在该节点的核心上运行
         *   numa_set_preferred()  — 优先在该节点分配内存
         *
         * CPU 亲和性绑定：
         *   pthread_setaffinity_np() — 将线程限制在单个核心上运行
         *   pthread_getaffinity_np() — 验证设置是否生效
         *
         * @return true = 绑定并验证成功；false = 设置或验证失败
         */
        bool ArrayFaceReceiver::bind_current_thread_to_cpu()
        {
#if defined(__linux__)
#if defined(RECEIVER_HAS_LIBNUMA)
            // 绑定到 NUMA 节点（减少跨节点内存访问延迟）
            if (numa_available() != -1)
            {
                (void)numa_run_on_node(config_.numa_node);
                numa_set_preferred(config_.numa_node);
            }
#endif

            // 设置 CPU 亲和性：仅允许在 config_.cpu_affinity 核心上运行
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);
            CPU_SET(config_.cpu_affinity, &cpuset);

            if (pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) != 0)
            {
                return false;
            }

            // 验证：读回实际的亲和性掩码，确认目标核心在集合中
            cpu_set_t actual;
            CPU_ZERO(&actual);
            if (pthread_getaffinity_np(pthread_self(), sizeof(actual), &actual) != 0)
            {
                return false;
            }
            return CPU_ISSET(config_.cpu_affinity, &actual) != 0;
#else
            return true; // 非 Linux 平台跳过亲和性设置
#endif
        }

        /**
         * @brief 收包线程入口函数
         *
         * 执行顺序：
         * 1. 绑定 CPU/NUMA 亲和性
         * 2. 若绑定失败则记录错误到统计计数器
         * 3. 进入 receive_loop 阻塞收包
         */
        void ArrayFaceReceiver::run()
        {
            const bool affinity_ok = bind_current_thread_to_cpu();
            affinity_verified_.store(affinity_ok, std::memory_order_release);
            if (!affinity_ok)
            {
                atomic_increment(stats_sink_.affinity_failures);
                atomic_increment(stats_sink_.recv_errors);
            }

            receive_loop(); // 阻塞直到 running_ 变为 false
        }

        /**
         * @brief 主收包循环——高性能 recvmmsg 批量接收的核心实现
         *
         * 本函数是整个接收器性能最敏感的部分（热路径），设计目标：
         * - 最小化系统调用次数（recvmmsg 一次收取多个包）
         * - 零拷贝（PacketPool 预分配缓冲区，报文数据不做内存拷贝）
         * - 缓存友好（prefetch 下一个报文 + CPU 亲和性）
         * - QoS 优先级保证（心跳包优先于数据包投递）
         *
         * Linux 路径和非 Linux 路径分别实现（#if defined(__linux__)）：
         * - Linux：使用 recvmmsg + mmsghdr + cmsg（批量收包 + DSCP 提取）
         * - 其他：使用 recvfrom 逐包接收（无优先级分类）
         */
        void ArrayFaceReceiver::receive_loop()
        {
            auto &metrics = monitoring::MetricsCollector::instance();
            const size_t batch_size = effective_batch_size(config_);
            const size_t drain_rounds = std::max<size_t>(1, config_.recv_drain_rounds);

            // =================================================================
            // Linux 路径：recvmmsg 批量收包
            // =================================================================
#if defined(__linux__)
            // ── 预分配 per-batch 工作缓冲区 ─────────────────────────
            std::vector<mmsghdr> msg_headers(batch_size);             // recvmmsg 的消息头数组
            std::vector<iovec> iovecs(batch_size);                    // scatter/gather IO 向量
            std::vector<uint8_t *> recv_buffers(batch_size, nullptr); // 从 PacketPool 分配的接收缓冲区
            // 控制消息缓冲区（用于接收 IP_TOS 辅助数据）
            std::vector<std::array<char, CMSG_SPACE(sizeof(uint8_t))>> control_buffers(batch_size);

            // ── P1-6 优化：一次性初始化 iovecs 和 msg_headers 的固定字段 ──
            // iovecs 的 iov_len 固定为 65535，msg_headers 的 msg_iovlen 固定为 1，
            // 只需初始化一次，每批次仅更新 iov_base 和清零 msg_len / controllen。
            for (size_t i = 0; i < batch_size; ++i)
            {
                std::memset(&msg_headers[i], 0, sizeof(msg_headers[i]));
                std::memset(&iovecs[i], 0, sizeof(iovecs[i]));
                iovecs[i].iov_len = 65535;
                msg_headers[i].msg_hdr.msg_iov = &iovecs[i];
                msg_headers[i].msg_hdr.msg_iovlen = 1;
                msg_headers[i].msg_hdr.msg_control = control_buffers[i].data();
                msg_headers[i].msg_hdr.msg_controllen = static_cast<socklen_t>(control_buffers[i].size());
            }

            // ── P1-3 优化：预分配心跳/数据分桶 vector，循环内仅 clear() 复用 ──
            struct PendingPacket
            {
                PacketBuffer packet_buffer;                                          ///< 零拷贝缓冲区
                size_t packet_len{0};                                                ///< 有效数据长度
                uint64_t receive_timestamp_ns{0};                                    ///< 接收时间戳
                protocol::PacketPriority priority{protocol::PacketPriority::NORMAL}; ///< 优先级
            };
            std::vector<PendingPacket> heartbeat_batch;
            std::vector<PendingPacket> data_batch;
            heartbeat_batch.reserve(batch_size);
            data_batch.reserve(batch_size);

            // ── 主循环：持续接收直到 running_ 变为 false ────────────
            while (running_.load(std::memory_order_acquire))
            {
                for (size_t drain_round = 0; drain_round < drain_rounds &&
                                            running_.load(std::memory_order_acquire);
                     ++drain_round)
                {
                    // ── Phase 1: 从 PacketPool 批量分配接收缓冲区 ───────
                    bool allocation_failed = false;
                    for (size_t i = 0; i < batch_size; ++i)
                    {
                        // P1-6 优化：仅更新每批次变化的字段，不做全量 memset
                        msg_headers[i].msg_len = 0;
                        msg_headers[i].msg_hdr.msg_controllen = static_cast<socklen_t>(control_buffers[i].size());
                        msg_headers[i].msg_hdr.msg_flags = 0;

                        recv_buffers[i] = packet_pool_->allocate();
                        if (recv_buffers[i] == nullptr)
                        {
                            atomic_increment(stats_sink_.recv_errors);
                            metrics.increment_socket_receive_errors();
                            allocation_failed = true;
                            break;
                        }

                        iovecs[i].iov_base = recv_buffers[i];
                    }

                    if (allocation_failed)
                    {
                        for (uint8_t *buffer : recv_buffers)
                        {
                            if (buffer != nullptr)
                            {
                                packet_pool_->deallocate(buffer);
                            }
                        }
                        break;
                    }

                    const int recv_flags = (drain_round == 0) ? 0 : MSG_DONTWAIT;
                    const int received = recvmmsg(
                        sockfd_,
                        msg_headers.data(),
                        static_cast<unsigned int>(batch_size),
                        recv_flags,
                        nullptr);

                    if (received <= 0)
                    {
                        for (uint8_t *buffer : recv_buffers)
                        {
                            if (buffer != nullptr)
                            {
                                packet_pool_->deallocate(buffer);
                            }
                        }

                        if (received < 0 && running_.load(std::memory_order_acquire) &&
                            errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
                        {
                            atomic_increment(stats_sink_.recv_errors);
                            metrics.increment_socket_receive_errors();
                        }

                        if (drain_round == 0)
                        {
                            break;
                        }
                        break;
                    }

                    metrics.increment_socket_receive_batches();

                    // ── P1-4 优化：整个批次共享一个时间戳 ───────────────
                    const uint64_t batch_timestamp_ns = static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count());

                    const auto capture_hook =
                        capture_hook_provider_ ? capture_hook_provider_() : std::shared_ptr<CaptureHook>{};

                    heartbeat_batch.clear();
                    data_batch.clear();

                    for (int i = 0; i < received; ++i)
                    {
                        const size_t next_index = static_cast<size_t>(i + 1);
                        if (next_index < static_cast<size_t>(received) &&
                            recv_buffers[next_index] != nullptr &&
                            config_.prefetch_hints_enabled)
                        {
                            prefetch_read(recv_buffers[next_index]);
                        }

                        const size_t packet_len = static_cast<size_t>(msg_headers[static_cast<size_t>(i)].msg_len);
                        PacketBuffer packet_buffer(recv_buffers[static_cast<size_t>(i)], packet_pool_.get());
                        recv_buffers[static_cast<size_t>(i)] = nullptr;

                        metrics.increment_socket_packets_received();
                        metrics.increment_socket_bytes_received(static_cast<uint64_t>(packet_len));

                        if (config_.source_filter_enabled)
                        {
                            constexpr size_t kSourceOffset = offsetof(protocol::CommonHeader, source_id);
                            if (packet_len <= kSourceOffset || packet_buffer.get()[kSourceOffset] != config_.expected_source_id)
                            {
                                atomic_increment(stats_sink_.packets_filtered);
                                metrics.increment_socket_source_filtered();
                                continue;
                            }
                        }

                        const uint64_t receive_timestamp_ns = batch_timestamp_ns;
                        const uint8_t dscp = extract_dscp_from_cmsg(msg_headers[static_cast<size_t>(i)].msg_hdr);
                        const protocol::PacketPriority priority =
                            classify_priority(packet_buffer.get(), packet_len, dscp);

                        if (capture_hook)
                        {
                            (*capture_hook)(packet_buffer.get(), packet_len, receive_timestamp_ns / 1000ull);
                        }

                        atomic_increment(stats_sink_.packets_received);
                        atomic_increment(stats_sink_.bytes_received, static_cast<uint64_t>(packet_len));

                        PendingPacket pending{};
                        pending.packet_buffer = std::move(packet_buffer);
                        pending.packet_len = packet_len;
                        pending.receive_timestamp_ns = receive_timestamp_ns;
                        pending.priority = priority;
                        if (priority == protocol::PacketPriority::HIGH)
                        {
                            heartbeat_batch.push_back(std::move(pending));
                        }
                        else
                        {
                            data_batch.push_back(std::move(pending));
                        }
                    }

                    if (packet_handler_)
                    {
                        for (auto &pending : heartbeat_batch)
                        {
                            packet_handler_(std::move(pending.packet_buffer),
                                            pending.packet_len,
                                            pending.receive_timestamp_ns,
                                            config_.array_id,
                                            pending.priority);
                        }
                        for (auto &pending : data_batch)
                        {
                            packet_handler_(std::move(pending.packet_buffer),
                                            pending.packet_len,
                                            pending.receive_timestamp_ns,
                                            config_.array_id,
                                            pending.priority);
                        }
                    }

                    for (uint8_t *buffer : recv_buffers)
                    {
                        if (buffer != nullptr)
                        {
                            packet_pool_->deallocate(buffer);
                        }
                    }

                    if (received < static_cast<int>(batch_size))
                    {
                        break;
                    }
                }
            }

            // =================================================================
            // 非 Linux 路径：recvfrom 逐包接收（备用实现）
            // =================================================================
#else
            while (running_.load(std::memory_order_acquire))
            {
                // 每次分配一个缓冲区
                uint8_t *buffer = packet_pool_->allocate();
                if (buffer == nullptr)
                {
                    atomic_increment(stats_sink_.recv_errors);
                    continue;
                }

                // 逐包接收（阻塞，受 SO_RCVTIMEO 100ms 控制）
                const ssize_t bytes = recvfrom(sockfd_, buffer, 65535, 0, nullptr, nullptr);
                if (bytes <= 0)
                {
                    packet_pool_->deallocate(buffer);
                    if (bytes < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
                    {
                        atomic_increment(stats_sink_.recv_errors);
                        metrics.increment_socket_receive_errors();
                    }
                    continue;
                }

                const size_t packet_len = static_cast<size_t>(bytes);
                metrics.increment_socket_packets_received();
                metrics.increment_socket_bytes_received(static_cast<uint64_t>(packet_len));
                PacketBuffer packet_buffer(buffer, packet_pool_.get());

                // 源 ID 过滤
                if (config_.source_filter_enabled)
                {
                    constexpr size_t kSourceOffset = offsetof(protocol::CommonHeader, source_id);
                    if (packet_len <= kSourceOffset || packet_buffer.get()[kSourceOffset] != config_.expected_source_id)
                    {
                        atomic_increment(stats_sink_.packets_filtered);
                        metrics.increment_socket_source_filtered();
                        continue;
                    }
                }

                const uint64_t receive_timestamp_ns = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());

                // 抓包旁路
                const auto capture_hook =
                    capture_hook_provider_ ? capture_hook_provider_() : std::shared_ptr<CaptureHook>{};
                if (capture_hook)
                {
                    (*capture_hook)(packet_buffer.get(), packet_len, receive_timestamp_ns / 1000ull);
                }

                atomic_increment(stats_sink_.packets_received);
                atomic_increment(stats_sink_.bytes_received, static_cast<uint64_t>(packet_len));

                // 非 Linux 路径无 DSCP 信息，统一使用 NORMAL 优先级
                if (packet_handler_)
                {
                    packet_handler_(std::move(packet_buffer), packet_len, receive_timestamp_ns, config_.array_id, protocol::PacketPriority::NORMAL);
                }
            }
#endif
        }

        ArrayFaceReceiver::RuntimeStats ArrayFaceReceiver::get_runtime_stats() const
        {
            RuntimeStats stats{};
            stats.array_id = config_.array_id;
            stats.affinity_verified = affinity_verified_.load(std::memory_order_acquire);
            if (packet_pool_)
            {
                stats.packet_pool = packet_pool_->get_statistics();
            }
            return stats;
        }

    } // namespace network
} // namespace receiver
