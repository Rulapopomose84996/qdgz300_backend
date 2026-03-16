/**
 * @file app_run.cpp
 * @brief M01 接收器主事件循环 & 优雅关闭的实现
 *
 * 本文件包含两个核心函数：
 *
 * 1. **app_run()**      — 主事件循环（100ms 周期）
 *    - 信号处理（SIGINT / SIGTERM / SIGHUP）
 *    - 配置热加载（SIGHUP → ConfigManager::reload()）
 *    - 分片重组 / 重排序的超时扫描
 *    - 增量指标上报（将单调递增的累加器转换为周期增量）
 *    - 系统级监控采集（每 5 秒）
 *
 * 2. **app_shutdown()**  — 优雅关闭（3 秒 deadline）
 *    - 按依赖逆序停止组件
 *    - drain 管线残留数据（flush Reassembler → Reorderer → Delivery）
 *    - 输出最终统计日志
 *
 * @note 匿名命名空间中的辅助函数仅本编译单元可见。
 */

#include "qdgz300/m01_receiver/app_run.h"

// ── 项目内部依赖 ────────────────────────────────────────────────────
#include "qdgz300/m01_receiver/config/config_manager.h"   // ConfigManager 单例 + 热加载回调
#include "qdgz300/m01_receiver/monitoring/logger.h"       // Logger 单例 + LOG_XXX 宏
#include "qdgz300/m01_receiver/monitoring/metrics.h"      // MetricsCollector 单例
#include "qdgz300/m01_receiver/pipeline/rx_stage.h"       // RxStage (SPSC 队列接入)
#include "qdgz300/m01_receiver/protocol/protocol_types.h" // PacketType / CommonHeader
#include "qdgz300/m01_receiver/signal_handler.h"          // g_running / g_reload_requested / 信号标志

// ── 标准库 ──────────────────────────────────────────────────────────
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <thread>

// =============================================================================
// 匿名命名空间：文件内部辅助函数
// =============================================================================
namespace
{
    /**
     * @brief 将日志级别字符串转换为枚举（同 app_init.cpp 中的版本）
     * @note 由于位于匿名命名空间，不会与 app_init.cpp 中的同名函数冲突。
     */
    receiver::monitoring::LogLevel parse_log_level(const std::string &level)
    {
        if (level == "DEBUG")
            return receiver::monitoring::LogLevel::DEBUG;
        if (level == "WARN")
            return receiver::monitoring::LogLevel::WARN;
        if (level == "ERROR")
            return receiver::monitoring::LogLevel::ERROR;
        return receiver::monitoring::LogLevel::INFO;
    }

    /**
     * @brief 根据 Capture 配置节构造 PcapWriterConfig
     *
     * 用于配置热加载时重新创建 PcapWriter 实例。
     *
     * @param cfg YAML 配置中的 capture 子节
     * @return 对应的 PcapWriterConfig 值对象
     */
    receiver::capture::PcapWriterConfig make_capture_config(const receiver::config::ReceiverConfig::Capture &cfg)
    {
        receiver::capture::PcapWriterConfig capture_cfg;
        capture_cfg.enabled = cfg.enabled;
        capture_cfg.spool_dir = cfg.spool_dir;
        capture_cfg.archive_dir = cfg.archive_dir;
        capture_cfg.max_file_size_mb = cfg.max_file_size_mb;
        capture_cfg.max_files = cfg.max_files;
        capture_cfg.filter_packet_types = cfg.filter_packet_types;
        capture_cfg.filter_source_ids = cfg.filter_source_ids;
        return capture_cfg;
    }

    /**
     * @brief 将协议层 PacketType 映射为监控层 PacketTypeIndex
     */
    receiver::monitoring::PacketTypeIndex packet_type_index(uint8_t packet_type)
    {
        switch (static_cast<receiver::protocol::PacketType>(packet_type))
        {
        case receiver::protocol::PacketType::DATA:
            return receiver::monitoring::PacketTypeIndex::DATA;
        case receiver::protocol::PacketType::HEARTBEAT:
            return receiver::monitoring::PacketTypeIndex::HEARTBEAT;
        default:
            return receiver::monitoring::PacketTypeIndex::UNKNOWN;
        }
    }

