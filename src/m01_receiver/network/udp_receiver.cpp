/**
 * @file udp_receiver.cpp
 * @brief UdpReceiver 门面类的实现——管理 ArrayFaceReceiver 集合
 *
 * 本文件实现了 UdpReceiver 的完整生命周期：
 * - 构造时保存配置和回调，初始化 capture_hook
 * - start()  解析阵面绑定 → 创建 ArrayFaceReceiver → 逐一启动
 * - stop()   逐一停止 ArrayFaceReceiver → 清空列表
 * - set_capture_hook()  运行时热替换抓包钩子
 *
 * Pimpl 模式：
 *   UdpReceiver::Impl 仅持有 vector<unique_ptr<ArrayFaceReceiver>>，
 *   将 ArrayFaceReceiver 的头文件依赖隐藏在 .cpp 中，减少编译时耦合。
 */

#include "qdgz300/m01_receiver/network/udp_receiver.h"

#include "qdgz300/m01_receiver/monitoring/logger.h"
#include "qdgz300/m01_receiver/network/array_face_receiver.h" // ArrayFaceReceiver 完整定义

#include <algorithm>
#include <memory>
#include <utility>

namespace receiver
{
    namespace network
    {
        /**
         * @class UdpReceiver::Impl
         * @brief Pimpl 内部类——持有各阵面接收器实例
         *
         * 使用 unique_ptr 管理 ArrayFaceReceiver 的生命周期。
         * start() 时填充，stop() 时清空。
         */
        class UdpReceiver::Impl
        {
        public:
            std::vector<std::unique_ptr<ArrayFaceReceiver>> array_receivers;
        };

        /**
         * @brief 构造函数：保存配置并初始化 capture_hook
         *
         * 如果配置中提供了 capture_hook，立即用 shared_ptr 包装以支持
         * 后续的 atomic load/store 热替换语义。
         */
        UdpReceiver::UdpReceiver(const UdpReceiverConfig &config, PacketCallback callback)
            : config_(config), callback_(std::move(callback)), impl_(std::make_unique<Impl>())
        {
            if (config_.capture_hook)
            {
                std::atomic_store_explicit(
                    &capture_hook_,
                    std::make_shared<CaptureHook>(std::move(config_.capture_hook)),
                    std::memory_order_release);
            }
        }

        UdpReceiver::~UdpReceiver()
        {
            stop(); // 确保析构时停止所有线程
        }

        /**
         * @brief 解析阵面绑定列表
         *
         * 逻辑：
         * - 如果配置中 array_faces 非空 → 直接使用（三阵面 / 多阵面模式）
         * - 否则 → 生成一个默认的单阵面绑定（向后兼容遗留配置）
         */
        std::vector<ArrayFaceBinding> UdpReceiver::resolve_bindings() const
        {
            if (!config_.array_faces.empty())
            {
                return config_.array_faces;
            }

            // 遗留单阵面回退：使用顶层 listen_port + cpu_affinity_start
            ArrayFaceBinding legacy{};
            legacy.array_id = 1;
            legacy.listen_port = config_.listen_port;
            legacy.cpu_affinity = std::max(0, config_.cpu_affinity_start);
            legacy.source_filter_enabled = false; // 单面模式不过滤源 ID
            legacy.expected_source_id = 0;
            return {legacy};
        }