    /**
     * @brief 在配置热加载时动态启用/禁用/重配置 PCAP 抓包
     *
     * 三种场景：
     * 1. capture.enabled = false → 停止旧 writer + 卸载 capture_hook
     * 2. capture.enabled = true  → 创建新 writer + 挂载 capture_hook
     * 3. 参数变更（output_dir、max_file_size_mb 等）→ 替换 writer 实例
     *
     * @param ctx   应用上下文（持有 udp_receiver 和 pcap_writer）
     * @param cfg   新配置中的 capture 子节
     */
    void apply_capture_runtime_config(receiver::AppContext &ctx,
                                      const receiver::config::ReceiverConfig::Capture &cfg)
    {
        if (!ctx.udp_receiver)
        {
            return;
        }

        if (!cfg.enabled)
        {
            // 抓包已关闭：卸载 hook + 停止并释放旧 writer
            ctx.udp_receiver->set_capture_hook({});
            auto old_writer = std::atomic_exchange_explicit(&ctx.pcap_writer,
                                                            std::shared_ptr<receiver::capture::PcapWriter>{},
                                                            std::memory_order_acq_rel);
            if (old_writer)
            {
                old_writer->stop();
                const auto stats = old_writer->get_statistics();
                LOG_INFO("Capture disabled by config reload: enqueued=%llu written=%llu queue_dropped=%llu write_errors=%llu rotated=%llu last_sealed=%s",
                         static_cast<unsigned long long>(stats.enqueued_packets),
                         static_cast<unsigned long long>(stats.written_packets),
                         static_cast<unsigned long long>(stats.dropped_queue_full),
                         static_cast<unsigned long long>(stats.write_errors),
                         static_cast<unsigned long long>(stats.rotated_files),
                         old_writer->last_sealed_file().c_str());
            }
            return;
        }

        // 抓包已启用 / 参数变更：创建新 writer
        auto new_writer = std::make_shared<receiver::capture::PcapWriter>(make_capture_config(cfg));
        if (!new_writer->start())
        {
            LOG_WARN("Capture enabled in reload but failed to start writer");
            return;
        }

        // 原子地发布新 writer（替换旧实例）
        std::atomic_store_explicit(&ctx.pcap_writer, std::move(new_writer), std::memory_order_release);
        // 重新挂载 capture_hook lambda（闭包引用 ctx.pcap_writer）
        ctx.udp_receiver->set_capture_hook([&ctx](const uint8_t *raw_data, size_t length, uint64_t timestamp_us)
                                           {
                                               auto writer = std::atomic_load_explicit(&ctx.pcap_writer, std::memory_order_acquire);
                                               if (writer)
                                               {
                                                   writer->write_packet(raw_data, length, timestamp_us);
                                               } });
        LOG_INFO("Capture enabled/reconfigured by config reload: dir=%s max_file_size_mb=%u max_files=%u",
                 cfg.spool_dir.c_str(),
                 static_cast<unsigned>(cfg.max_file_size_mb),
                 static_cast<unsigned>(cfg.max_files));
    }

    /**
     * @brief 注册配置热加载回调
     *
     * 当 ConfigManager::reload() 成功后，会调用此回调来动态调整运行时参数：
     * - 日志级别
     * - 重组超时时间
     * - 重排序超时时间
     * - PCAP 抓包配置
     *
     * @param ctx 应用上下文（通过引用捕获到 lambda 闭包中）
     */
    void register_reload_callback(receiver::AppContext &ctx)
    {
        receiver::config::ConfigManager::instance().register_reload_callback(
            [&](const receiver::config::ReceiverConfig &new_config)
            {
                auto &logger = receiver::monitoring::Logger::instance();

                // 动态调整日志级别
                logger.set_level(parse_log_level(new_config.logging.level));

                // 动态调整管线超时参数（所有阵面）
                for (auto &fp : ctx.face_pipelines)
                {
                    fp.reassembler->set_timeout_ms(new_config.reassembly.timeout_ms);
                    fp.reorderer->set_timeout_ms(new_config.reorder.timeout_ms);
                }

                // 动态调整抓包配置
                apply_capture_runtime_config(ctx, new_config.capture);

                LOG_INFO("Config reloaded: log_level=%s reasm_timeout_ms=%u reorder_timeout_ms=%u",
                         new_config.logging.level.c_str(),
                         static_cast<unsigned>(new_config.reassembly.timeout_ms),
                         static_cast<unsigned>(new_config.reorder.timeout_ms));
            });
    }

    /**
     * @brief 格式化监听地址字符串（用于日志输出）
     *
     * @param network_cfg 网络配置节
     * @return 格式化的监听地址字符串
     *   - 单阵面："10.0.0.1:9999"
     *   - 三阵面："[ArrayFace#0] 10.0.0.1:9999 | [ArrayFace#1] 10.0.0.2:9999 | [ArrayFace#2] 10.0.0.3:9999"
     */
    std::string format_listen_config(const receiver::config::ReceiverConfig::Network &network_cfg)
    {
        if (network_cfg.bind_ips.empty())
        {
            // 单阵面模式：显示 IP:Port
            return network_cfg.bind_ip + ":" + std::to_string(network_cfg.listen_port);
        }

        // 三阵面模式：显示多个 IP:Port（带阵面编号）
        std::ostringstream oss;
        for (size_t i = 0; i < network_cfg.bind_ips.size(); ++i)
        {
            if (i != 0)
            {
                oss << " | ";
            }
            oss << "[ArrayFace#" << i << "] "
                << network_cfg.bind_ips[i] << ":" << network_cfg.listen_port;
        }
        return oss.str();
    }
} // namespace

namespace receiver
{
    int app_run(AppContext &ctx)
    {
        auto &config_mgr = config::ConfigManager::instance();
        auto &metrics = monitoring::MetricsCollector::instance();
        const auto &config = config_mgr.get_config();

        // ── DRY-RUN 模式：仅打印配置摘要后退出 ──────────────────────
        if (ctx.dry_run)
        {
            const std::string listen_config = format_listen_config(config.network);
            LOG_INFO("DRY RUN config=%s listen=%s delivery=%s metrics=%s:%u",
                     ctx.config_file.c_str(),
                     listen_config.c_str(),
                     config.delivery.method.c_str(),
                     config.monitoring.metrics_bind_ip.c_str(),
                     static_cast<unsigned>(config.monitoring.metrics_port));
            std::cout << "DRY RUN OK" << std::endl;
            return 0;
        }

        // ══════════════════════════════════════════════════════════════
        // 正式启动阶段
        // ══════════════════════════════════════════════════════════════
        register_reload_callback(ctx);    // 注册 SIGHUP 配置热加载回调
        reset_signal_flags();             // 清空历史信号标志
        install_signal_handlers();        // 安装 SIGINT/SIGTERM/SIGHUP 处理函数
        metrics.set_heartbeat_state(1);   // heartbeat_state=1 表示 "正在启动"
        metrics.collect_system_metrics(); // 采集一次系统级基线指标

        // 启动 Prometheus HTTP 端点（暴露 /metrics 供 Grafana 抓取）
        if (!metrics.start())
        {
            LOG_ERROR("Failed to start metrics endpoint");
            return 1;
        }

        // 启动 UDP 接收器（创建 socket + 启动各阵面收包线程）
        if (!ctx.udp_receiver->start())
        {
            LOG_ERROR("Failed to start UDP receiver");
            return 1;
        }

        // ── 启动后续处理层线程（每阵面一个，从 SPSC 队列消费） ───────
        // 处理线程不绑定 CPU 16-18，由 OS 调度到其他可用核心
        ctx.processing_running.store(true, std::memory_order_release);
        for (auto &fp : ctx.face_pipelines)
        {
            ctx.processing_threads.emplace_back(
                [&fp, &ctx]()
                {
                    auto &metrics = monitoring::MetricsCollector::instance();

                    while (ctx.processing_running.load(std::memory_order_acquire))
                    {
                        auto env_opt = fp.rx_stage->queue().try_pop();
                        if (!env_opt.has_value())
                        {
                            // 队列为空，短暂让出 CPU
                            std::this_thread::sleep_for(std::chrono::microseconds(10));
                            continue;
                        }

                        pipeline::RxEnvelope &env = env_opt.value();

                        // 从信封重建 ParsedPacket（零拷贝，仅设指针）
                        protocol::ParsedPacket pkt;
                        pkt.header = env.header;
                        pkt.payload = env.packet_data.get() + protocol::COMMON_HEADER_SIZE;
                        pkt.total_size = env.packet_length;

                        // 指标上报
                        metrics.increment_pipeline_packets_entered();
                        METRICS_INC_PACKETS_RECEIVED(1);
                        metrics.increment_packets_received_by_type(
                            packet_type_index(env.header.packet_type), 1);
                        metrics.increment_bytes_received(env.packet_length);
                        metrics.increment_pipeline_parse_ok();
                        metrics.increment_pipeline_validate_ok();

                        // 类型分发 → 重组 → 重排（零拷贝：PacketBuffer 所有权随 dispatch 传递）
                        fp.dispatcher->dispatch(pkt, std::move(env.packet_data));

                        // 数据延迟计算
                        if (env.header.timestamp > 0)
                        {
                            const auto now_ms = static_cast<uint64_t>(
                                std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count());
                            if (now_ms >= env.header.timestamp)
                            {
                                const double delay_seconds = static_cast<double>(now_ms - env.header.timestamp) / 1000.0;
                                metrics.observe_data_delay(delay_seconds);
                            }
                        }
                    }

                    // 退出前 drain SPSC 队列中剩余数据
                    while (auto remaining = fp.rx_stage->queue().try_pop())
                    {
                        protocol::ParsedPacket pkt;
                        pkt.header = remaining->header;
                        pkt.payload = remaining->packet_data.get() + protocol::COMMON_HEADER_SIZE;
                        pkt.total_size = remaining->packet_length;
                        fp.dispatcher->dispatch(pkt);
                    }
                });
        }

        for (auto &sc : ctx.stub_consumers)
        {
            if (!sc->start())
            {
                LOG_ERROR("Failed to start StubConsumer");
                for (auto &started_sc : ctx.stub_consumers)
                {
                    if (started_sc)
                    {
                        started_sc->stop();
                    }
                }
                ctx.processing_running.store(false, std::memory_order_release);
                for (auto &t : ctx.processing_threads)
                {
                    if (t.joinable())
                    {
                        t.join();
                    }
                }
                ctx.processing_threads.clear();
                ctx.udp_receiver->stop();
                return 1;
            }
        }

        ctx.started = true;             // 标记已成功启动（供 app_shutdown 判断）
        metrics.set_heartbeat_state(2); // heartbeat_state=2 表示 "运行中"
        const std::string listen_config = format_listen_config(config.network);
        LOG_INFO("Receiver started: %s", listen_config.c_str());
        auto last_system_metrics = std::chrono::steady_clock::now();

        // ══════════════════════════════════════════════════════════════
        // 主事件循环（100ms 周期轮询）
        //
        // g_running 是 atomic<bool>，由信号处理函数在 SIGINT/SIGTERM 时
        // 通过 release store 设为 false，此处 acquire load 读取。
        // ══════════════════════════════════════════════════════════════
        while (g_running.load(std::memory_order_acquire))
        {
            // ── 1. 信号计数器增量上报 ────────────────────────────────
            // 信号处理函数在另一个线程中递增原子计数器，
            // 此处计算自上次检查以来的增量并上报到 Prometheus。
            const uint64_t now_sigint = g_sigint_received.load(std::memory_order_relaxed);
            if (now_sigint > ctx.last_sigint_received)
            {
                metrics.increment_signal_received("SIGINT", now_sigint - ctx.last_sigint_received);
                ctx.last_sigint_received = now_sigint;
            }

            const uint64_t now_sigterm = g_sigterm_received.load(std::memory_order_relaxed);
            if (now_sigterm > ctx.last_sigterm_received)
            {
                metrics.increment_signal_received("SIGTERM", now_sigterm - ctx.last_sigterm_received);
                ctx.last_sigterm_received = now_sigterm;
            }

            const uint64_t now_sighup = g_sighup_received.load(std::memory_order_relaxed);
            if (now_sighup > ctx.last_sighup_received)
            {
                metrics.increment_signal_received("SIGHUP", now_sighup - ctx.last_sighup_received);
                ctx.last_sighup_received = now_sighup;
            }

            // ── 2. 配置热加载（SIGHUP 触发） ────────────────────────
            // exchange(false) 原子地读取并清除 reload 标志
            if (g_reload_requested.exchange(false, std::memory_order_acq_rel))
            {
                if (!config_mgr.reload())
                {
                    LOG_ERROR("Config reload failed");
                }
                else
                {
                    metrics.increment_config_reloads();
                }
            }

            // ── 3. 管线超时扫描（所有阵面） ────────────────────────────
            for (auto &fp : ctx.face_pipelines)
            {
                fp.reassembler->check_timeouts();
                fp.reorderer->check_timeout();
            }

            // ── 4. 增量指标上报（聚合所有阵面） ────────────────────────
            // 策略：各子系统维护单调递增的原子计数器，此处读取当前值，
            //       与上次记录的高水位做差值，得到本周期增量并上报。
            //       所有 load 使用 relaxed 内存序（仅需要数值一致性，不需要同步）。
            uint64_t total_active_contexts = 0;
            uint64_t total_missing_fragments = 0;

            for (auto &fp : ctx.face_pipelines)
            {
                const auto &reasm_stats = fp.reassembler->get_statistics();

                // 4a. Reassembler 活跃上下文数（created - destroyed）
                total_active_contexts +=
                    reasm_stats.contexts_created.load(std::memory_order_relaxed) -
                    reasm_stats.contexts_destroyed.load(std::memory_order_relaxed);
                total_missing_fragments +=
                    reasm_stats.total_missing_fragments.load(std::memory_order_relaxed);

                // 4b. Reassembler 迟到分片
                const uint64_t now_reasm_late = reasm_stats.late_fragments.load(std::memory_order_relaxed);
                if (now_reasm_late > fp.last_reasm_late_fragments)
                {
                    metrics.increment_packets_dropped_by_reason(
                        monitoring::DropReasonIndex::LATE_FRAGMENT,
                        now_reasm_late - fp.last_reasm_late_fragments);
                    fp.last_reasm_late_fragments = now_reasm_late;
                }

                // 4c. Reassembler 重复分片
                const uint64_t now_reasm_dup = reasm_stats.duplicate_fragments.load(std::memory_order_relaxed);
                if (now_reasm_dup > fp.last_reasm_duplicate_fragments)
                {
                    metrics.increment_packets_dropped_by_reason(
                        monitoring::DropReasonIndex::REASM_DUPLICATE_FRAG,
                        now_reasm_dup - fp.last_reasm_duplicate_fragments);
                    fp.last_reasm_duplicate_fragments = now_reasm_dup;
                }

                // 4d. Reassembler 超时未完成帧
                const uint64_t now_reasm_incomplete = reasm_stats.frames_incomplete.load(std::memory_order_relaxed);
                if (now_reasm_incomplete > fp.last_reasm_frames_incomplete)
                {
                    metrics.increment_packets_dropped_by_reason(
                        monitoring::DropReasonIndex::REASM_TIMEOUT,
                        now_reasm_incomplete - fp.last_reasm_frames_incomplete);
                    fp.last_reasm_frames_incomplete = now_reasm_incomplete;
                }

                // 4e. Reassembler 上下文池溢出
                const uint64_t now_reasm_ctx_overflow = reasm_stats.contexts_overflow.load(std::memory_order_relaxed);
                if (now_reasm_ctx_overflow > fp.last_reasm_contexts_overflow)
                {
                    metrics.increment_packets_dropped_by_reason(
                        monitoring::DropReasonIndex::MAX_CONTEXTS_EXCEEDED,
                        now_reasm_ctx_overflow - fp.last_reasm_contexts_overflow);
                    fp.last_reasm_contexts_overflow = now_reasm_ctx_overflow;
                }

                // 4f. Reassembler 单 key 字节溢出
                const uint64_t now_reasm_bytes_overflow = reasm_stats.reasm_bytes_overflow.load(std::memory_order_relaxed);
                if (now_reasm_bytes_overflow > fp.last_reasm_bytes_overflow)
                {
                    metrics.increment_packets_dropped_by_reason(
                        monitoring::DropReasonIndex::REASM_BYTES_OVERFLOW,
                        now_reasm_bytes_overflow - fp.last_reasm_bytes_overflow);
                    fp.last_reasm_bytes_overflow = now_reasm_bytes_overflow;
                }

                // 4g. Reorderer 序号重复包
                const auto reorder_stats = fp.reorderer->get_statistics();
                const uint64_t now_reorder_dup = reorder_stats.packets_duplicate;
                if (now_reorder_dup > fp.last_reorder_duplicates)
                {
                    metrics.increment_packets_dropped_by_reason(
                        monitoring::DropReasonIndex::SEQUENCE_DUPLICATE,
                        now_reorder_dup - fp.last_reorder_duplicates);
                    fp.last_reorder_duplicates = now_reorder_dup;
                }

                // 4h. Reorderer 超时零填充包
                const uint64_t now_reorder_zero = reorder_stats.packets_zero_filled;
                if (now_reorder_zero > fp.last_reorder_zero_filled)
                {
                    metrics.increment_packets_dropped_by_reason(
                        monitoring::DropReasonIndex::TIMEOUT,
                        now_reorder_zero - fp.last_reorder_zero_filled);
                    fp.last_reorder_zero_filled = now_reorder_zero;
                }
            }

            // 设置聚合后的 gauge 指标
            metrics.set_active_reorder_contexts(static_cast<size_t>(total_active_contexts));
            metrics.set_missing_fragments_total(total_missing_fragments);

            // ── 4i. RxStage SPSC 队列深度（每阵面） ──────────────────
            for (const auto &fp : ctx.face_pipelines)
            {
                if (fp.rx_stage)
                {
                    const auto rx_stats = fp.rx_stage->get_stats();
                    const auto queue_depth = fp.rx_stage->queue().size();
                    // 队列深度和 drop 计数可用于运维监控和性能调优
                    // 后续可将这些指标导出到 Prometheus
                    (void)rx_stats;
                    (void)queue_depth;
                }
            }

            // ── 5. 系统级指标采集（每 5 秒一次） ─────────────────────
            const auto now = std::chrono::steady_clock::now();
            if (now - last_system_metrics >= std::chrono::seconds(5))
            {
                metrics.collect_system_metrics();   // CPU / 内存 / 网络接口等
                metrics.increment_heartbeat_sent(); // 记录一次心跳发送
                last_system_metrics = now;
            }

            // ── 6. 让出 CPU 避免空转 ─────────────────────────────────
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        return 0;
    }

    /**
     * @brief 优雅关闭实现
     *
     * 关闭策略：
     * - 总超时 3 秒，每个阶段完成后检查是否已超时
     * - 若任一阶段超时，调用 std::_Exit(1) 强制退出（避免死锁或无限等待）
     *
     * 关闭顺序（按数据流逆序）：
     *   UdpReceiver → PcapWriter → Reassembler → Reorderer → Delivery → Metrics
     */
    int app_shutdown(AppContext &ctx)
    {
        if (!ctx.started)
        {
            return 0; // 主循环从未启动，无需清理
        }

        const auto shutdown_start = std::chrono::steady_clock::now();
        const auto shutdown_deadline = shutdown_start + std::chrono::seconds(3);
        // 超时检查 lambda：若当前时间超过 deadline 则强制退出
        const auto enforce_shutdown_timeout = [&](const char *stage)
        {
            if (std::chrono::steady_clock::now() > shutdown_deadline)
            {
                LOG_ERROR("Shutdown timeout exceeded at stage=%s, forcing exit", stage);
                std::_Exit(1);
            }
        };

        // ── Stage 0: 停止 RawBlock 消费者（在管线 stop 之前） ────────
        for (auto &sc : ctx.stub_consumers)
        {
            if (sc)
            {
                sc->stop();
            }
        }
        enforce_shutdown_timeout("stub_consumers.stop");

        // ── Stage 1: 停止网络接收（不再有新包进入管线） ──────────────
        ctx.udp_receiver->stop();
        enforce_shutdown_timeout("udp_receiver.stop");

        // ── Stage 1.5: 停止后续处理层线程 ───────────────────────────
        // 设置 processing_running=false 后 join 所有处理线程。
        // 处理线程在退出前会 drain SPSC 队列中的剩余数据。
        ctx.processing_running.store(false, std::memory_order_release);
        for (auto &t : ctx.processing_threads)
        {
            if (t.joinable())
            {
                t.join();
            }
        }
        ctx.processing_threads.clear();
        enforce_shutdown_timeout("processing_threads.join");

        // 打印 RxStage 统计信息
        for (auto &fp : ctx.face_pipelines)
        {
            if (fp.rx_stage)
            {
                auto stats = fp.rx_stage->get_stats();
                LOG_INFO("RxStage[face=%u]: rx_total=%llu parse_ok=%llu validate_ok=%llu enqueued=%llu queue_drops=%llu",
                         static_cast<unsigned>(fp.array_id),
                         static_cast<unsigned long long>(stats.rx_total),
                         static_cast<unsigned long long>(stats.parse_ok),
                         static_cast<unsigned long long>(stats.validate_ok),
                         static_cast<unsigned long long>(stats.enqueued),
                         static_cast<unsigned long long>(stats.queue_drops));
            }
        }

        // ── Stage 2: 停止抓包写入（刷写文件缓冲区） ────────────────
        auto pcap_writer = std::atomic_exchange_explicit(&ctx.pcap_writer,
                                                         std::shared_ptr<capture::PcapWriter>{},
                                                         std::memory_order_acq_rel);
        if (pcap_writer)
        {
            pcap_writer->stop();
            const auto stats = pcap_writer->get_statistics();
            LOG_INFO("Capture spool summary: enqueued=%llu written=%llu queue_dropped=%llu write_errors=%llu rotated=%llu last_sealed=%s",
                     static_cast<unsigned long long>(stats.enqueued_packets),
                     static_cast<unsigned long long>(stats.written_packets),
                     static_cast<unsigned long long>(stats.dropped_queue_full),
                     static_cast<unsigned long long>(stats.write_errors),
                     static_cast<unsigned long long>(stats.rotated_files),
                     pcap_writer->last_sealed_file().c_str());
        }
        enforce_shutdown_timeout("pcap_writer.stop");

        // ── Stage 3: 强制完成所有未完成的重组上下文 ─────────────────
        size_t forced_contexts = 0;
        for (auto &fp : ctx.face_pipelines)
        {
            forced_contexts += fp.reassembler->flush_all();
        }
        enforce_shutdown_timeout("reassembler.flush_all");

        // ── Stage 4: 释放重排窗口中的所有缓存包 ─────────────────────
        size_t drained_packets = 0;
        for (auto &fp : ctx.face_pipelines)
        {
            drained_packets += fp.reorderer->flush();
        }
        enforce_shutdown_timeout("reorderer.flush");

        // ── Stage 5: 确保投递层缓冲区清空 ───────────────────────────
        if (ctx.delivery_interface)
        {
            ctx.delivery_interface->flush();
        }
        enforce_shutdown_timeout("delivery.flush");

        // ── Stage 6: 关闭 Prometheus HTTP 端点 ──────────────────────
        monitoring::MetricsCollector::instance().stop();
        monitoring::MetricsCollector::instance().set_heartbeat_state(0); // heartbeat_state=0 表示 "已停止"
        enforce_shutdown_timeout("metrics.stop");

        // ── 打印关闭摘要和最终统计 ──────────────────────────────────
        LOG_INFO("Shutdown flush summary: forced_contexts=%llu drained_packets=%llu",
                 static_cast<unsigned long long>(forced_contexts),
                 static_cast<unsigned long long>(drained_packets));

        // 打印 UDP 报文接收的最终统计信息（用于运维排查）
        const auto &udp_stats = ctx.udp_receiver->get_statistics();
        LOG_INFO("Final stats: packets_received=%llu filtered=%llu bytes_received=%llu errors=%llu affinity_failures=%llu bind_failures=%llu",
                 static_cast<unsigned long long>(udp_stats.packets_received.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(udp_stats.packets_filtered.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(udp_stats.bytes_received.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(udp_stats.recv_errors.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(udp_stats.affinity_failures.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(udp_stats.socket_bind_failures.load(std::memory_order_relaxed)));

        ctx.started = false; // 标记已关闭
        return 0;
    }
} // namespace receiver