        /**
         * @brief 启动所有阵面接收器
         *
         * 流程：
         * 1. CAS 设置 running_ = true（幂等：已 running 则直接返回）
         * 2. resolve_bindings() 得到绑定列表
         * 3. 遍历绑定列表，为每个阵面创建 ArrayFaceReceiver：
         *    - 拷贝阵面特定参数（bind_ip / cpu_affinity / source_id）
         *    - 拷贝全局参数（recv_batch_size / socket_rcvbuf_mb / numa_node）
         *    - 注册 packet_handler lambda：将底层 buffer 包装成 ReceivedPacket
         *      后转交给 callback_
         *    - 注册 capture_hook_provider lambda：返回当前的 capture_hook_ shared_ptr
         *    - 传入 stats_sink（指向共享的 atomic 计数器）
         * 4. 依次调用 ArrayFaceReceiver::start()
         * 5. 任一失败则回滚（stop 所有已启动的）并返回 false
         */
        bool UdpReceiver::start()
        {
            // 原子 CAS：保证幂等启动
            if (running_.exchange(true, std::memory_order_acq_rel))
            {
                return true; // 已在运行
            }

            const std::vector<ArrayFaceBinding> bindings = resolve_bindings();
            if (bindings.empty())
            {
                running_.store(false, std::memory_order_release);
                return false;
            }

            impl_->array_receivers.clear();
            impl_->array_receivers.reserve(bindings.size());

            bool started_all = true;
            for (const ArrayFaceBinding &binding : bindings)
            {
                // ── 组装阵面级配置 ──────────────────────────────────
                ArrayFaceReceiverConfig face_config{};
                face_config.array_id = binding.array_id;
                face_config.expected_source_id = binding.expected_source_id;
                face_config.bind_ip = binding.bind_ip; // 每个阵面独立的 IP 地址
                face_config.listen_port = binding.listen_port;
                face_config.cpu_affinity = binding.cpu_affinity;
                face_config.source_filter_enabled = binding.source_filter_enabled;
                face_config.recv_batch_size = config_.recv_batch_size; // 全局共享参数
                face_config.socket_rcvbuf_mb = config_.socket_rcvbuf_mb;
                face_config.enable_ip_freebind = config_.enable_ip_freebind;
                face_config.numa_node = config_.numa_node;
                face_config.prefetch_hints_enabled = config_.prefetch_hints_enabled;

                // ── 组装统计指针（所有阵面共享同一组 atomic 计数器） ─
                ArrayFaceStatsSink stats_sink{};
                stats_sink.packets_received = &stats_.packets_received;
                stats_sink.packets_filtered = &stats_.packets_filtered;
                stats_sink.bytes_received = &stats_.bytes_received;
                stats_sink.recv_errors = &stats_.recv_errors;
                stats_sink.affinity_failures = &stats_.affinity_failures;
                stats_sink.socket_bind_failures = &stats_.socket_bind_failures;

                // ── 创建 ArrayFaceReceiver ──────────────────────────
                auto receiver = std::make_unique<ArrayFaceReceiver>(
                    face_config,
                    // packet_handler: 将底层 raw buffer 包装为 ReceivedPacket 后回调上层
                    [this](PacketBuffer &&packet_data, size_t length, uint64_t receive_ts_ns, uint8_t array_id, protocol::PacketPriority priority)
                    {
                        if (!callback_)
                        {
                            return;
                        }
                        ReceivedPacket packet{};
                        packet.data = std::move(packet_data);
                        packet.length = length;
                        packet.receive_timestamp_ns = receive_ts_ns;
                        packet.array_id = array_id;
                        packet.priority = priority;
                        callback_(std::move(packet));
                    },
                    // capture_hook_provider: 返回当前 capture_hook 的 shared_ptr 快照
                    [this]()
                    {
                        return std::atomic_load_explicit(&capture_hook_, std::memory_order_acquire);
                    },
                    stats_sink);

                if (!receiver->start())
                {
                    LOG_ERROR("UdpReceiver failed to start array face=%u bind=%s:%u cpu=%d source_filter=%s expected_source_id=0x%02X",
                              static_cast<unsigned>(binding.array_id),
                              binding.bind_ip.c_str(),
                              static_cast<unsigned>(binding.listen_port),
                              binding.cpu_affinity,
                              binding.source_filter_enabled ? "true" : "false",
                              static_cast<unsigned>(binding.expected_source_id));
                    started_all = false;
                    break; // 启动失败，终止后续阵面的创建
                }
                impl_->array_receivers.push_back(std::move(receiver));
            }

            if (!started_all)
            {
                stop(); // 回滚：停止并释放所有已启动的阵面
                return false;
            }
            return true;
        }

        /**
         * @brief 停止所有阵面接收器
         *
         * CAS 设置 running_ = false 后逐一 stop 各 ArrayFaceReceiver
         * （等待收包线程 join + 关闭 socket），最后清空列表。
         */
        void UdpReceiver::stop()
        {
            if (!running_.exchange(false, std::memory_order_acq_rel))
            {
                return; // 已处于停止状态
            }

            for (auto &receiver : impl_->array_receivers)
            {
                if (receiver)
                {
                    receiver->stop();
                }
            }
            impl_->array_receivers.clear();
        }

        /**
         * @brief 运行时热替换抓包钩子
         *
         * 使用 atomic_store + shared_ptr 实现无锁替换：
         * - 传入有效 hook → 包装为 shared_ptr 并发布
         * - 传入空 hook → 发布空 shared_ptr（卸载钩子）
         *
         * 收包线程通过 capture_hook_provider lambda 做 atomic_load
         * 获取最新的钩子快照，因此替换操作对收包线程即时生效且无需加锁。
         */
        void UdpReceiver::set_capture_hook(CaptureHook hook)
        {
            if (hook)
            {
                std::atomic_store_explicit(
                    &capture_hook_,
                    std::make_shared<CaptureHook>(std::move(hook)),
                    std::memory_order_release);
                return;
            }
            // 卸载钩子
            std::atomic_store_explicit(&capture_hook_, std::shared_ptr<CaptureHook>{}, std::memory_order_release);
        }

    } // namespace network
} // namespace receiver
